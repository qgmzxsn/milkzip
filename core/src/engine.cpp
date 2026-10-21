/* Loads 7-Zip at runtime (dlopen / LoadLibrary). This file does not compile
   7-Zip into the binary. 7-Zip stays LGPL in its own shared library.
   RAR and RAR5 are opened for extraction only. Nothing here creates RAR. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <stdlib.h>

#if defined(_WIN32)
/* Windows headers already define GUID. Do not include 7-Zip's MyGuidDef.h first. */
#  include "Common/MyWindows.h"
#  define INITGUID
#  include <initguid.h>
#else
#  define INITGUID
#  include "Common/MyGuidDef.h"
#  include "Common/MyWindows.h"
#endif
#include "Common/Common0.h"
#include "7zip/IStream.h"
#include "7zip/IProgress.h"
#include "7zip/IPassword.h"
#include "7zip/Archive/IArchive.h"
#include "Common/MyCom.h"

#include "archiver.h"
#include "path_safety.hpp"
#include "safe_write.hpp"
#include "lz4_stream.hpp"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <dirent.h>
#  include <dlfcn.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

constexpr uint64_t kWriteCap = 64ull << 30;
constexpr uint32_t kMaxEntries = 1000000;
constexpr size_t kMaxSymlink = 4096;
/* Disguised archives are normally glued to the end of the carrier. Scanning
   only a tail window avoids reading an entire multi-gigabyte video. */
constexpr uint64_t kDisguiseScanWindow = 32ull << 20;
// Inner and explicitly opened carriers use the same detection window.

void wipe_bytes(void *data, size_t size) {
  volatile unsigned char *p = static_cast<volatile unsigned char *>(data);
  while (size--) *p++ = 0;
}

void wipe_string(std::string &s) {
  if (!s.empty()) wipe_bytes(&s[0], s.size());
  s.clear();
}

void wipe_wide(std::wstring &s) {
  if (!s.empty()) wipe_bytes(&s[0], s.size() * sizeof(wchar_t));
  s.clear();
}

struct PasswordState {
  std::wstring text;
  bool provided = false;
  bool asked = false;
  ~PasswordState() { wipe_wide(text); }
};

void utf8_append(std::string &out, uint32_t cp) {
  if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

std::string wide_to_utf8(const wchar_t *w, size_t n) {
  std::string out;
  for (size_t i = 0; i < n; ++i) {
    uint32_t cp = static_cast<uint32_t>(w[i]);
    /* 7-Zip can hand back UTF-16 code units even when wchar_t is 32-bit. */
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n) {
      uint32_t low = static_cast<uint32_t>(w[i + 1]);
      if (low >= 0xDC00 && low <= 0xDFFF) {
        cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
        ++i;
      }
    }
    utf8_append(out, cp);
  }
  return out;
}

std::wstring utf8_to_wide(const char *s) {
  std::wstring out;
  if (!s) return out;
  const unsigned char *p = reinterpret_cast<const unsigned char *>(s);
  while (*p) {
    uint32_t cp = 0;
    if (*p < 0x80) {
      cp = *p++;
    } else if ((*p & 0xE0) == 0xC0 && p[1]) {
      cp = ((*p & 0x1F) << 6) | (p[1] & 0x3F);
      p += 2;
    } else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
      cp = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
      p += 3;
    } else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
      cp = ((*p & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
      p += 4;
    } else {
      cp = *p++;
    }
    /* 7-Zip stores non-BMP characters as UTF-16 surrogates in wchar_t. */
    if (cp > 0xFFFF) {
      cp -= 0x10000;
      out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
      out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
    } else {
      out.push_back(static_cast<wchar_t>(cp));
    }
  }
  return out;
}

uint32_t bstr_bytes(BSTR b) {
  if (!b) return 0;
#if defined(_WIN32)
  return ::SysStringByteLen(b);
#else
  return *(reinterpret_cast<uint32_t *>(b) - 1);
#endif
}

BSTR alloc_wide(const wchar_t *s, size_t n) {
  if (n > 0x0fffffffu / sizeof(wchar_t)) return nullptr;
#if defined(_WIN32)
  return ::SysAllocStringLen(s, static_cast<UINT>(n));
#else
  uint32_t bytes = static_cast<uint32_t>(n * sizeof(wchar_t));
  void *mem = malloc(bytes + sizeof(uint32_t) + sizeof(wchar_t));
  if (!mem) return nullptr;
  *static_cast<uint32_t *>(mem) = bytes;
  wchar_t *b = reinterpret_cast<wchar_t *>(static_cast<uint32_t *>(mem) + 1);
  if (n) memcpy(b, s, bytes);
  b[n] = 0;
  return b;
#endif
}

void free_bstr(BSTR b) {
  if (!b) return;
#if defined(_WIN32)
  ::SysFreeString(b);
#else
  free(reinterpret_cast<uint32_t *>(b) - 1);
#endif
}

void prop_clear(PROPVARIANT *v) {
  if (v->vt == VT_BSTR) free_bstr(v->bstrVal);
  memset(v, 0, sizeof(*v));
}

bool bstr_utf8(BSTR b, std::string &out, bool &embedded_nul) {
  embedded_nul = false;
  out.clear();
  if (!b) return true;
  uint32_t chars = bstr_bytes(b) / static_cast<uint32_t>(sizeof(wchar_t));
  size_t slen = 0;
  while (slen < chars && b[slen] != 0) ++slen;
  if (slen != chars) embedded_nul = true;
  out = wide_to_utf8(b, slen);
  return true;
}

enum class Kind {
  Zip,
  SevenZ,
  Tar,
  TarGz,
  TarBz2,
  TarXz,
  TarZst,
  Rar,
  Gzip, Bzip2, Xz, Zstd, Lzma, Compress, Lz4, Cab, Iso, Udf, Wim, Arj, Lzh, Cpio, Ar, Rpm, Dmg, Xar,
  Split,
  Unsupported
};

std::string basename_lower(const std::string &path) {
  size_t slash = path.find_last_of("/\\");
  std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
  for (char &c : base) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return base;
}

bool ends_with(const std::string &s, const char *suf) {
  size_t n = strlen(suf);
  return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

bool all_digits(const std::string &s) {
  if (s.empty()) return false;
  for (char c : s)
    if (c < '0' || c > '9') return false;
  return true;
}

/* .7z.001 / .zip.001 / bare .001, pkzip .zNN, rar .rNN and .partN.rar.
   These open as archives. A plain file that only looks like a volume still
   has to parse. */
Kind split_kind(const std::string &base) {
  size_t dot = base.rfind('.');
  if (dot == std::string::npos || dot == 0) return Kind::Unsupported;
  std::string ext = base.substr(dot + 1);
  std::string stem = base.substr(0, dot);
  if (ext.size() >= 2 && ext.size() <= 4 && all_digits(ext)) {
    if (ends_with(stem, ".7z")) return Kind::SevenZ;
    if (ends_with(stem, ".zip")) return Kind::Zip;
    if (ends_with(stem, ".rar")) return Kind::Rar;
    return Kind::SevenZ;
  }
  if (ext.size() == 3 && ext[0] == 'z' && ext[1] >= '0' && ext[1] <= '9' && ext[2] >= '0' &&
      ext[2] <= '9') {
    return Kind::Zip;
  }
  if (ext.size() == 3 && ext[0] == 'r' && ext[1] >= '0' && ext[1] <= '9' && ext[2] >= '0' &&
      ext[2] <= '9') {
    return Kind::Rar;
  }
  if (ends_with(base, ".rar")) {
    size_t part = stem.rfind(".part");
    if (part != std::string::npos && all_digits(stem.substr(part + 5))) return Kind::Rar;
  }
  return Kind::Unsupported;
}

bool numeric_volume_name(const std::string &path) {
  std::string base = basename_lower(path);
  size_t dot = base.rfind('.');
  if (dot == std::string::npos) return false;
  std::string ext = base.substr(dot + 1);
  return ext.size() >= 2 && ext.size() <= 4 && all_digits(ext);
}

Kind classify(const std::string &path) {
  std::string base = basename_lower(path);
  Kind split = split_kind(base);
  if (split != Kind::Unsupported) return split;
  if (ends_with(base, ".tar.gz") || ends_with(base, ".tgz")) return Kind::TarGz;
  if (ends_with(base, ".tar.bz2") || ends_with(base, ".tbz2") || ends_with(base, ".tbz"))
    return Kind::TarBz2;
  if (ends_with(base, ".tar.xz") || ends_with(base, ".txz")) return Kind::TarXz;
  if (ends_with(base, ".tar.zst") || ends_with(base, ".tzst")) return Kind::TarZst;
  if (ends_with(base, ".tar")) return Kind::Tar;
  if (ends_with(base, ".7z")) return Kind::SevenZ;
  if (ends_with(base, ".zip")) return Kind::Zip;
  if (ends_with(base, ".rar")) return Kind::Rar;
  const std::pair<const char *, Kind> extra[] = {
      {".gz", Kind::Gzip}, {".bz2", Kind::Bzip2}, {".xz", Kind::Xz},
      {".zst", Kind::Zstd}, {".zstd", Kind::Zstd}, {".cab", Kind::Cab},
      {".lzma", Kind::Lzma}, {".z", Kind::Compress}, {".lz4", Kind::Lz4},
      {".zipx", Kind::Zip}, {".jar", Kind::Zip}, {".epub", Kind::Zip},
      {".docx", Kind::Zip}, {".xlsx", Kind::Zip}, {".pptx", Kind::Zip},
      {".odt", Kind::Zip}, {".ods", Kind::Zip}, {".odp", Kind::Zip}, {".apk", Kind::Zip},
      {".iso", Kind::Iso}, {".udf", Kind::Udf}, {".wim", Kind::Wim},
      {".swm", Kind::Wim}, {".arj", Kind::Arj}, {".lzh", Kind::Lzh},
      {".lha", Kind::Lzh}, {".cpio", Kind::Cpio}, {".ar", Kind::Ar},
      {".deb", Kind::Ar}, {".rpm", Kind::Rpm}, {".dmg", Kind::Dmg}, {".xar", Kind::Xar}};
  for (const auto &item : extra) if (ends_with(base, item.first)) return item.second;
  return Kind::Unsupported;
}

/* Media carriers, self-extracting executables, and content-detected custom
   suffixes such as .zipsc. An executable is only read here; it is never run. */
bool is_disguise_carrier(const std::string &path) {
  std::string base = basename_lower(path);
  return ends_with(base, ".mp4") || ends_with(base, ".m4v") || ends_with(base, ".mov") ||
         ends_with(base, ".jpg") || ends_with(base, ".jpeg") || ends_with(base, ".png") ||
         ends_with(base, ".gif") || ends_with(base, ".bmp") || ends_with(base, ".webp") ||
         ends_with(base, ".avi") || ends_with(base, ".mkv") || ends_with(base, ".webm") ||
         ends_with(base, ".exe") || ends_with(base, ".sfx") || ends_with(base, ".zipsc");
}

using CreateObjectFn = HRESULT(STDAPICALLTYPE *)(const GUID *, const GUID *, void **);
using GetNumberOfFormatsFn = HRESULT(STDAPICALLTYPE *)(UInt32 *);
using GetHandlerProperty2Fn = HRESULT(STDAPICALLTYPE *)(UInt32, PROPID, PROPVARIANT *);

struct EngineLib {
  void *handle = nullptr;
  CreateObjectFn CreateObject = nullptr;
  GetNumberOfFormatsFn GetNumberOfFormats = nullptr;
  GetHandlerProperty2Fn GetHandlerProperty2 = nullptr;

  ~EngineLib() { close(); }
  EngineLib() = default;
  EngineLib(const EngineLib &) = delete;
  EngineLib &operator=(const EngineLib &) = delete;

  void close() {
    if (!handle) return;
#if defined(_WIN32)
    ::FreeLibrary(static_cast<HMODULE>(handle));
#else
    ::dlclose(handle);
#endif
    handle = nullptr;
    CreateObject = nullptr;
    GetNumberOfFormats = nullptr;
    GetHandlerProperty2 = nullptr;
  }
};

struct ListedEntry {
  std::string path;
  bool is_dir = false;
  bool is_symlink = false;
  uint64_t size = 0;
  uint64_t packed_size = 0;
  int64_t modified_unix = -1;
};

struct VolumeContext {
  std::string directory;
  std::string archive_name;
  std::string missing;
  std::string next_absent;
};

struct ArchiverArchive {
  EngineLib engine;
  CMyComPtr<IInStream> stream;
  CMyComPtr<IInArchive> archive;
  CMyComPtr<IInArchive> owner_archive;
  CMyComPtr<IInStream> owner_stream;
  std::vector<ListedEntry> entries;
  std::string message;
  std::string temp_dir;
  std::string source_path;
  VolumeContext volumes;
  std::string fallback_name;
  bool native_lz4 = false;

  ~ArchiverArchive() {
    archive.Release();
    stream.Release();
    owner_archive.Release();
    owner_stream.Release();
    engine.close();
    if (!temp_dir.empty()) {
      std::string inner = temp_dir + "/inner.bin";
#if defined(_WIN32)
      std::wstring wide_inner = utf8_to_wide(inner.c_str());
      std::wstring wide_dir = utf8_to_wide(temp_dir.c_str());
      ::DeleteFileW(wide_inner.c_str());
      ::RemoveDirectoryW(wide_dir.c_str());
#else
      ::unlink(inner.c_str());
      ::rmdir(temp_dir.c_str());
#endif
      temp_dir.clear();
    }
  }
};

std::string library_dir() {
#if defined(_WIN32)
  HMODULE module = nullptr;
  if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&archiver_open), &module)) {
    return std::string();
  }
  wchar_t buf[MAX_PATH * 4];
  DWORD n = ::GetModuleFileNameW(module, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
  if (n == 0) return std::string();
  std::string path = wide_to_utf8(buf, n);
  size_t slash = path.find_last_of("/\\");
  if (slash == std::string::npos) return std::string();
  return path.substr(0, slash);
#else
  Dl_info info;
  memset(&info, 0, sizeof(info));
  if (::dladdr(reinterpret_cast<void *>(&archiver_open), &info) == 0 || !info.dli_fname) {
    return std::string();
  }
  std::string path = info.dli_fname;
  size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) return std::string();
  return path.substr(0, slash);
#endif
}

bool load_engine(EngineLib &lib, const char *engine_path, std::string &message) {
  std::string path;
  if (engine_path && engine_path[0]) {
    path = engine_path;
  } else if (const char *env = getenv("ARCHIVER_7ZIP")) {
    if (env[0]) path = env;
  }
  std::vector<std::string> tries;
  if (!path.empty()) tries.push_back(path);
  std::string dir = library_dir();
  if (!dir.empty()) {
    tries.push_back(dir + "/7z.so");
    tries.push_back(dir + "/7z.dll");
    tries.push_back(dir + "/lib7z.dylib");
  }
  for (const std::string &candidate : tries) {
#if defined(_WIN32)
    std::wstring wide = utf8_to_wide(candidate.c_str());
    HMODULE module = ::LoadLibraryW(wide.c_str());
    if (!module) continue;
    lib.handle = module;
    lib.CreateObject = reinterpret_cast<CreateObjectFn>(::GetProcAddress(module, "CreateObject"));
    lib.GetNumberOfFormats =
        reinterpret_cast<GetNumberOfFormatsFn>(::GetProcAddress(module, "GetNumberOfFormats"));
    lib.GetHandlerProperty2 =
        reinterpret_cast<GetHandlerProperty2Fn>(::GetProcAddress(module, "GetHandlerProperty2"));
#else
    void *module = ::dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!module) continue;
    lib.handle = module;
    lib.CreateObject = reinterpret_cast<CreateObjectFn>(::dlsym(module, "CreateObject"));
    lib.GetNumberOfFormats =
        reinterpret_cast<GetNumberOfFormatsFn>(::dlsym(module, "GetNumberOfFormats"));
    lib.GetHandlerProperty2 =
        reinterpret_cast<GetHandlerProperty2Fn>(::dlsym(module, "GetHandlerProperty2"));
#endif
    if (lib.CreateObject && lib.GetNumberOfFormats && lib.GetHandlerProperty2) return true;
    lib.close();
  }
  message = "7-Zip shared library was not found. Pass the path to 7z.so, 7z.dll, or lib7z.dylib.";
  return false;
}

bool iequals(const std::string &a, const char *b) {
  size_t n = strlen(b);
  if (a.size() != n) return false;
  for (size_t i = 0; i < n; ++i) {
    char c = a[i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    char d = b[i];
    if (d >= 'A' && d <= 'Z') d = static_cast<char>(d - 'A' + 'a');
    if (c != d) return false;
  }
  return true;
}

bool guid_from_prop(PROPVARIANT &prop, GUID &out) {
  if (prop.vt != VT_BSTR || !prop.bstrVal) return false;
  if (bstr_bytes(prop.bstrVal) < sizeof(GUID)) return false;
  memcpy(&out, prop.bstrVal, sizeof(GUID));
  return true;
}

bool find_handler(EngineLib &lib, const char *name, GUID &out) {
  UInt32 count = 0;
  if (lib.GetNumberOfFormats(&count) != S_OK) return false;
  for (UInt32 i = 0; i < count; ++i) {
    PROPVARIANT prop;
    memset(&prop, 0, sizeof(prop));
    if (lib.GetHandlerProperty2(i, NArchive::NHandlerPropID::kName, &prop) != S_OK) {
      prop_clear(&prop);
      continue;
    }
    std::string got;
    bool nul = false;
    bstr_utf8(prop.bstrVal, got, nul);
    prop_clear(&prop);
    if (!iequals(got, name)) continue;
    memset(&prop, 0, sizeof(prop));
    if (lib.GetHandlerProperty2(i, NArchive::NHandlerPropID::kClassID, &prop) != S_OK) {
      prop_clear(&prop);
      return false;
    }
    bool ok = guid_from_prop(prop, out);
    prop_clear(&prop);
    return ok;
  }
  return false;
}

class FileInStream Z7_final : public IInStream,
                              public IStreamGetSize,
                              public CMyUnknownImp {
  Z7_IFACES_IMP_UNK_3(IInStream, ISequentialInStream, IStreamGetSize)
 public:
#if defined(_WIN32)
  HANDLE handle = INVALID_HANDLE_VALUE;
  ~FileInStream() {
    if (handle != INVALID_HANDLE_VALUE) ::CloseHandle(handle);
  }
#else
  int fd = -1;
  ~FileInStream() {
    if (fd >= 0) ::close(fd);
  }
#endif
  /* Visible range inside the real file. A renamed or appended archive is
     opened as this slice so 7-Zip does not have to search the carrier. */
  uint64_t view_base = 0;
  uint64_t view_size = 0;
  uint64_t view_pos = 0;
  /* Current OS file position. Sequential reads keep it in sync so a read
     does not need an extra SetFilePointer/lseek call. */
  uint64_t phys_pos = 0;

  bool seek_phys(uint64_t pos) {
#if defined(_WIN32)
    LARGE_INTEGER move;
    move.QuadPart = static_cast<LONGLONG>(pos);
    if (!::SetFilePointerEx(handle, move, nullptr, FILE_BEGIN)) return false;
#else
    if (::lseek(fd, static_cast<off_t>(pos), SEEK_SET) < 0) return false;
#endif
    phys_pos = pos;
    return true;
  }
};

Z7_COM7F_IMF(FileInStream::Read(void *data, UInt32 size, UInt32 *processedSize)) {
  if (processedSize) *processedSize = 0;
  if (view_pos >= view_size) return S_OK;
  uint64_t remain = view_size - view_pos;
  uint32_t want = size;
  if (static_cast<uint64_t>(want) > remain) want = static_cast<uint32_t>(remain);
  if (want == 0) return S_OK;
  const uint64_t physical = view_base + view_pos;
  if (phys_pos != physical && !seek_phys(physical)) return E_FAIL;
#if defined(_WIN32)
  DWORD got = 0;
  if (!::ReadFile(handle, data, want, &got, nullptr)) return E_FAIL;
  view_pos += got;
  phys_pos += got;
  if (processedSize) *processedSize = static_cast<UInt32>(got);
#else
  ssize_t n = ::read(fd, data, want);
  if (n < 0) return E_FAIL;
  view_pos += static_cast<uint64_t>(n);
  phys_pos += static_cast<uint64_t>(n);
  if (processedSize) *processedSize = static_cast<UInt32>(n);
#endif
  return S_OK;
}

Z7_COM7F_IMF(FileInStream::Seek(Int64 offset, UInt32 seekOrigin, UInt64 *newPosition)) {
  int64_t next = 0;
  if (seekOrigin == 0) next = offset;
  else if (seekOrigin == 1) next = static_cast<int64_t>(view_pos) + offset;
  else if (seekOrigin == 2) next = static_cast<int64_t>(view_size) + offset;
  else return E_INVALIDARG;
  if (next < 0) return HRESULT_WIN32_ERROR_NEGATIVE_SEEK;
  view_pos = static_cast<uint64_t>(next);
  if (newPosition) *newPosition = view_pos;
  return S_OK;
}

Z7_COM7F_IMF(FileInStream::GetSize(UInt64 *size)) {
  *size = view_size;
  return S_OK;
}

class OpenCallback Z7_final : public IArchiveOpenCallback,
                              public IArchiveOpenVolumeCallback,
                              public ICryptoGetTextPassword,
                              public CMyUnknownImp {
  Z7_IFACES_IMP_UNK_3(IArchiveOpenCallback, IArchiveOpenVolumeCallback, ICryptoGetTextPassword)
 public:
  PasswordState *password = nullptr;
  VolumeContext *volumes = nullptr;
};

Z7_COM7F_IMF(OpenCallback::SetTotal(const UInt64 * /*files*/, const UInt64 * /*bytes*/)) {
  return S_OK;
}
Z7_COM7F_IMF(OpenCallback::SetCompleted(const UInt64 * /*files*/, const UInt64 * /*bytes*/)) {
  return S_OK;
}
Z7_COM7F_IMF(OpenCallback::CryptoGetTextPassword(BSTR *passwordOut)) {
  if (!password) return E_ABORT;
  password->asked = true;
  if (!password->provided) return E_ABORT;
  *passwordOut = alloc_wide(password->text.c_str(), password->text.size());
  return *passwordOut ? S_OK : E_OUTOFMEMORY;
}

struct ExtractCtx {
  PasswordState *password = nullptr;
  archiver::SafeDest *dest = nullptr;
  std::string *message = nullptr;
  std::string fallback_name;
  bool path_failed = false;
  bool io_failed = false;
  bool unsupported = false;
  bool wrong_password = false;
  bool data_error = false;
  bool pending_link = false;
  std::string pending_path;
  std::string *link_buf = nullptr;
  uint64_t written = 0;
  uint64_t total = 0;
  ArchiverProgressCallback progress = nullptr;
  void *progress_user = nullptr;
  bool cancelled = false;

  bool report_progress() {
    if (cancelled) return false;
    if (progress && progress(written, total, progress_user)) {
      cancelled = true;
      if (message) *message = "extraction cancelled";
      return false;
    }
    return true;
  }
};

class FileOutStream Z7_final : public ISequentialOutStream, public CMyUnknownImp {
  Z7_IFACES_IMP_UNK_1(ISequentialOutStream)
 public:
  std::unique_ptr<archiver::SafeDest::File> file;
  ExtractCtx *ctx = nullptr;
  ~FileOutStream() = default;
};

Z7_COM7F_IMF(FileOutStream::Write(const void *data, UInt32 size, UInt32 *processedSize)) {
  if (processedSize) *processedSize = 0;
  if (!file || !ctx) return E_FAIL;
  if (!ctx->report_progress()) return E_ABORT;
  if (ctx->written > kWriteCap - size) {
    ctx->io_failed = true;
    if (ctx->message) *ctx->message = "output is larger than this build will write";
    return E_ABORT;
  }
  std::string error;
  if (!file->write(data, size, error)) {
    ctx->io_failed = true;
    if (ctx->message) *ctx->message = "could not write output";
    return E_FAIL;
  }
  ctx->written += size;
  if (processedSize) *processedSize = size;
  if (!ctx->report_progress()) return E_ABORT;
  return S_OK;
}

class MemOutStream Z7_final : public ISequentialOutStream, public CMyUnknownImp {
  Z7_IFACES_IMP_UNK_1(ISequentialOutStream)
 public:
  std::string *dest = nullptr;
  ExtractCtx *ctx = nullptr;
};

Z7_COM7F_IMF(MemOutStream::Write(const void *data, UInt32 size, UInt32 *processedSize)) {
  if (processedSize) *processedSize = 0;
  if (!dest) return E_FAIL;
  if (dest->size() + size > kMaxSymlink) {
    if (ctx) {
      ctx->path_failed = true;
      if (ctx->message) *ctx->message = "symlink target is too long";
    }
    return E_ABORT;
  }
  dest->append(static_cast<const char *>(data), size);
  if (processedSize) *processedSize = size;
  return S_OK;
}

void note_write_error(ExtractCtx *ctx, const std::string &error) {
  if (error == "io" || error == "destination-exists") {
    ctx->io_failed = true;
    if (ctx->message && ctx->message->empty()) {
      *ctx->message = error == "destination-exists"
                          ? "destination file already exists; nothing was overwritten"
                          : "could not write output";
    }
    return;
  }
  if (error == "windows-symlink-untested") {
    ctx->unsupported = true;
    if (ctx->message) *ctx->message = "Windows symlink extraction was not verified and is refused";
    return;
  }
  ctx->path_failed = true;
  if (ctx->message && ctx->message->empty()) *ctx->message = "refused path";
}

class ExtractCallback Z7_final : public IArchiveExtractCallback,
                                 public ICryptoGetTextPassword,
                                 public CMyUnknownImp {
  Z7_COM_UNKNOWN_IMP_SPEC(Z7_COM_QI_ENTRY_UNKNOWN(IArchiveExtractCallback)
                              Z7_COM_QI_ENTRY(IArchiveExtractCallback)
                                  Z7_COM_QI_ENTRY(IProgress) Z7_COM_QI_ENTRY(ICryptoGetTextPassword))
  Z7_IFACE_COM7_IMP(IProgress)
  Z7_IFACE_COM7_IMP(IArchiveExtractCallback)
  Z7_IFACE_COM7_IMP(ICryptoGetTextPassword)
 public:
  IInArchive *archive = nullptr;
  ExtractCtx *ctx = nullptr;
  std::string link_buf;
  CMyComPtr<ISequentialOutStream> active_output;
  archiver::SafeDest::File *active_file = nullptr;
};

bool prop_bool(const PROPVARIANT &prop) {
  return prop.vt == VT_BOOL && prop.boolVal != VARIANT_FALSE;
}

uint64_t prop_u64(const PROPVARIANT &prop) {
  if (prop.vt == VT_UI8) return prop.uhVal.QuadPart;
  if (prop.vt == VT_UI4) return prop.ulVal;
  if (prop.vt == VT_UI2) return prop.uiVal;
  if (prop.vt == VT_I8 && prop.hVal.QuadPart > 0) return static_cast<uint64_t>(prop.hVal.QuadPart);
  return 0;
}

HRESULT read_string_prop(IInArchive *archive, UInt32 index, PROPID id, std::string &out,
                         bool &embedded_nul) {
  PROPVARIANT prop;
  memset(&prop, 0, sizeof(prop));
  HRESULT hr = archive->GetProperty(index, id, &prop);
  if (hr != S_OK) {
    prop_clear(&prop);
    return hr;
  }
  if (prop.vt == VT_EMPTY || prop.vt == VT_NULL) {
    out.clear();
    embedded_nul = false;
    prop_clear(&prop);
    return S_OK;
  }
  if (prop.vt != VT_BSTR) {
    prop_clear(&prop);
    out.clear();
    return S_OK;
  }
  bstr_utf8(prop.bstrVal, out, embedded_nul);
  prop_clear(&prop);
  return S_OK;
}

Z7_COM7F_IMF(ExtractCallback::SetTotal(UInt64 total)) {
  if (!ctx) return E_FAIL;
  ctx->total = total;
  return ctx->report_progress() ? S_OK : E_ABORT;
}
Z7_COM7F_IMF(ExtractCallback::SetCompleted(const UInt64 * /*completeValue*/)) {
  return ctx && ctx->report_progress() ? S_OK : E_ABORT;
}

Z7_COM7F_IMF(ExtractCallback::GetStream(UInt32 index, ISequentialOutStream **outStream,
                                        Int32 askExtractMode)) {
  *outStream = nullptr;
  if (!ctx || !archive) return E_FAIL;
  active_output.Release();
  active_file = nullptr;
  ctx->pending_link = false;
  link_buf.clear();
  if (askExtractMode != NArchive::NExtract::NAskMode::kExtract) return S_OK;

  std::string path;
  bool embedded = false;
  if (read_string_prop(archive, index, kpidPath, path, embedded) != S_OK) return E_FAIL;
  if (path.empty()) path = ctx->fallback_name;
  if (path.empty()) return S_OK;
  if (embedded) {
    ctx->path_failed = true;
    if (ctx->message) *ctx->message = "refused path: control-character";
    return E_ABORT;
  }
  archiver::CheckedPath checked = archiver::check_entry_path(path);
  if (checked.reject != archiver::PathReject::Ok) {
    ctx->path_failed = true;
    if (ctx->message) {
      *ctx->message = std::string("refused path: ") + archiver::path_reject_string(checked.reject);
    }
    return E_ABORT;
  }

  PROPVARIANT prop;
  memset(&prop, 0, sizeof(prop));
  if (archive->GetProperty(index, kpidIsDir, &prop) != S_OK) {
    prop_clear(&prop);
    return E_FAIL;
  }
  bool is_dir = prop_bool(prop);
  prop_clear(&prop);

  std::string link;
  if (read_string_prop(archive, index, kpidSymLink, link, embedded) != S_OK) return E_FAIL;
  if (embedded) link.clear();

  memset(&prop, 0, sizeof(prop));
  uint32_t posix = 0;
  if (archive->GetProperty(index, kpidPosixAttrib, &prop) == S_OK && prop.vt == VT_UI4) {
    posix = prop.ulVal;
  }
  prop_clear(&prop);
  bool posix_link = (posix & 0170000u) == 0120000u;

  if (is_dir) {
    std::string error;
    if (!ctx->dest->mkdir_entry(checked.relative, error)) {
      note_write_error(ctx, error);
      return E_ABORT;
    }
    return S_OK;
  }

  if (!link.empty()) {
    archiver::CheckedPath target = archiver::check_symlink_target(checked.relative, link);
    if (target.reject != archiver::PathReject::Ok) {
      ctx->path_failed = true;
      if (ctx->message) *ctx->message = "refused symlink";
      return E_ABORT;
    }
    std::string error;
    if (!ctx->dest->make_symlink(checked.relative, link, error)) {
      note_write_error(ctx, error);
      return E_ABORT;
    }
    return S_OK;
  }

  if (posix_link) {
    ctx->pending_link = true;
    ctx->pending_path = checked.relative;
    ctx->link_buf = &link_buf;
    MemOutStream *mem = new MemOutStream();
    mem->dest = &link_buf;
    mem->ctx = ctx;
    CMyComPtr<ISequentialOutStream> held(mem);
    *outStream = held.Detach();
    return S_OK;
  }

  std::string error;
  std::unique_ptr<archiver::SafeDest::File> file = ctx->dest->create_file(checked.relative, error);
  if (!file) {
    note_write_error(ctx, error);
    return E_ABORT;
  }
  FileOutStream *out = new FileOutStream();
  out->file = std::move(file);
  out->ctx = ctx;
  active_output = out;
  active_file = out->file.get();
  CMyComPtr<ISequentialOutStream> held(out);
  *outStream = held.Detach();
  return S_OK;
}

Z7_COM7F_IMF(ExtractCallback::PrepareOperation(Int32 /*askExtractMode*/)) { return S_OK; }

Z7_COM7F_IMF(ExtractCallback::SetOperationResult(Int32 opRes)) {
  if (!ctx) return S_OK;
  if (active_output) {
    if (opRes == NArchive::NExtract::NOperationResult::kOK && !ctx->cancelled) {
      std::string error;
      if (!active_file->commit(error)) note_write_error(ctx, error);
    }
    active_output.Release();
    active_file = nullptr;
  }
  if (opRes == NArchive::NExtract::NOperationResult::kWrongPassword) ctx->wrong_password = true;
  if (opRes != NArchive::NExtract::NOperationResult::kOK &&
      opRes != NArchive::NExtract::NOperationResult::kWrongPassword) {
    ctx->data_error = true;
  }
  if (ctx->pending_link) {
    ctx->pending_link = false;
    if (opRes != NArchive::NExtract::NOperationResult::kOK || ctx->cancelled) {
      wipe_string(link_buf);
      return S_OK;
    }
    if (link_buf.find('\0') != std::string::npos || link_buf.size() > kMaxSymlink) {
      ctx->path_failed = true;
      if (ctx->message) *ctx->message = "refused symlink";
      return S_OK;
    }
    archiver::CheckedPath target = archiver::check_symlink_target(ctx->pending_path, link_buf);
    if (target.reject != archiver::PathReject::Ok) {
      ctx->path_failed = true;
      if (ctx->message) *ctx->message = "refused symlink";
      return S_OK;
    }
    std::string error;
    if (ctx->dest && !ctx->dest->make_symlink(ctx->pending_path, link_buf, error)) {
      note_write_error(ctx, error);
    }
    wipe_string(link_buf);
  }
  return S_OK;
}

Z7_COM7F_IMF(ExtractCallback::CryptoGetTextPassword(BSTR *passwordOut)) {
  if (!passwordOut) return E_ABORT;
  *passwordOut = nullptr;
  if (!ctx || !ctx->password) return E_ABORT;
  ctx->password->asked = true;
  if (!ctx->password->provided) return E_ABORT;
  *passwordOut = alloc_wide(ctx->password->text.c_str(), ctx->password->text.size());
  return *passwordOut ? S_OK : E_OUTOFMEMORY;
}

FileInStream *open_fd(const std::string &path, uint64_t slice_base = 0) {
  FileInStream *stream = new FileInStream();
#if defined(_WIN32)
  std::wstring wide = utf8_to_wide(path.c_str());
  HANDLE handle = ::CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    delete stream;
    return nullptr;
  }
  stream->handle = handle;
  LARGE_INTEGER length;
  if (!::GetFileSizeEx(handle, &length) || slice_base > static_cast<uint64_t>(length.QuadPart)) {
    delete stream;
    return nullptr;
  }
  stream->view_size = static_cast<uint64_t>(length.QuadPart) - slice_base;
#else
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    delete stream;
    return nullptr;
  }
  stream->fd = fd;
  struct stat st;
  if (::fstat(fd, &st) != 0 || slice_base > static_cast<uint64_t>(st.st_size)) {
    delete stream;
    return nullptr;
  }
  stream->view_size = static_cast<uint64_t>(st.st_size) - slice_base;
#endif
  stream->view_base = slice_base;
  stream->view_pos = 0;
  return stream;
}

std::string parent_dir_of(const std::string &path) {
  size_t slash = path.find_last_of("/\\");
  if (slash == std::string::npos) return std::string();
  return path.substr(0, slash);
}

std::string base_name_of(const std::string &path) {
  size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string join_dir(const std::string &dir, const std::string &name) {
  if (dir.empty()) return name;
  if (dir.back() == '/' || dir.back() == '\\') return dir + name;
#if defined(_WIN32)
  return dir + "\\" + name;
#else
  return dir + "/" + name;
#endif
}

bool regular_file_exists(const std::string &path) {
#if defined(_WIN32)
  std::wstring wide = utf8_to_wide(path.c_str());
  DWORD attr = ::GetFileAttributesW(wide.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
  struct stat st;
  return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#endif
}

bool next_volume_name(const std::string &name, std::string &out) {
  std::string lower = name;
  for (char &c : lower) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  size_t digit_end = name.size();
  if (ends_with(lower, ".rar")) {
    if (name.size() < 4) return false;
    digit_end = name.size() - 4;
  }
  size_t i = digit_end;
  while (i > 0 && name[i - 1] >= '0' && name[i - 1] <= '9') --i;
  if (i == digit_end) return false;
  std::string digits = name.substr(i, digit_end - i);
  int carry = 1;
  for (size_t k = digits.size(); k > 0; --k) {
    int d = (digits[k - 1] - '0') + carry;
    carry = d / 10;
    digits[k - 1] = static_cast<char>('0' + (d % 10));
  }
  if (carry) digits.insert(digits.begin(), '1');
  out = name.substr(0, i) + digits + name.substr(digit_end);
  return out != name;
}

bool later_volume_exists(const std::string &dir, const std::string &missing_name) {
  std::string cur = missing_name;
  for (int step = 0; step < 8; ++step) {
    std::string next;
    if (!next_volume_name(cur, next)) return false;
    if (regular_file_exists(join_dir(dir, next))) return true;
    cur = next;
  }
  return false;
}

bool safe_volume_leaf(const wchar_t *name, std::string &utf8) {
  if (!name || !name[0]) return false;
  size_t n = 0;
  while (name[n]) ++n;
  utf8 = wide_to_utf8(name, n);
  if (utf8.empty() || utf8 == "." || utf8 == "..") return false;
  if (utf8.find('/') != std::string::npos || utf8.find('\\') != std::string::npos) return false;
  if (utf8.find(':') != std::string::npos) return false;
  if (utf8.find("..") != std::string::npos) return false;
  return true;
}

Z7_COM7F_IMF(OpenCallback::GetProperty(PROPID propID, PROPVARIANT *value)) {
  if (!value) return E_INVALIDARG;
  memset(value, 0, sizeof(*value));
  if (propID == kpidName && volumes && !volumes->archive_name.empty()) {
    std::wstring wide = utf8_to_wide(volumes->archive_name.c_str());
    value->vt = VT_BSTR;
    value->bstrVal = alloc_wide(wide.c_str(), wide.size());
    return value->bstrVal ? S_OK : E_OUTOFMEMORY;
  }
  return S_OK;
}

Z7_COM7F_IMF(OpenCallback::GetStream(const wchar_t *name, IInStream **inStream)) {
  if (!inStream) return E_INVALIDARG;
  *inStream = nullptr;
  if (!volumes) return S_FALSE;
  std::string leaf;
  if (!safe_volume_leaf(name, leaf)) return S_FALSE;
  std::string full = join_dir(volumes->directory, leaf);
  FileInStream *raw = open_fd(full);
  if (!raw) {
    if (later_volume_exists(volumes->directory, leaf)) {
      volumes->missing = leaf;
      return E_FAIL;
    }
    if (volumes->next_absent.empty()) volumes->next_absent = leaf;
    return S_FALSE;
  }
  CMyComPtr<IInStream> held(raw);
  *inStream = held.Detach();
  return S_OK;
}

bool read_at(const std::string &path, uint64_t offset, unsigned char *buf, size_t size, size_t *got) {
  *got = 0;
  if (size == 0) return true;
#if defined(_WIN32)
  std::wstring wide = utf8_to_wide(path.c_str());
  HANDLE handle = ::CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER move;
  move.QuadPart = static_cast<LONGLONG>(offset);
  if (!::SetFilePointerEx(handle, move, nullptr, FILE_BEGIN)) {
    ::CloseHandle(handle);
    return false;
  }
  DWORD n = 0;
  BOOL ok = ::ReadFile(handle, buf, static_cast<DWORD>(size), &n, nullptr);
  ::CloseHandle(handle);
  if (!ok) return false;
  *got = n;
#else
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  ssize_t n = ::pread(fd, buf, size, static_cast<off_t>(offset));
  ::close(fd);
  if (n < 0) return false;
  *got = static_cast<size_t>(n);
#endif
  return true;
}

bool file_size_of(const std::string &path, uint64_t *size) {
#if defined(_WIN32)
  std::wstring wide = utf8_to_wide(path.c_str());
  HANDLE handle = ::CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER length;
  BOOL ok = ::GetFileSizeEx(handle, &length);
  ::CloseHandle(handle);
  if (!ok || length.QuadPart < 0) return false;
  *size = static_cast<uint64_t>(length.QuadPart);
#else
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) return false;
  *size = static_cast<uint64_t>(st.st_size);
#endif
  return true;
}

bool read_prefix(const std::string &path, unsigned char *buf, size_t size, size_t *got) {
  *got = 0;
#if defined(_WIN32)
  std::wstring wide = utf8_to_wide(path.c_str());
  HANDLE handle = ::CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  DWORD n = 0;
  BOOL ok = ::ReadFile(handle, buf, static_cast<DWORD>(size), &n, nullptr);
  ::CloseHandle(handle);
  if (!ok) return false;
  *got = n;
#else
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  ssize_t n = ::read(fd, buf, size);
  ::close(fd);
  if (n < 0) return false;
  *got = static_cast<size_t>(n);
#endif
  return true;
}

ArchiverResult from_open_hr(HRESULT hr, const PasswordState &password) {
  if (hr == S_OK) return ARCHIVER_OK;
  if (password.asked && !password.provided) return ARCHIVER_ERR_PASSWORD_REQUIRED;
  if (password.asked && password.provided) return ARCHIVER_ERR_PASSWORD;
  return ARCHIVER_ERR_FORMAT;
}

bool archive_has_encrypted_item(IInArchive *archive) {
  UInt32 count = 0;
  if (!archive || archive->GetNumberOfItems(&count) != S_OK) return false;
  for (UInt32 i = 0; i < count; ++i) {
    PROPVARIANT prop;
    memset(&prop, 0, sizeof(prop));
    if (archive->GetProperty(i, kpidEncrypted, &prop) == S_OK && prop_bool(prop)) {
      prop_clear(&prop);
      return true;
    }
    prop_clear(&prop);
  }
  return false;
}

ArchiverResult from_extract(HRESULT hr, const ExtractCtx &ctx, const PasswordState &password,
                            bool encrypted) {
  if (ctx.path_failed) return ARCHIVER_ERR_PATH;
  if (ctx.unsupported) return ARCHIVER_ERR_UNSUPPORTED;
  if (ctx.io_failed) return ARCHIVER_ERR_IO;
  if (ctx.cancelled) return ARCHIVER_ERR_CANCELLED;
  if (password.asked && !password.provided) return ARCHIVER_ERR_PASSWORD_REQUIRED;
  if (ctx.wrong_password || (ctx.data_error && (encrypted || password.asked))) {
    return password.provided ? ARCHIVER_ERR_PASSWORD : ARCHIVER_ERR_PASSWORD_REQUIRED;
  }
  if (encrypted && !password.provided && hr != S_OK) return ARCHIVER_ERR_PASSWORD_REQUIRED;
  if (ctx.data_error || hr != S_OK) return ARCHIVER_ERR_FORMAT;
  return ARCHIVER_OK;
}

bool make_temp_dir(std::string &out) {
#if defined(_WIN32)
  wchar_t base[MAX_PATH];
  DWORD n = ::GetTempPathW(MAX_PATH, base);
  if (n == 0 || n >= MAX_PATH) return false;
  for (unsigned i = 0; i < 1000; ++i) {
    std::wstring dir = std::wstring(base) + L"milkzip-" +
                       std::to_wstring(::GetCurrentProcessId()) + L"-" + std::to_wstring(i);
    if (::CreateDirectoryW(dir.c_str(), nullptr)) {
      out = wide_to_utf8(dir.c_str(), dir.size());
      return true;
    }
    if (::GetLastError() != ERROR_ALREADY_EXISTS) return false;
  }
  return false;
#else
  const char *base = getenv("TMPDIR");
  std::string dir = (base && base[0]) ? base : "/tmp";
  if (dir.back() != '/') dir.push_back('/');
  dir += "milkzip-XXXXXX";
  std::vector<char> buf(dir.begin(), dir.end());
  buf.push_back('\0');
  if (!mkdtemp(buf.data())) return false;
  out = buf.data();
  return true;
#endif
}

std::string archive_error_text(IInArchive *archive) {
  if (!archive) return std::string();
  PROPVARIANT prop;
  memset(&prop, 0, sizeof(prop));
  if (archive->GetArchiveProperty(kpidError, &prop) != S_OK) {
    prop_clear(&prop);
    return std::string();
  }
  std::string out;
  if (prop.vt == VT_BSTR) {
    bool nul = false;
    bstr_utf8(prop.bstrVal, out, nul);
  }
  prop_clear(&prop);
  const char *prefix = "Missing volume : ";
  size_t n = strlen(prefix);
  if (out.compare(0, n, prefix) == 0) return out.substr(n);
  return std::string();
}

void fill_volume_context(VolumeContext &ctx, const std::string &path) {
  ctx.directory = parent_dir_of(path);
  ctx.archive_name = base_name_of(path);
  ctx.missing.clear();
  ctx.next_absent.clear();
}

ArchiverResult finish_open_result(HRESULT hr, const PasswordState &password, VolumeContext *volumes,
                                  IInArchive *opened, std::string &message) {
  if (volumes && !volumes->missing.empty()) {
    message = "missing volume: " + volumes->missing;
    return ARCHIVER_ERR_MISSING_VOLUME;
  }
  ArchiverResult result = from_open_hr(hr, password);
  if (result != ARCHIVER_OK) {
    if (message.empty()) {
      if (result == ARCHIVER_ERR_PASSWORD_REQUIRED) message = "password required";
      else if (result == ARCHIVER_ERR_PASSWORD) message = "password rejected";
      else message = "not a supported archive";
    }
    return result;
  }
  std::string missing = archive_error_text(opened);
  if (!missing.empty()) {
    message = "missing volume: " + missing;
    return ARCHIVER_ERR_MISSING_VOLUME;
  }
  return ARCHIVER_OK;
}

ArchiverResult open_with(EngineLib &lib, const GUID &clsid, const std::string &path,
                         PasswordState &password, CMyComPtr<IInArchive> &archive,
                         CMyComPtr<IInStream> &stream, std::string &message,
                         uint64_t slice_base = 0, VolumeContext *volumes = nullptr) {
  FileInStream *raw = open_fd(path, slice_base);
  if (!raw) {
    message = "could not open archive file";
    return ARCHIVER_ERR_IO;
  }
  CMyComPtr<IInStream> in(raw);
  void *obj = nullptr;
  HRESULT hr = lib.CreateObject(&clsid, &IID_IInArchive, &obj);
  if (hr != S_OK || !obj) {
    message = "7-Zip did not create an archive handler";
    return ARCHIVER_ERR_ENGINE;
  }
  CMyComPtr<IInArchive> in_archive;
  in_archive.Attach(static_cast<IInArchive *>(obj));
  OpenCallback *callback = new OpenCallback();
  callback->password = &password;
  callback->volumes = volumes;
  CMyComPtr<IArchiveOpenCallback> held(callback);
  UInt64 max_check = 0;
  hr = in_archive->Open(in, &max_check, held);
  ArchiverResult result = finish_open_result(hr, password, volumes, in_archive, message);
  if (result != ARCHIVER_OK) {
    in_archive->Close();
    return result;
  }
  archive = in_archive;
  stream = in;
  return ARCHIVER_OK;
}

ArchiverResult open_on_stream(EngineLib &lib, const GUID &clsid, IInStream *in, PasswordState &password,
                              CMyComPtr<IInArchive> &archive, std::string &message) {
  if (!in) return ARCHIVER_ERR_FORMAT;
  void *obj = nullptr;
  HRESULT hr = lib.CreateObject(&clsid, &IID_IInArchive, &obj);
  if (hr != S_OK || !obj) {
    message = "7-Zip did not create an archive handler";
    return ARCHIVER_ERR_ENGINE;
  }
  CMyComPtr<IInArchive> in_archive;
  in_archive.Attach(static_cast<IInArchive *>(obj));
  OpenCallback *callback = new OpenCallback();
  callback->password = &password;
  CMyComPtr<IArchiveOpenCallback> held(callback);
  if (in->Seek(0, 0, nullptr) != S_OK) {
    message = "not a supported archive";
    return ARCHIVER_ERR_FORMAT;
  }
  UInt64 max_check = 0;
  hr = in_archive->Open(in, &max_check, held);
  ArchiverResult result = finish_open_result(hr, password, nullptr, in_archive, message);
  if (result != ARCHIVER_OK) {
    in_archive->Close();
    return result;
  }
  archive = in_archive;
  return ARCHIVER_OK;
}

ArchiverResult read_entries(IInArchive *archive, std::vector<ListedEntry> &entries,
                            std::string &message, const std::string &fallback = {}) {
  UInt32 count = 0;
  if (archive->GetNumberOfItems(&count) != S_OK) {
    message = "could not list archive";
    return ARCHIVER_ERR_FORMAT;
  }
  if (count > kMaxEntries) {
    message = "archive has too many entries";
    return ARCHIVER_ERR_UNSUPPORTED;
  }
  uint64_t declared = 0;
  for (UInt32 i = 0; i < count; ++i) {
    std::string path;
    bool embedded = false;
    if (read_string_prop(archive, i, kpidPath, path, embedded) != S_OK) {
      message = "could not read entry name";
      return ARCHIVER_ERR_FORMAT;
    }
    if (path.empty()) path = fallback;
    if (path.empty()) continue;
    if (embedded) {
      message = "refused path: control-character";
      return ARCHIVER_ERR_PATH;
    }
    archiver::CheckedPath checked = archiver::check_entry_path(path);
    if (checked.reject != archiver::PathReject::Ok) {
      message = std::string("refused path: ") + archiver::path_reject_string(checked.reject);
      return ARCHIVER_ERR_PATH;
    }
    std::string link;
    bool link_nul = false;
    if (read_string_prop(archive, i, kpidSymLink, link, link_nul) != S_OK) {
      message = "could not read entry";
      return ARCHIVER_ERR_FORMAT;
    }
    if (!link.empty()) {
      if (link_nul || archiver::check_symlink_target(checked.relative, link).reject !=
                          archiver::PathReject::Ok) {
        message = "refused symlink";
        return ARCHIVER_ERR_PATH;
      }
    }
    PROPVARIANT prop;
    memset(&prop, 0, sizeof(prop));
    bool is_dir = false;
    if (archive->GetProperty(i, kpidIsDir, &prop) == S_OK) is_dir = prop_bool(prop);
    prop_clear(&prop);
    memset(&prop, 0, sizeof(prop));
    uint64_t size = 0;
    if (archive->GetProperty(i, kpidSize, &prop) == S_OK) size = prop_u64(prop);
    prop_clear(&prop);
    memset(&prop, 0, sizeof(prop));
    uint64_t packed = 0;
    if (archive->GetProperty(i, kpidPackSize, &prop) == S_OK) packed = prop_u64(prop);
    prop_clear(&prop);
    memset(&prop, 0, sizeof(prop));
    int64_t modified = -1;
    if (archive->GetProperty(i, kpidMTime, &prop) == S_OK && prop.vt == VT_FILETIME) {
      uint64_t ticks = (static_cast<uint64_t>(prop.filetime.dwHighDateTime) << 32) |
                       prop.filetime.dwLowDateTime;
      const uint64_t epoch = 116444736000000000ull;
      if (ticks >= epoch) modified = static_cast<int64_t>((ticks - epoch) / 10000000ull);
    }
    prop_clear(&prop);
    if (size > kWriteCap || declared > kWriteCap - size) {
      message = "archive declares more data than this build will write";
      return ARCHIVER_ERR_UNSUPPORTED;
    }
    declared += size;
    memset(&prop, 0, sizeof(prop));
    uint32_t posix = 0;
    if (archive->GetProperty(i, kpidPosixAttrib, &prop) == S_OK && prop.vt == VT_UI4) posix = prop.ulVal;
    prop_clear(&prop);
    ListedEntry entry;
    entry.path = checked.relative;
    entry.is_dir = is_dir;
    entry.is_symlink = !link.empty() || (posix & 0170000u) == 0120000u;
    entry.size = size;
    entry.packed_size = packed;
    entry.modified_unix = modified;
    entries.push_back(entry);
  }
  return ARCHIVER_OK;
}

const char *outer_name(Kind kind) {
  switch (kind) {
    case Kind::Zip: return "zip";
    case Kind::SevenZ: return "7z";
    case Kind::Tar: return "tar";
    case Kind::TarGz: return "gzip";
    case Kind::TarBz2: return "bzip2";
    case Kind::TarXz: return "xz";
    case Kind::TarZst: case Kind::Zstd: return "zstd";
    case Kind::Gzip: return "gzip";
    case Kind::Bzip2: return "bzip2";
    case Kind::Xz: return "xz";
    case Kind::Lzma: return "lzma";
    case Kind::Compress: return "Z";
    case Kind::Cab: return "Cab";
    case Kind::Iso: return "Iso";
    case Kind::Udf: return "Udf";
    case Kind::Wim: return "Wim";
    case Kind::Arj: return "Arj";
    case Kind::Lzh: return "Lzh";
    case Kind::Cpio: return "Cpio";
    case Kind::Ar: return "Ar";
    case Kind::Rpm: return "Rpm";
    case Kind::Dmg: return "Dmg";
    case Kind::Xar: return "Xar";
    default: return "";
  }
}

bool handler_for(EngineLib &lib, Kind kind, const std::string &path, GUID &out, std::string &message) {
  if (kind == Kind::Rar) {
    unsigned char sig[8];
    memset(sig, 0, sizeof(sig));
    size_t n = 0;
    if (!read_prefix(path, sig, sizeof(sig), &n)) {
      message = "could not open archive file";
      return false;
    }
    bool rar5 = n >= 8 && sig[0] == 'R' && sig[1] == 'a' && sig[2] == 'r' && sig[3] == '!' &&
                sig[6] == 1;
    if (find_handler(lib, rar5 ? "Rar5" : "Rar", out)) return true;
    message = "7-Zip library has no RAR handler";
    return false;
  }
  if (kind == Kind::TarZst || kind == Kind::Zstd) {
    if (find_handler(lib, "zstd", out) || find_handler(lib, "zstd2", out)) return true;
    message = "7-Zip library has no zstd handler";
    return false;
  }
  const char *name = outer_name(kind);
  if (!name[0] || !find_handler(lib, name, out)) {
    message = "7-Zip library has no handler for this extension";
    return false;
  }
  return true;
}

class BorrowedFileOut Z7_final : public ISequentialOutStream, public CMyUnknownImp {
  Z7_IFACES_IMP_UNK_1(ISequentialOutStream)
 public:
  archiver::SafeDest::File *file = nullptr;
  bool *failed = nullptr;
  uint64_t written = 0;
};

Z7_COM7F_IMF(BorrowedFileOut::Write(const void *data, UInt32 size, UInt32 *processedSize)) {
  if (processedSize) *processedSize = 0;
  if (!file) return E_FAIL;
  if (written > kWriteCap - size) {
    if (failed) *failed = true;
    return E_ABORT;
  }
  std::string error;
  if (!file->write(data, size, error)) {
    if (failed) *failed = true;
    return E_FAIL;
  }
  written += size;
  if (processedSize) *processedSize = size;
  return S_OK;
}

class SingleCallback Z7_final : public IArchiveExtractCallback,
                                public ICryptoGetTextPassword,
                                public CMyUnknownImp {
  Z7_IFACES_IMP_UNK_2(IArchiveExtractCallback, ICryptoGetTextPassword)
  Z7_IFACE_COM7_IMP(IProgress)
 public:
  PasswordState *password = nullptr;
  std::unique_ptr<archiver::SafeDest::File> file;
  bool failed = false;
  bool wrong_password = false;
  UInt32 seen = 0;
};

Z7_COM7F_IMF(SingleCallback::SetTotal(UInt64 /*total*/)) { return S_OK; }
Z7_COM7F_IMF(SingleCallback::SetCompleted(const UInt64 * /*completeValue*/)) { return S_OK; }
Z7_COM7F_IMF(SingleCallback::PrepareOperation(Int32 /*askExtractMode*/)) { return S_OK; }
Z7_COM7F_IMF(SingleCallback::SetOperationResult(Int32 opRes)) {
  if (opRes == NArchive::NExtract::NOperationResult::kWrongPassword) wrong_password = true;
  if (opRes != NArchive::NExtract::NOperationResult::kOK &&
      opRes != NArchive::NExtract::NOperationResult::kUnavailable) {
    failed = true;
  }
  return S_OK;
}
Z7_COM7F_IMF(SingleCallback::CryptoGetTextPassword(BSTR *passwordOut)) {
  if (!passwordOut) return E_ABORT;
  *passwordOut = nullptr;
  if (!password) return E_ABORT;
  password->asked = true;
  if (!password->provided) return E_ABORT;
  *passwordOut = alloc_wide(password->text.c_str(), password->text.size());
  return *passwordOut ? S_OK : E_OUTOFMEMORY;
}
Z7_COM7F_IMF(SingleCallback::GetStream(UInt32 index, ISequentialOutStream **outStream,
                                       Int32 askExtractMode)) {
  *outStream = nullptr;
  if (askExtractMode != NArchive::NExtract::NAskMode::kExtract) return S_OK;
  ++seen;
  if (seen != 1 || !file) {
    failed = true;
    return E_ABORT;
  }
  (void)index;
  /* Ignore the name stored in the compressor. The bytes go to a path we chose. */
  BorrowedFileOut *out = new BorrowedFileOut();
  out->file = file.get();
  out->failed = &failed;
  CMyComPtr<ISequentialOutStream> held(out);
  *outStream = held.Detach();
  return S_OK;
}

ArchiverResult extract_single(IInArchive *archive, archiver::SafeDest &dest, PasswordState &password,
                              std::string &message) {
  UInt32 count = 0;
  if (archive->GetNumberOfItems(&count) != S_OK || count != 1) {
    message = "compressed tar must contain one stream";
    return ARCHIVER_ERR_FORMAT;
  }
  std::string error;
  std::unique_ptr<archiver::SafeDest::File> file = dest.create_file("inner.bin", error);
  if (!file) {
    message = "could not write temporary tar";
    return ARCHIVER_ERR_IO;
  }
  SingleCallback *callback = new SingleCallback();
  callback->password = &password;
  callback->file = std::move(file);
  CMyComPtr<IArchiveExtractCallback> held(callback);
  UInt32 all = static_cast<UInt32>(-1);
  HRESULT hr = archive->Extract(nullptr, all, 0, held);
  if (password.asked && !password.provided) return ARCHIVER_ERR_PASSWORD_REQUIRED;
  if (callback->wrong_password || (callback->failed && password.asked && password.provided)) {
    message = "password rejected";
    return ARCHIVER_ERR_PASSWORD;
  }
  if (hr != S_OK || callback->failed || callback->seen != 1) {
    message = "could not unwrap tar compression";
    return ARCHIVER_ERR_FORMAT;
  }
  if (!callback->file->commit(error)) {
    message = "could not write temporary tar";
    return ARCHIVER_ERR_IO;
  }
  return ARCHIVER_OK;
}

PasswordState password_from(const char *password) {
  PasswordState state;
  if (password) {
    state.text = utf8_to_wide(password);
    state.provided = true;
  }
  return state;
}

ArchiverResult open_kind(ArchiverArchive *archive, Kind kind, const std::string &path,
                         PasswordState &password, VolumeContext *volumes = nullptr) {
  GUID clsid;
  if (!handler_for(archive->engine, kind, path, clsid, archive->message)) {
    return archive->message.find("could not open") != std::string::npos ? ARCHIVER_ERR_IO
                                                                         : ARCHIVER_ERR_ENGINE;
  }
  return open_with(archive->engine, clsid, path, password, archive->archive, archive->stream,
                   archive->message, 0, volumes);
}

ArchiverResult open_numeric_volume(ArchiverArchive *archive, Kind kind, const std::string &path,
                                   PasswordState &password);

static const char *kEmptyMessage = "";

const char *archiver_result_string(ArchiverResult result) {
  switch (result) {
    case ARCHIVER_OK: return "ok";
    case ARCHIVER_ERR_ARGUMENT: return "bad-argument";
    case ARCHIVER_ERR_IO: return "io";
    case ARCHIVER_ERR_FORMAT: return "format";
    case ARCHIVER_ERR_ENGINE: return "engine";
    case ARCHIVER_ERR_PASSWORD_REQUIRED: return "password-required";
    case ARCHIVER_ERR_PASSWORD: return "password-rejected";
    case ARCHIVER_ERR_PATH: return "path";
    case ARCHIVER_ERR_UNSUPPORTED: return "unsupported";
    case ARCHIVER_ERR_CANCELLED: return "cancelled";
    case ARCHIVER_ERR_MISSING_VOLUME: return "missing-volume";
    case ARCHIVER_ERR_NEED_CHOICE: return "need-choice";
    case ARCHIVER_ERR_LIMIT: return "limit";
    case ARCHIVER_ERR_PARTIAL: return "partial";
  }
  return "error";
}

const char *archiver_message(const ArchiverArchive *archive) {
  if (!archive || archive->message.empty()) return kEmptyMessage;
  return archive->message.c_str();
}

ArchiverResult archiver_check_entry_path(const char *path) {
  if (!path) return ARCHIVER_ERR_ARGUMENT;
  archiver::CheckedPath checked = archiver::check_entry_path(path);
  return checked.reject == archiver::PathReject::Ok ? ARCHIVER_OK : ARCHIVER_ERR_PATH;
}

ArchiverResult archiver_check_symlink(const char *link_path, const char *target) {
  if (!link_path || !target) return ARCHIVER_ERR_ARGUMENT;
  archiver::CheckedPath link = archiver::check_entry_path(link_path);
  if (link.reject != archiver::PathReject::Ok) return ARCHIVER_ERR_PATH;
  archiver::CheckedPath checked = archiver::check_symlink_target(link.relative, target);
  return checked.reject == archiver::PathReject::Ok ? ARCHIVER_OK : ARCHIVER_ERR_PATH;
}

bool bytes_eq(const unsigned char *p, const char *lit, size_t n) {
  return memcmp(p, lit, n) == 0;
}

bool is_zip_local(const unsigned char *p) {
  return p[0] == 'P' && p[1] == 'K' && p[2] == 3 && p[3] == 4;
}
bool is_7z_sig(const unsigned char *p) {
  return bytes_eq(p, "\x37\x7A\xBC\xAF\x27\x1C", 6);
}
bool is_rar_sig(const unsigned char *p) {
  return bytes_eq(p, "Rar!\x1A\x07", 6);
}

uint32_t read_u32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint16_t read_u16(const unsigned char *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
uint64_t read_u64(const unsigned char *p) {
  return (uint64_t)read_u32(p) | ((uint64_t)read_u32(p + 4) << 32);
}

bool guid_for_kind(EngineLib &lib, Kind kind, const std::string &path, uint64_t offset, GUID &out,
                   std::string &message) {
  if (kind == Kind::Rar) {
    unsigned char sig[8];
    memset(sig, 0, sizeof(sig));
    size_t n = 0;
    if (!read_at(path, offset, sig, sizeof(sig), &n)) {
      message = "could not open archive file";
      return false;
    }
    bool rar5 = n >= 8 && is_rar_sig(sig) && sig[6] == 1;
    if (find_handler(lib, rar5 ? "Rar5" : "Rar", out)) return true;
    message = "7-Zip library has no RAR handler";
    return false;
  }
  const char *name = outer_name(kind);
  if (!name[0] || !find_handler(lib, name, out)) {
    message = "7-Zip library has no handler for this extension";
    return false;
  }
  return true;
}

Kind kind_from_signature(const unsigned char *sig, size_t n) {
  if (n >= 4 && is_zip_local(sig)) return Kind::Zip;
  if (n >= 6 && is_7z_sig(sig)) return Kind::SevenZ;
  if (n >= 7 && is_rar_sig(sig)) return Kind::Rar;
  return Kind::Unsupported;
}

ArchiverResult open_numeric_volume(ArchiverArchive *archive, Kind kind, const std::string &path,
                                   PasswordState &password) {
  GUID split_id;
  if (!find_handler(archive->engine, "Split", split_id)) {
    archive->message = "7-Zip library has no split handler";
    return ARCHIVER_ERR_ENGINE;
  }
  fill_volume_context(archive->volumes, path);
  CMyComPtr<IInArchive> split;
  CMyComPtr<IInStream> split_stream;
  ArchiverResult opened = open_with(archive->engine, split_id, path, password, split, split_stream,
                                     archive->message, 0, &archive->volumes);
  if (opened != ARCHIVER_OK) {
    if (opened == ARCHIVER_ERR_MISSING_VOLUME || opened == ARCHIVER_ERR_PASSWORD ||
        opened == ARCHIVER_ERR_PASSWORD_REQUIRED) {
      return opened;
    }
    archive->message.clear();
    return open_kind(archive, kind, path, password, &archive->volumes);
  }

  IInArchiveGetStream *raw_getter = nullptr;
  if (split->QueryInterface(IID_IInArchiveGetStream, reinterpret_cast<void **>(&raw_getter)) != S_OK ||
      !raw_getter) {
    archive->message = "not a supported archive";
    split->Close();
    return ARCHIVER_ERR_FORMAT;
  }
  CMyComPtr<IInArchiveGetStream> getter;
  getter.Attach(raw_getter);
  CMyComPtr<ISequentialInStream> seq;
  if (getter->GetStream(0, &seq) != S_OK || !seq) {
    archive->message = "not a supported archive";
    split->Close();
    return ARCHIVER_ERR_FORMAT;
  }
  IInStream *raw_sub = nullptr;
  if (seq->QueryInterface(IID_IInStream, reinterpret_cast<void **>(&raw_sub)) != S_OK || !raw_sub) {
    archive->message = "not a supported archive";
    split->Close();
    return ARCHIVER_ERR_FORMAT;
  }
  CMyComPtr<IInStream> sub;
  sub.Attach(raw_sub);
  unsigned char sig[8];
  memset(sig, 0, sizeof(sig));
  size_t got = 0;
  if (sub->Seek(0, 0, nullptr) != S_OK) {
    archive->message = "not a supported archive";
    split->Close();
    return ARCHIVER_ERR_FORMAT;
  }
  UInt32 read = 0;
  if (sub->Read(sig, sizeof(sig), &read) != S_OK) {
    archive->message = "not a supported archive";
    split->Close();
    return ARCHIVER_ERR_FORMAT;
  }
  got = read;
  Kind inner = kind_from_signature(sig, got);
  // Volume names locate siblings; the concatenated bytes select the format.
  if (inner == Kind::Unsupported) inner = kind;
  GUID inner_id;
  std::string saved_absent = archive->volumes.next_absent;
  bool have_handler = false;
  if (inner == Kind::Rar) {
    bool rar5 = got >= 8 && is_rar_sig(sig) && sig[6] == 1;
    have_handler = find_handler(archive->engine, rar5 ? "Rar5" : "Rar", inner_id);
    if (!have_handler) archive->message = "7-Zip library has no RAR handler";
  } else {
    have_handler = guid_for_kind(archive->engine, inner, path, 0, inner_id, archive->message);
  }
  if (!have_handler) {
    split->Close();
    return ARCHIVER_ERR_ENGINE;
  }
  CMyComPtr<IInArchive> inner_archive;
  ArchiverResult inner_open =
      open_on_stream(archive->engine, inner_id, sub, password, inner_archive, archive->message);
  if (inner_open != ARCHIVER_OK) {
    split->Close();
    if (!saved_absent.empty() && inner_open != ARCHIVER_ERR_PASSWORD &&
        inner_open != ARCHIVER_ERR_PASSWORD_REQUIRED) {
      archive->message = "missing volume: " + saved_absent;
      return ARCHIVER_ERR_MISSING_VOLUME;
    }
    return inner_open;
  }
  archive->owner_archive = split;
  archive->owner_stream = split_stream;
  archive->archive = inner_archive;
  archive->stream = sub;
  archive->message.clear();
  return ARCHIVER_OK;
}

void close_open_archive(ArchiverArchive *archive) {
  if (archive->archive) archive->archive->Close();
  archive->archive.Release();
  archive->stream.Release();
  archive->entries.clear();
}

/* Open one candidate and keep it only when the handler can list at least one file.
   A signature match alone is not enough. */
ArchiverResult try_listed_archive(ArchiverArchive *archive, Kind kind, const std::string &path,
                                  uint64_t offset, PasswordState &password) {
  GUID clsid;
  if (!guid_for_kind(archive->engine, kind, path, offset, clsid, archive->message)) {
    return ARCHIVER_ERR_ENGINE;
  }
  ArchiverResult opened = open_with(archive->engine, clsid, path, password, archive->archive,
                                     archive->stream, archive->message, offset);
  if (opened != ARCHIVER_OK) return opened;
  std::vector<ListedEntry> probe;
  std::string message;
  ArchiverResult listed = read_entries(archive->archive, probe, message);
  if (listed == ARCHIVER_OK) {
    archive->message.clear();
    return ARCHIVER_OK;
  }
  close_open_archive(archive);
  if (listed == ARCHIVER_ERR_PATH || listed == ARCHIVER_ERR_PASSWORD ||
      listed == ARCHIVER_ERR_PASSWORD_REQUIRED || listed == ARCHIVER_ERR_UNSUPPORTED) {
    archive->message = message;
    return listed;
  }
  return ARCHIVER_ERR_FORMAT;
}

struct SliceHit {
  Kind kind;
  uint64_t offset;
};

void remember_hit(std::vector<SliceHit> &hits, Kind kind, uint64_t offset, size_t max = 0) {
  for (const SliceHit &hit : hits) {
    if (hit.kind == kind && hit.offset == offset) return;
  }
  if (max && hits.size() >= max) hits.erase(hits.begin());
  hits.push_back(SliceHit{kind, offset});
}

/* [ZIP64 end record][ZIP64 locator][classic EOCD]. The ZIP64 record ends
   exactly where the locator starts. Its cd_size/cd_offset are relative to the
   start of the ZIP, so the start is record_pos - cd_size - cd_offset. */
bool zip64_start_from_locator(const std::string &path, uint64_t locator_pos, uint64_t *start) {
  const uint64_t window = 1ull << 20;
  uint64_t begin = locator_pos > window ? locator_pos - window : 0;
  size_t len = static_cast<size_t>(locator_pos - begin);
  if (len < 56) return false;
  std::vector<unsigned char> buf(len);
  size_t got = 0;
  if (!read_at(path, begin, buf.data(), len, &got) || got < 56) return false;
  for (size_t i = got - 4 + 1; i-- > 0;) {
    if (!(buf[i] == 'P' && buf[i + 1] == 'K' && buf[i + 2] == 6 && buf[i + 3] == 6)) continue;
    uint64_t record_size = read_u64(buf.data() + i + 4);
    if (record_size < 44 || record_size > len) continue;
    if (static_cast<uint64_t>(i) + 12 + record_size != len) continue;
    uint64_t cd_size = read_u64(buf.data() + i + 40);
    uint64_t cd_off = read_u64(buf.data() + i + 48);
    uint64_t record_pos = begin + i;
    if (record_pos < cd_size || record_pos - cd_size < cd_off) continue;
    uint64_t origin = record_pos - cd_size - cd_off;
    if (origin < locator_pos) {
      *start = origin;
      return true;
    }
  }
  return false;
}

bool zip_start_from_eocd(const std::string &path, uint64_t file_size, uint64_t *start) {
  if (file_size < 22) return false;
  // The EOCD is normally at the end, but an archive glued into a media file may
  // be followed by container data. Keep a larger search window for that case.
  constexpr uint64_t kEocdWindow = 1ull << 20;
  uint64_t tail = file_size < kEocdWindow ? file_size : kEocdWindow;
  std::vector<unsigned char> end(static_cast<size_t>(tail));
  size_t got = 0;
  if (!read_at(path, file_size - tail, end.data(), static_cast<size_t>(tail), &got) || got < 22) {
    return false;
  }
  for (size_t i = got - 22;; --i) {
    if (end[i] == 'P' && end[i + 1] == 'K' && end[i + 2] == 5 && end[i + 3] == 6) {
      uint16_t comment = read_u16(end.data() + i + 20);
      uint64_t eocd = (file_size - got) + i;
      if (eocd + 22ull + comment <= file_size) {
        uint64_t origin = 0;
        bool have_origin = false;
        if (eocd >= 20) {
          unsigned char locator[20];
          size_t locator_got = 0;
          if (read_at(path, eocd - 20, locator, 20, &locator_got) && locator_got == 20 &&
              locator[0] == 'P' && locator[1] == 'K' && locator[2] == 6 && locator[3] == 7) {
            have_origin = zip64_start_from_locator(path, eocd - 20, &origin);
          }
        }
        if (!have_origin) {
          uint32_t cd_size = read_u32(end.data() + i + 12);
          uint32_t cd_off = read_u32(end.data() + i + 16);
          if (cd_size != 0xFFFFFFFFu && cd_off != 0xFFFFFFFFu) {
            int64_t classic = static_cast<int64_t>(eocd) - static_cast<int64_t>(cd_size) -
                              static_cast<int64_t>(cd_off);
            if (classic >= 0) {
              origin = static_cast<uint64_t>(classic);
              have_origin = true;
            }
          }
        }
        if (have_origin && origin < file_size) {
          *start = origin;
          return true;
        }
      }
    }
    if (i == 0) break;
  }
  return false;
}

void scan_appended_signatures(const std::string &path, uint64_t begin, uint64_t file_size,
                              std::vector<SliceHit> &jpeg, std::vector<SliceHit> &tail) {
  const uint64_t overlap = 16;
  std::vector<unsigned char> buf(1u << 20);
  uint64_t pos = begin;
  while (pos < file_size) {
    size_t want = buf.size();
    if (file_size - pos < want) want = static_cast<size_t>(file_size - pos);
    size_t got = 0;
    if (!read_at(path, pos, buf.data(), want, &got) || got < 6) break;
    for (size_t i = 0; i + 6 <= got; ++i) {
      uint64_t off = pos + i;
      if (off == 0) continue;
      const unsigned char *p = buf.data() + i;
      if (is_7z_sig(p)) remember_hit(tail, Kind::SevenZ, off, 64);
      else if (i + 7 <= got && is_rar_sig(p)) remember_hit(tail, Kind::Rar, off, 64);
      if (p[0] == 0xFF && p[1] == 0xD9) {
        const unsigned char *next = p + 2;
        uint64_t after = off + 2;
        if (i + 6 <= got && is_zip_local(next)) remember_hit(jpeg, Kind::Zip, after, 64);
        else if (i + 8 <= got && is_7z_sig(next)) remember_hit(jpeg, Kind::SevenZ, after, 64);
        else if (i + 9 <= got && is_rar_sig(next)) remember_hit(jpeg, Kind::Rar, after, 64);
      }
    }
    if (pos + got >= file_size) break;
    uint64_t step = got > overlap ? got - overlap : got;
    if (step == 0) break;
    pos += step;
  }
  if (tail.size() > 32) tail.erase(tail.begin(), tail.end() - 32);
}

/* A self-extracting executable keeps a ZIP/7z/RAR payload after its PE stub.
   Scan the beginning of the file as well as the tail; the stub is normally
   much smaller than this window. */
void scan_stub_signatures(const std::string &path, uint64_t file_size, std::vector<SliceHit> &hits) {
  constexpr uint64_t kStubWindow = 8ull << 20;
  const uint64_t end = file_size < kStubWindow ? file_size : kStubWindow;
  const uint64_t overlap = 16;
  std::vector<unsigned char> buf(1u << 20);
  uint64_t pos = 0;
  while (pos < end) {
    size_t want = buf.size();
    if (end - pos < want) want = static_cast<size_t>(end - pos);
    size_t got = 0;
    if (!read_at(path, pos, buf.data(), want, &got) || got < 6) break;
    for (size_t i = 0; i + 6 <= got; ++i) {
      const unsigned char *p = buf.data() + i;
      const uint64_t off = pos + i;
      if (is_7z_sig(p)) remember_hit(hits, Kind::SevenZ, off, 64);
      else if (i + 7 <= got && is_rar_sig(p)) remember_hit(hits, Kind::Rar, off, 64);
      else if (is_zip_local(p)) remember_hit(hits, Kind::Zip, off, 64);
    }
    if (pos + got >= end) break;
    const uint64_t step = got > overlap ? got - overlap : got;
    if (step == 0) break;
    pos += step;
  }
}

ArchiverResult open_disguised(ArchiverArchive *archive, const std::string &path, PasswordState &password,
                              uint64_t scan_window) {
  std::vector<SliceHit> hits;
  unsigned char head[8];
  memset(head, 0, sizeof(head));
  size_t head_n = 0;
  read_at(path, 0, head, sizeof(head), &head_n);
  if (head_n >= 4 && is_zip_local(head)) remember_hit(hits, Kind::Zip, 0);
  else if (head_n >= 6 && is_7z_sig(head)) remember_hit(hits, Kind::SevenZ, 0);
  else if (head_n >= 7 && is_rar_sig(head)) remember_hit(hits, Kind::Rar, 0);

  uint64_t file_size = 0;
  if (!file_size_of(path, &file_size)) {
    archive->message = "could not open archive file";
    return ARCHIVER_ERR_IO;
  }
  uint64_t zip_at = 0;
  if (zip_start_from_eocd(path, file_size, &zip_at)) remember_hit(hits, Kind::Zip, zip_at);

  const std::string base = basename_lower(path);
  if (hits.empty() || hits[0].offset != 0) {
    std::vector<SliceHit> stub;
    scan_stub_signatures(path, file_size, stub);
    for (const SliceHit &hit : stub) remember_hit(hits, hit.kind, hit.offset);
  }

  std::vector<SliceHit> jpeg;
  std::vector<SliceHit> tail;
  /* Header hits are enough for a renamed file. Only a tail window is scanned
     for an archive appended to a large media file. */
  if (hits.empty() || hits[0].offset != 0) {
    const uint64_t scan_from = file_size > scan_window ? file_size - scan_window : 0;
    scan_appended_signatures(path, scan_from, file_size, jpeg, tail);
    for (const SliceHit &hit : jpeg) remember_hit(hits, hit.kind, hit.offset);
    for (size_t i = tail.size(); i > 0; --i) remember_hit(hits, tail[i - 1].kind, tail[i - 1].offset);
  }

  if (hits.empty()) {
    archive->message = "no archive in this file";
    return ARCHIVER_ERR_UNSUPPORTED;
  }
  for (const SliceHit &hit : hits) {
    ArchiverResult result = try_listed_archive(archive, hit.kind, path, hit.offset, password);
    if (result == ARCHIVER_OK) return ARCHIVER_OK;
    if (result == ARCHIVER_ERR_PASSWORD_REQUIRED || result == ARCHIVER_ERR_PASSWORD ||
        result == ARCHIVER_ERR_PATH) {
      return result;
    }
  }
  archive->message = "no archive in this file";
  return ARCHIVER_ERR_UNSUPPORTED;
}

std::string leading_volume(const std::string &path) {
  std::string base = basename_lower(path);
  size_t dot = base.rfind('.');
  if (dot == std::string::npos) return path;
  std::string ext = base.substr(dot + 1);
  if (ext.size() >= 2 && ext.size() <= 4 && all_digits(ext))
    return path.substr(0, path.size() - ext.size()) + std::string(ext.size() - 1, '0') + "1";
  if (ext.size() == 3 && all_digits(ext.substr(1))) {
    if (ext[0] == 'r') return path.substr(0, path.size() - 3) + "rar";
    if (ext[0] == 'z') return path.substr(0, path.size() - 3) + "zip";
  }
  size_t part = base.rfind(".part");
  if (part != std::string::npos && ends_with(base, ".rar")) {
    std::string number = base.substr(part + 5, base.size() - part - 9);
    if (all_digits(number)) {
      size_t start = path.size() - base.size() + part + 5;
      return path.substr(0, start) + std::string(number.size() - 1, '0') + "1" + path.substr(start + number.size());
    }
  }
  return path;
}

// Extension-independent, bounded reads. Weak formats such as LZMA still have
// to pass the actual handler; signatures are only candidates.
Kind content_kind(const std::string &path) {
  unsigned char h[512] = {};
  size_t n = 0;
  if (!read_at(path, 0, h, sizeof(h), &n)) return Kind::Unsupported;
  Kind primary = kind_from_signature(h, n);
  if (primary != Kind::Unsupported) return primary;
  if (n >= 4 && h[0] == 'P' && h[1] == 'K' &&
      ((h[2] == 5 && h[3] == 6) || (h[2] == 6 && h[3] == 6))) return Kind::Zip;
  if (n >= 3 && h[0] == 0x1F && h[1] == 0x8B && h[2] == 8) return Kind::Gzip;
  if (n >= 4 && !memcmp(h, "BZh", 3) && h[3] >= '1' && h[3] <= '9') return Kind::Bzip2;
  if (n >= 6 && !memcmp(h, "\xfd" "7zXZ\0", 6)) return Kind::Xz;
  if (n >= 4 && read_u32(h) == 0xFD2FB528u) return Kind::Zstd;
  if (n >= 4 && (read_u32(h) == 0x184D2204u || read_u32(h) == 0x184C2102u)) return Kind::Lz4;
  // LZ4 and Zstandard share skippable-frame magic. Walk the frame headers
  // without reading their potentially large payload, then select the decoder.
  if (n >= 8 && (read_u32(h) & 0xFFFFFFF0u) == 0x184D2A50u) {
    uint64_t pos = 0, size = 0;
    if (file_size_of(path, &size)) for (unsigned i = 0; i < 1024; ++i) {
      unsigned char frame[8]; size_t got = 0;
      if (!read_at(path, pos, frame, sizeof(frame), &got) || got < 4) break;
      const uint32_t magic = read_u32(frame);
      if (magic == 0xFD2FB528u) return Kind::Zstd;
      if (magic == 0x184D2204u || magic == 0x184C2102u) return Kind::Lz4;
      if (got < 8 || (magic & 0xFFFFFFF0u) != 0x184D2A50u) break;
      const uint64_t skip = 8ull + read_u32(frame + 4);
      if (pos > size || skip > size - pos) break;
      pos += skip;
    }
  }
  if (n >= 3 && h[0] == 0x1F && h[1] == 0x9D) return Kind::Compress;
  if (n >= 8 && !memcmp(h, "MSCF\0\0\0\0", 8)) return Kind::Cab;
  if (n >= 8 && !memcmp(h, "MSWIM\0\0\0", 8)) return Kind::Wim;
  if (n >= 4 && !memcmp(h, "xar!", 4)) return Kind::Xar;
  if (n >= 8 && !memcmp(h, "!<arch>\n", 8)) return Kind::Ar;
  if (n >= 4 && read_u32(h) == 0xDBEEABEDu) return Kind::Rpm;
  if (n >= 2 && h[0] == 0x60 && h[1] == 0xEA) return Kind::Arj;
  if (n >= 7 && h[2] == '-' && h[3] == 'l' && (h[4] == 'h' || h[4] == 'z') && h[6] == '-') return Kind::Lzh;
  if ((n >= 6 && !memcmp(h, "07070", 5)) || (n >= 2 &&
      ((h[0] == 0xC7 && h[1] == 0x71) || (h[0] == 0x71 && h[1] == 0xC7)))) return Kind::Cpio;
  if (n >= 262 && !memcmp(h + 257, "ustar", 5)) return Kind::Tar;
  if (n >= 13 && h[0] < 225) {
    const uint32_t dictionary = read_u32(h + 1);
    const bool valid_dictionary = dictionary >= 4096 && dictionary <= (1u << 30) &&
        (((dictionary & (dictionary - 1)) == 0) ||
         (dictionary % 3 == 0 && ((dictionary / 3) & (dictionary / 3 - 1)) == 0));
    if (valid_dictionary && (read_u64(h + 5) == UINT64_MAX || read_u64(h + 5) <= kWriteCap)) return Kind::Lzma;
  }
  unsigned char marker[6]; size_t got = 0;
  if (read_at(path, 32768, marker, sizeof(marker), &got) && got == 6) {
    if (!memcmp(marker + 1, "CD001", 5)) return Kind::Iso;
    if (!memcmp(marker + 1, "BEA01", 5)) return Kind::Udf;
  }
  uint64_t size = 0;
  if (file_size_of(path, &size) && size >= 512 &&
      read_at(path, size - 512, marker, 4, &got) && got == 4 && !memcmp(marker, "koly", 4)) return Kind::Dmg;
  return Kind::Unsupported;
}

bool carrier_header(const std::string &path) {
  unsigned char h[16] = {}; size_t n = 0;
  read_at(path, 0, h, sizeof(h), &n);
  return (n >= 2 && ((h[0] == 'M' && h[1] == 'Z') || (h[0] == 0xFF && h[1] == 0xD8))) ||
      (n >= 8 && (!memcmp(h, "\x89PNG\r\n\x1a\n", 8) || !memcmp(h + 4, "ftyp", 4))) ||
      (n >= 4 && (!memcmp(h, "RIFF", 4) || !memcmp(h, "GIF8", 4) || read_u32(h) == 0xA3DF451Au));
}

ArchiverResult open_archive_impl(const char *input_path, const char *engine_path,
                                 const char *password, ArchiverArchive **out,
                                 uint64_t scan_window) {
  if (!out) return ARCHIVER_ERR_ARGUMENT;
  *out = nullptr;
  if (!input_path || !input_path[0]) return ARCHIVER_ERR_ARGUMENT;
  const std::string normalized = leading_volume(input_path);
  const char *archive_path = normalized.c_str();
  if (normalized != input_path && !regular_file_exists(normalized)) {
    auto *failed = new ArchiverArchive();
    failed->message = "missing volume: " + base_name_of(normalized);
    *out = failed;
    return ARCHIVER_ERR_MISSING_VOLUME;
  }

  const Kind hint = classify(archive_path);
  Kind detected = content_kind(archive_path);
  Kind kind = detected == Kind::Unsupported ? hint : detected;
  // Preserve the existing transparent tar wrappers when the suffix agrees.
  if ((hint == Kind::TarGz && detected == Kind::Gzip) ||
      (hint == Kind::TarBz2 && detected == Kind::Bzip2) ||
      (hint == Kind::TarXz && detected == Kind::Xz) ||
      (hint == Kind::TarZst && detected == Kind::Zstd) ||
      (hint == Kind::Udf && detected == Kind::Iso)) kind = hint;
  if (kind == Kind::Lz4 && detected != Kind::Lz4) kind = Kind::Unsupported;
  const bool disguise = kind == Kind::Unsupported;

  std::unique_ptr<ArchiverArchive> archive(new ArchiverArchive());
  archive->source_path = archive_path;
  if (kind == Kind::Gzip || kind == Kind::Bzip2 || kind == Kind::Xz || kind == Kind::Zstd ||
      kind == Kind::Lzma || kind == Kind::Compress || kind == Kind::Lz4) {
    std::string name = archive_path;
    size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name.erase(0, slash + 1);
    size_t dot = name.rfind('.');
    if (dot != std::string::npos) name.resize(dot);
    else name += ".out";
    archive->fallback_name = name.empty() ? "content" : name;
  }
  if (kind == Kind::Lz4) {
    if (detected != Kind::Lz4) {
      archive->message = "invalid LZ4 header"; *out = archive.release(); return ARCHIVER_ERR_FORMAT;
    }
    archive->native_lz4 = true;
    ListedEntry entry; entry.path = archive->fallback_name;
    entry.packed_size = 0; file_size_of(archive_path, &entry.packed_size);
    if (archiver::check_entry_path(entry.path).reject != archiver::PathReject::Ok) {
      archive->message = "refused stream filename"; *out = archive.release(); return ARCHIVER_ERR_PATH;
    }
    archive->entries.push_back(entry); *out = archive.release(); return ARCHIVER_OK;
  }
  fill_volume_context(archive->volumes, archive_path);
  PasswordState pw = password_from(password);
  if (!load_engine(archive->engine, engine_path, archive->message)) {
    *out = archive.release();
    return ARCHIVER_ERR_ENGINE;
  }

  ArchiverResult result = ARCHIVER_OK;
  if (disguise) {
    result = open_disguised(archive.get(), archive_path, pw, scan_window);
  } else if (kind == Kind::TarGz || kind == Kind::TarBz2 || kind == Kind::TarXz || kind == Kind::TarZst) {
    result = open_kind(archive.get(), kind, archive_path, pw);
    if (result == ARCHIVER_OK) {
      if (!make_temp_dir(archive->temp_dir)) {
        archive->message = "could not create a temporary directory";
        result = ARCHIVER_ERR_IO;
      } else {
        std::string error;
        std::unique_ptr<archiver::SafeDest> temp = archiver::SafeDest::open(archive->temp_dir, error);
        if (!temp) {
          archive->message = "could not create a temporary directory";
          result = ARCHIVER_ERR_IO;
        } else {
          result = extract_single(archive->archive, *temp, pw, archive->message);
        }
      }
      if (archive->archive) archive->archive->Close();
      archive->archive.Release();
      archive->stream.Release();
      if (result == ARCHIVER_OK) {
        GUID tar_id;
        if (!find_handler(archive->engine, "tar", tar_id)) {
          archive->message = "7-Zip library has no tar handler";
          result = ARCHIVER_ERR_ENGINE;
        } else {
          result = open_with(archive->engine, tar_id, archive->temp_dir + "/inner.bin", pw,
                             archive->archive, archive->stream, archive->message);
          if (result == ARCHIVER_ERR_FORMAT) {
            // A compound extension is still only a hint. A valid plain gzip
            // renamed to .tar.gz must remain readable as a single stream.
            archive->message.clear();
            archive->fallback_name = "content";
            const Kind compression = kind == Kind::TarGz ? Kind::Gzip :
                kind == Kind::TarBz2 ? Kind::Bzip2 : kind == Kind::TarXz ? Kind::Xz : Kind::Zstd;
            result = open_kind(archive.get(), compression, archive_path, pw);
          }
        }
      }
    }
  } else if (numeric_volume_name(archive_path)) {
    result = open_numeric_volume(archive.get(), kind, archive_path, pw);
  } else {
    result = open_kind(archive.get(), kind, archive_path, pw, &archive->volumes);
  }

  if (!disguise && (result == ARCHIVER_ERR_FORMAT || result == ARCHIVER_ERR_UNSUPPORTED)) {
    const ArchiverResult original = result;
    const std::string detail = archive->message;
    close_open_archive(archive.get());
    result = open_disguised(archive.get(), archive_path, pw, scan_window);
    if (result == ARCHIVER_ERR_UNSUPPORTED) { result = original; archive->message = detail; }
  }

  if (result == ARCHIVER_OK && archive->entries.empty()) {
    result = read_entries(archive->archive, archive->entries, archive->message, archive->fallback_name);
    if (result != ARCHIVER_OK) {
      if (archive->archive) archive->archive->Close();
      archive->archive.Release();
      archive->stream.Release();
    }
  }
  *out = archive.release();
  return result;
}

ArchiverResult archiver_open(const char *input_path, const char *engine_path, const char *password,
                             ArchiverArchive **out) {
  return open_archive_impl(input_path, engine_path, password, out, kDisguiseScanWindow);
}

ArchiverResult archiver_entry_count(const ArchiverArchive *archive, uint32_t *count) {
  if (!archive || !count) return ARCHIVER_ERR_ARGUMENT;
  *count = static_cast<uint32_t>(archive->entries.size());
  return ARCHIVER_OK;
}

ArchiverResult archiver_entry_at(const ArchiverArchive *archive, uint32_t index, ArchiverEntry *out) {
  if (!archive || !out) return ARCHIVER_ERR_ARGUMENT;
  if (index >= archive->entries.size()) return ARCHIVER_ERR_ARGUMENT;
  const ListedEntry &entry = archive->entries[index];
  out->path = static_cast<char *>(malloc(entry.path.size() + 1));
  if (!out->path) return ARCHIVER_ERR_IO;
  memcpy(out->path, entry.path.c_str(), entry.path.size() + 1);
  out->is_directory = entry.is_dir ? 1 : 0;
  out->is_symlink = entry.is_symlink ? 1 : 0;
  out->size = entry.size;
  out->packed_size = entry.packed_size;
  out->modified_unix = entry.modified_unix;
  return ARCHIVER_OK;
}

void archiver_entry_free(ArchiverEntry *entry) {
  if (!entry) return;
  free(entry->path);
  entry->path = nullptr;
}

FILE *open_read(const char *path);

ArchiverResult process_lz4(ArchiverArchive *archive, archiver::SafeDest::File *output,
    ArchiverProgressCallback progress, void *user) {
  std::unique_ptr<FILE, decltype(&fclose)> input(open_read(archive->source_path.c_str()), fclose);
  if (!input) { archive->message = "could not read LZ4 file"; return ARCHIVER_ERR_IO; }
  archive->message.clear();
  return archiver::decode_lz4(input.get(), output, progress, user, archive->message);
}

ArchiverResult archiver_extract(ArchiverArchive *archive, const char *dest_dir, const char *password) {
  return archiver_extract_with_progress(archive, dest_dir, password, nullptr, nullptr);
}

ArchiverResult archiver_extract_with_progress(
    ArchiverArchive *archive, const char *dest_dir, const char *password,
    ArchiverProgressCallback progress, void *user) {
  if (!archive || !dest_dir || !dest_dir[0]) return ARCHIVER_ERR_ARGUMENT;
  if (archive->native_lz4) {
    std::string error;
    auto dest = archiver::SafeDest::open(dest_dir, error);
    if (!dest) {
      archive->message = error;
      return error == "symlink-component" ? ARCHIVER_ERR_PATH : ARCHIVER_ERR_IO;
    }
    auto output = dest->create_file(archive->fallback_name, error);
    if (!output) { archive->message = error; return ARCHIVER_ERR_IO; }
    const ArchiverResult result = process_lz4(archive, output.get(), progress, user);
    if (result != ARCHIVER_OK) return result;
    if (!output->commit(error)) { archive->message = error; return ARCHIVER_ERR_IO; }
    return ARCHIVER_OK;
  }
  if (!archive->archive) {
    if (archive->message.empty()) archive->message = "archive is not open";
    return ARCHIVER_ERR_FORMAT;
  }
  PasswordState pw = password_from(password);
  archive->message.clear();
  bool encrypted = archive_has_encrypted_item(archive->archive);
  if (encrypted && !pw.provided) {
    archive->message = "password required";
    return ARCHIVER_ERR_PASSWORD_REQUIRED;
  }
  std::string error;
  std::unique_ptr<archiver::SafeDest> dest = archiver::SafeDest::open(dest_dir, error);
  if (!dest) {
    archive->message = error == "symlink-component" ? "refused destination link"
                                                    : "could not open destination";
    return error == "symlink-component" ? ARCHIVER_ERR_PATH : ARCHIVER_ERR_IO;
  }
  ExtractCtx ctx;
  ctx.password = &pw;
  ctx.dest = dest.get();
  ctx.fallback_name = archive->fallback_name;
  ctx.message = &archive->message;
  ctx.progress = progress;
  ctx.progress_user = user;
  for (const ListedEntry &entry : archive->entries) ctx.total += entry.size;
  if (!ctx.report_progress()) return ARCHIVER_ERR_CANCELLED;
  ExtractCallback *callback = new ExtractCallback();
  callback->archive = archive->archive;
  callback->ctx = &ctx;
  CMyComPtr<IArchiveExtractCallback> held(callback);
  UInt32 all = static_cast<UInt32>(-1);
  HRESULT hr = archive->archive->Extract(nullptr, all, 0, held);
  return from_extract(hr, ctx, pw, encrypted);
}

class ProbeCallback Z7_final : public IArchiveExtractCallback,
                               public ICryptoGetTextPassword,
                               public CMyUnknownImp {
  Z7_IFACES_IMP_UNK_2(IArchiveExtractCallback, ICryptoGetTextPassword)
  Z7_IFACE_COM7_IMP(IProgress)
 public:
  PasswordState *password = nullptr;
  bool wrong = false;
  bool data_error = false;
  bool failed = false;
};

Z7_COM7F_IMF(ProbeCallback::SetTotal(UInt64 /*total*/)) { return S_OK; }
Z7_COM7F_IMF(ProbeCallback::SetCompleted(const UInt64 * /*completeValue*/)) { return S_OK; }
Z7_COM7F_IMF(ProbeCallback::PrepareOperation(Int32 /*askExtractMode*/)) { return S_OK; }
Z7_COM7F_IMF(ProbeCallback::SetOperationResult(Int32 opRes)) {
  if (opRes == NArchive::NExtract::NOperationResult::kWrongPassword) wrong = true;
  else if (opRes == NArchive::NExtract::NOperationResult::kDataError ||
           opRes == NArchive::NExtract::NOperationResult::kCRCError) {
    data_error = true;
  } else if (opRes != NArchive::NExtract::NOperationResult::kOK &&
             opRes != NArchive::NExtract::NOperationResult::kUnavailable) {
    failed = true;
  }
  if (wrong || data_error || failed) return E_ABORT;
  return S_OK;
}
Z7_COM7F_IMF(ProbeCallback::CryptoGetTextPassword(BSTR *passwordOut)) {
  if (!passwordOut) return E_ABORT;
  *passwordOut = nullptr;
  if (!password) return E_ABORT;
  password->asked = true;
  if (!password->provided) return E_ABORT;
  *passwordOut = alloc_wide(password->text.c_str(), password->text.size());
  return *passwordOut ? S_OK : E_OUTOFMEMORY;
}
Z7_COM7F_IMF(ProbeCallback::GetStream(UInt32 /*index*/, ISequentialOutStream **outStream,
                                      Int32 /*askExtractMode*/)) {
  if (outStream) *outStream = nullptr;
  return S_OK;
}

ArchiverResult probe_encrypted(IInArchive *archive, PasswordState &password, std::string &message) {
  if (!archive_has_encrypted_item(archive)) return ARCHIVER_OK;
  UInt32 count = 0;
  if (archive->GetNumberOfItems(&count) != S_OK) {
    message = "could not list archive";
    return ARCHIVER_ERR_FORMAT;
  }
  UInt32 index = 0;
  bool found = false;
  uint64_t smallest = UINT64_MAX;
  for (UInt32 i = 0; i < count; ++i) {
    PROPVARIANT prop;
    memset(&prop, 0, sizeof(prop));
    bool encrypted = false;
    bool is_dir = false;
    if (archive->GetProperty(i, kpidEncrypted, &prop) == S_OK) encrypted = prop_bool(prop);
    prop_clear(&prop);
    memset(&prop, 0, sizeof(prop));
    if (archive->GetProperty(i, kpidIsDir, &prop) == S_OK) is_dir = prop_bool(prop);
    prop_clear(&prop);
    if (encrypted && !is_dir) {
      memset(&prop, 0, sizeof(prop));
      const bool known = archive->GetProperty(i, kpidSize, &prop) == S_OK && prop.vt != VT_EMPTY;
      const uint64_t size = known ? prop_u64(prop) : UINT64_MAX;
      prop_clear(&prop);
      if (!found || size < smallest) { index = i; found = true; smallest = size; }
    }
  }
  if (!found) return ARCHIVER_OK;
  ProbeCallback *callback = new ProbeCallback();
  callback->password = &password;
  CMyComPtr<IArchiveExtractCallback> held(callback);
  HRESULT hr = archive->Extract(&index, 1, 1, held);
  if (callback->wrong || callback->data_error ||
      (callback->failed && password.asked && password.provided)) {
    message = password.provided ? "password rejected" : "password required";
    return password.provided ? ARCHIVER_ERR_PASSWORD : ARCHIVER_ERR_PASSWORD_REQUIRED;
  }
  if (password.asked && !password.provided) {
    message = "password required";
    return ARCHIVER_ERR_PASSWORD_REQUIRED;
  }
  if (hr != S_OK && hr != E_ABORT) {
    message = "not a supported archive";
    return ARCHIVER_ERR_FORMAT;
  }
  if (callback->failed) {
    message = "not a supported archive";
    return ARCHIVER_ERR_FORMAT;
  }
  return ARCHIVER_OK;
}

void copy_message(char *buffer, uint32_t cap, const std::string &text) {
  if (!buffer || cap == 0) return;
  size_t n = text.size();
  if (n >= cap) n = cap - 1;
  if (n) memcpy(buffer, text.data(), n);
  buffer[n] = 0;
}

ArchiverResult archiver_open_with_passwords(const char *archive_path, const char *engine_path,
                                            const char *const *passwords, uint32_t password_count,
                                            int32_t *matched_index, ArchiverArchive **out) {
  if (matched_index) *matched_index = -1;
  if (!out) return ARCHIVER_ERR_ARGUMENT;
  *out = nullptr;
  ArchiverArchive *opened = nullptr;
  ArchiverResult result = archiver_open(archive_path, engine_path, nullptr, &opened);
  if (result == ARCHIVER_OK && opened && (opened->native_lz4 ||
      (opened->archive && !archive_has_encrypted_item(opened->archive)))) {
    *out = opened;
    return ARCHIVER_OK;
  }
  if (result != ARCHIVER_OK && result != ARCHIVER_ERR_PASSWORD &&
      result != ARCHIVER_ERR_PASSWORD_REQUIRED) {
    *out = opened;
    return result;
  }
  const bool encrypted_headers = result == ARCHIVER_ERR_PASSWORD_REQUIRED;
  archiver_close(opened);
  opened = nullptr;
  if (!passwords || password_count == 0) {
    result = archiver_open(archive_path, engine_path, nullptr, out);
    if (result == ARCHIVER_OK && *out && (*out)->archive &&
        archive_has_encrypted_item((*out)->archive)) {
      (*out)->message = "password required";
      return ARCHIVER_ERR_PASSWORD_REQUIRED;
    }
    return result;
  }
  for (uint32_t i = 0; i < password_count; ++i) {
    if (!passwords[i]) continue;
    result = archiver_open(archive_path, engine_path, passwords[i], &opened);
    if (result == ARCHIVER_ERR_PASSWORD || result == ARCHIVER_ERR_PASSWORD_REQUIRED) {
      archiver_close(opened);
      opened = nullptr;
      continue;
    }
    if (result != ARCHIVER_OK) {
      *out = opened;
      return result;
    }
    PasswordState pw = password_from(passwords[i]);
    std::string message;
    // Successful header decryption already verifies this candidate. For ZIP
    // and other visible-header formats, test the smallest encrypted item.
    ArchiverResult probed = encrypted_headers ? ARCHIVER_OK : probe_encrypted(opened->archive, pw, message);
    if (probed == ARCHIVER_ERR_PASSWORD || probed == ARCHIVER_ERR_PASSWORD_REQUIRED) {
      archiver_close(opened);
      opened = nullptr;
      continue;
    }
    if (probed != ARCHIVER_OK) {
      if (!message.empty()) opened->message = message;
      *out = opened;
      return probed;
    }
    archiver_close(opened);
    opened = nullptr;
    result = archiver_open(archive_path, engine_path, passwords[i], &opened);
    if (matched_index && result == ARCHIVER_OK) *matched_index = static_cast<int32_t>(i);
    *out = opened;
    return result;
  }
  result = archiver_open(archive_path, engine_path, nullptr, out);
  if (result == ARCHIVER_OK && *out && (*out)->archive && archive_has_encrypted_item((*out)->archive)) {
    (*out)->message = "password required";
    return ARCHIVER_ERR_PASSWORD_REQUIRED;
  }
  return result;
}

ArchiverResult archiver_test(ArchiverArchive *archive, const char *password) {
  if (!archive) return ARCHIVER_ERR_ARGUMENT;
  if (archive->native_lz4) return process_lz4(archive, nullptr, nullptr, nullptr);
  if (!archive->archive) {
    if (archive->message.empty()) archive->message = "archive is not open";
    return ARCHIVER_ERR_FORMAT;
  }
  PasswordState pw = password_from(password);
  archive->message.clear();
  bool encrypted = archive_has_encrypted_item(archive->archive);
  if (encrypted && !pw.provided) {
    archive->message = "password required";
    return ARCHIVER_ERR_PASSWORD_REQUIRED;
  }
  ExtractCtx ctx;
  ctx.password = &pw;
  ctx.message = &archive->message;
  ExtractCallback *callback = new ExtractCallback();
  callback->archive = archive->archive;
  callback->ctx = &ctx;
  CMyComPtr<IArchiveExtractCallback> held(callback);
  UInt32 all = static_cast<UInt32>(-1);
  HRESULT hr = archive->archive->Extract(nullptr, all, 1, held);
  return from_extract(hr, ctx, pw, encrypted);
}

uint32_t crc32_bytes(const unsigned char *data, size_t n, uint32_t crc) {
  static uint32_t table[256];
  static bool ready = false;
  if (!ready) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int bit = 0; bit < 8; ++bit) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      table[i] = c;
    }
    ready = true;
  }
  crc = ~crc;
  for (size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

void put_u16(unsigned char *p, uint16_t v) {
  p[0] = static_cast<unsigned char>(v);
  p[1] = static_cast<unsigned char>(v >> 8);
}
void put_u32(unsigned char *p, uint32_t v) {
  p[0] = static_cast<unsigned char>(v);
  p[1] = static_cast<unsigned char>(v >> 8);
  p[2] = static_cast<unsigned char>(v >> 16);
  p[3] = static_cast<unsigned char>(v >> 24);
}

FILE *open_write(const char *path) {
#if defined(_WIN32)
  std::wstring wide = utf8_to_wide(path);
  return _wfopen(wide.c_str(), L"wb");
#else
  return fopen(path, "wb");
#endif
}

FILE *open_read(const char *path) {
#if defined(_WIN32)
  std::wstring wide = utf8_to_wide(path);
  return _wfopen(wide.c_str(), L"rb");
#else
  return fopen(path, "rb");
#endif
}

ArchiverResult archiver_create_zip(const char *zip_path, const char *const *files, uint32_t file_count) {
  if (!zip_path || !zip_path[0] || (!files && file_count)) return ARCHIVER_ERR_ARGUMENT;
  struct Item {
    std::string name;
    uint32_t crc = 0;
    uint32_t size = 0;
    uint32_t offset = 0;
  };
  std::vector<Item> items;
  for (uint32_t i = 0; i < file_count; ++i) {
    if (!files[i] || !files[i][0]) return ARCHIVER_ERR_ARGUMENT;
    Item item;
    item.name = base_name_of(files[i]);
    if (item.name.empty() || item.name == "." || item.name == ".." ||
        item.name.find('/') != std::string::npos || item.name.find('\\') != std::string::npos) {
      return ARCHIVER_ERR_PATH;
    }
    for (const Item &prev : items) {
      if (prev.name == item.name) return ARCHIVER_ERR_ARGUMENT;
    }
    items.push_back(item);
  }
  FILE *out = open_write(zip_path);
  if (!out) return ARCHIVER_ERR_IO;
  bool ok = true;
  for (uint32_t i = 0; i < file_count; ++i) {
    FILE *in = open_read(files[i]);
    if (!in) {
      ok = false;
      break;
    }
    std::string data;
    char buf[8192];
    while (ok) {
      size_t n = fread(buf, 1, sizeof(buf), in);
      if (n) data.append(buf, n);
      if (n < sizeof(buf)) {
        if (ferror(in)) ok = false;
        break;
      }
      if (data.size() > kWriteCap) {
        ok = false;
        break;
      }
    }
    fclose(in);
    if (!ok) break;
    items[i].crc = crc32_bytes(reinterpret_cast<const unsigned char *>(data.data()), data.size(), 0);
    items[i].size = static_cast<uint32_t>(data.size());
    items[i].offset = static_cast<uint32_t>(ftell(out));
    unsigned char local[30];
    memset(local, 0, sizeof(local));
    put_u32(local, 0x04034b50u);
    put_u16(local + 4, 20);
    put_u16(local + 6, 1u << 11);
    put_u32(local + 14, items[i].crc);
    put_u32(local + 18, items[i].size);
    put_u32(local + 22, items[i].size);
    put_u16(local + 26, static_cast<uint16_t>(items[i].name.size()));
    if (fwrite(local, 1, 30, out) != 30 ||
        fwrite(items[i].name.data(), 1, items[i].name.size(), out) != items[i].name.size() ||
        (data.size() && fwrite(data.data(), 1, data.size(), out) != data.size())) {
      ok = false;
    }
  }
  long central_at = ftell(out);
  uint32_t central_size = 0;
  for (const Item &item : items) {
    unsigned char central[46];
    memset(central, 0, sizeof(central));
    put_u32(central, 0x02014b50u);
    put_u16(central + 4, 20);
    put_u16(central + 6, 20);
    put_u16(central + 8, 1u << 11);
    put_u32(central + 16, item.crc);
    put_u32(central + 20, item.size);
    put_u32(central + 24, item.size);
    put_u16(central + 28, static_cast<uint16_t>(item.name.size()));
    put_u32(central + 42, item.offset);
    if (fwrite(central, 1, 46, out) != 46 ||
        fwrite(item.name.data(), 1, item.name.size(), out) != item.name.size()) {
      ok = false;
      break;
    }
    central_size += 46u + static_cast<uint32_t>(item.name.size());
  }
  unsigned char end[22];
  memset(end, 0, sizeof(end));
  put_u32(end, 0x06054b50u);
  put_u16(end + 8, static_cast<uint16_t>(items.size()));
  put_u16(end + 10, static_cast<uint16_t>(items.size()));
  put_u32(end + 12, central_size);
  put_u32(end + 16, static_cast<uint32_t>(central_at));
  if (fwrite(end, 1, 22, out) != 22) ok = false;
  if (fclose(out) != 0) ok = false;
  return ok ? ARCHIVER_OK : ARCHIVER_ERR_IO;
}

constexpr int kNestedDepthLimit = 8;
constexpr uint64_t kNestedByteLimit = 4ull << 30;
constexpr uint32_t kNestedFileLimit = 100000;
constexpr uint64_t kNestedRatioLimit = 1000;
constexpr uint64_t kNestedRatioFloor = 1ull << 20;

struct NestedBudget {
  int depth = 0;
  uint64_t bytes = 0;
  uint32_t files = 0;
  std::vector<std::string> secrets;
  std::string report;
  std::string failures;
  uint32_t completed_archives = 0, failed_branches = 0;
  void record(const std::string &line) {
    const bool failed = line.compare(0, 8, "failed [") == 0;
    if (failed) ++failed_branches;
    else if (line.compare(0, 11, "extracted: ") == 0) ++completed_archives;
    std::string &target = failed ? failures : report;
    if (target.size() + line.size() + 1 <= 16384) target += line + "\n";
  }
  ~NestedBudget() { for (auto &secret : secrets) wipe_string(secret); }
};

bool preserve_container(const std::string &path, const ArchiverArchive *archive) {
  const std::string base = basename_lower(path);
  const char *suffixes[] = {".docx", ".docm", ".xlsx", ".xlsm", ".pptx", ".pptm",
      ".odt", ".ods", ".odp", ".epub", ".jar", ".war", ".apk", ".ipa", ".xpi", ".appx", ".msix"};
  for (const char *suffix : suffixes) if (ends_with(base, suffix)) return true;
  if (archive) for (const auto &item : archive->entries) {
    if (item.path == "word/document.xml" || item.path == "xl/workbook.xml" ||
        item.path == "ppt/presentation.xml" || item.path == "META-INF/container.xml" ||
        item.path == "META-INF/MANIFEST.MF" || item.path == "AndroidManifest.xml") return true;
  }
  return false;
}

bool delete_file_at(const std::string &path) {
#if defined(_WIN32)
  std::wstring wide = utf8_to_wide(path.c_str());
  return ::DeleteFileW(wide.c_str()) != 0;
#else
  return ::unlink(path.c_str()) == 0;
#endif
}

// Non-leading volumes are supplied to the handler by the first volume.
bool continuation_volume(const std::string &path) {
  std::string base = basename_lower(path);
  size_t dot = base.rfind('.');
  std::string ext = dot == std::string::npos ? "" : base.substr(dot + 1);
  if (all_digits(ext) && ext.size() >= 2 && ext.size() <= 4)
    return strtoul(ext.c_str(), nullptr, 10) != 1;
  if (ext.size() == 3 && (ext[0] == 'r' || ext[0] == 'z') && all_digits(ext.substr(1)))
    return true;
  size_t part = base.rfind(".part");
  if (part != std::string::npos && ends_with(base, ".rar")) {
    std::string number = base.substr(part + 5, base.size() - part - 9);
    if (all_digits(number)) return strtoul(number.c_str(), nullptr, 10) != 1;
  }
  return false;
}

struct NestedProgress {
  ArchiverProgressCallback callback;
  void *user;
  uint64_t used;
  uint64_t limit;
  bool exceeded = false;
  uint64_t written = 0;
};
int nested_progress(uint64_t completed, uint64_t total, void *user) {
  auto *p = static_cast<NestedProgress *>(user);
  p->written = std::max(p->written, completed);
  if (completed > p->limit - p->used) { p->exceeded = true; return 1; }
  return p->callback ? p->callback(completed, total, p->user) : 0;
}

ArchiverResult nested_extract(const std::string &path, const char *engine, const std::string &dest,
                              const char *const *passwords, uint32_t password_count,
                              const char *chosen, NestedBudget &budget, ArchiverProgressCallback progress,
                              void *progress_user, std::string &message,
                              const ArchiverNestedOptions *options = nullptr) {
  message.clear();
  const uint32_t depth_limit = options ? options->max_depth : kNestedDepthLimit;
  const uint64_t byte_limit = options ? (options->max_output_bytes ? options->max_output_bytes : kWriteCap)
                                     : kNestedByteLimit;
  if (static_cast<uint32_t>(budget.depth) >= depth_limit) {
    message = "nested depth limit";
    return ARCHIVER_ERR_LIMIT;
  }
  if (progress && progress(0, 0, progress_user)) return ARCHIVER_ERR_CANCELLED;
  int32_t matched = -1;
  ArchiverArchive *archive = nullptr;
  std::vector<const char *> candidates;
  for (uint32_t i = 0; passwords && i < password_count; ++i) candidates.push_back(passwords[i]);
  for (const auto &item : budget.secrets) candidates.push_back(item.c_str());
  ArchiverResult opened = archiver_open_with_passwords(
      path.c_str(), engine, candidates.data(), static_cast<uint32_t>(candidates.size()), &matched, &archive);
  std::string entered;
  bool rejected = false;
  while ((opened == ARCHIVER_ERR_PASSWORD_REQUIRED || opened == ARCHIVER_ERR_PASSWORD) &&
         options && options->password) {
    archiver_close(archive);
    archive = nullptr;
    char buffer[4096] = {};
    if (!options->password(path.c_str(), rejected, buffer, sizeof(buffer), options->user)) {
      wipe_bytes(buffer, sizeof(buffer));
      wipe_string(entered);
      return ARCHIVER_ERR_CANCELLED;
    }
    buffer[sizeof(buffer) - 1] = 0;
    wipe_string(entered);
    entered = buffer;
    wipe_bytes(buffer, sizeof(buffer));
    const char *view = entered.c_str();
    opened = archiver_open_with_passwords(path.c_str(), engine, &view, 1, &matched, &archive);
    rejected = true;
  }
  // Own every handle on every exit, including cancellation and limits.
  std::unique_ptr<ArchiverArchive> held(archive);
  if (opened != ARCHIVER_OK) {
    message = archive && !archive->message.empty() ? archive->message : archiver_result_string(opened);
    wipe_string(entered);
    return opened;
  }
  std::vector<ListedEntry> entries = archive->entries;
  uint64_t declared = 0;
  uint32_t count = static_cast<uint32_t>(entries.size());
  for (const ListedEntry &entry : entries) {
    if (entry.size > byte_limit || declared > byte_limit - entry.size) {
      message = "output byte limit"; wipe_string(entered); return ARCHIVER_ERR_LIMIT;
    }
    declared += entry.size;
  }
  if (count > kNestedFileLimit || budget.files > kNestedFileLimit - count) {
    message = "file count limit"; wipe_string(entered); return ARCHIVER_ERR_LIMIT;
  }
  if (budget.bytes > byte_limit - declared) {
    message = "output byte limit"; wipe_string(entered); return ARCHIVER_ERR_LIMIT;
  }
  uint64_t packed = 0;
  file_size_of(archive->source_path, &packed);
  if (declared >= kNestedRatioFloor && packed > 0 && declared / packed > kNestedRatioLimit) {
    message = "compression ratio limit"; wipe_string(entered); return ARCHIVER_ERR_LIMIT;
  }
  const char *password = rejected ? entered.c_str() :
      (matched >= 0 && static_cast<size_t>(matched) < candidates.size() ? candidates[matched] : nullptr);
  NestedProgress reporting{progress, progress_user, budget.bytes, byte_limit};
  ArchiverResult extracted = archiver_extract_with_progress(
      archive, dest.c_str(), password, nested_progress, &reporting);
  if (extracted == ARCHIVER_OK && rejected) {
    budget.secrets.push_back(entered);
    if (options->password_accepted) options->password_accepted(entered.c_str(), options->user);
  }
  wipe_string(entered);
  // Failed branches can already have written completed files. Charge them too
  // before continuing so repeated bad branches cannot evade the shared budget.
  budget.bytes += extracted == ARCHIVER_OK ? std::max(declared, reporting.written) : reporting.written;
  budget.files += count;
  if (extracted != ARCHIVER_OK) {
    message = reporting.exceeded ? "output byte limit" : archive->message;
    return reporting.exceeded ? ARCHIVER_ERR_LIMIT : extracted;
  }
  held.reset();
  budget.record("extracted: " + path);
  ++budget.depth;
  struct DepthGuard { int &depth; ~DepthGuard() { --depth; } } guard{budget.depth};
  if (options && static_cast<uint32_t>(budget.depth) >= depth_limit) return ARCHIVER_OK;

  std::vector<std::string> archives;
  // Inspect only this layer's extracted entries. Never unpack unrelated files
  // already present in the destination or traverse extracted symbolic links.
  for (const auto &entry : entries) {
    const std::string &rel = entry.path;
    if (entry.is_dir || entry.is_symlink) continue;
    if (continuation_volume(rel)) {
      const std::string first = leading_volume(rel);
      const bool present = std::any_of(entries.begin(), entries.end(), [&](const ListedEntry &item) {
        return !item.is_dir && !item.is_symlink && item.path == first;
      });
      if (present) continue;
      archives.push_back(rel);
      continue;
    }
    const std::string full = join_dir(dest, rel);
    const Kind hint = classify(rel), detected = content_kind(full);
    if (hint == Kind::Unsupported && detected == Kind::Unsupported &&
        !is_disguise_carrier(rel) && !carrier_header(full)) continue;
    if (progress && progress(0, 0, progress_user)) return ARCHIVER_ERR_CANCELLED;
    ArchiverArchive *inner = nullptr;
    ArchiverResult trial = open_archive_impl(full.c_str(), engine, nullptr, &inner, kDisguiseScanWindow);
    bool is_archive = trial == ARCHIVER_OK || trial == ARCHIVER_ERR_PASSWORD_REQUIRED ||
                      trial == ARCHIVER_ERR_PASSWORD || trial == ARCHIVER_ERR_MISSING_VOLUME;
    if (trial == ARCHIVER_ERR_PATH || trial == ARCHIVER_ERR_ENGINE) {
      message = rel + ": " + (inner ? inner->message : archiver_result_string(trial));
      archiver_close(inner);
      return trial;
    }
    if (trial == ARCHIVER_OK && preserve_container(rel, inner)) {
      budget.record("kept container: " + rel);
      archiver_close(inner);
      continue;
    }
    archiver_close(inner);
    if (is_archive || hint != Kind::Unsupported || detected != Kind::Unsupported) archives.push_back(rel);
  }
  if (archives.empty()) return ARCHIVER_OK;
  int decision = archives.size() == 1 ? 0 : -4;
  if (options && options->all_branches) decision = -3;
  if (options && options->choose) {
    std::vector<const char *> views;
    for (const auto &item : archives) views.push_back(item.c_str());
    decision = options->choose(views.data(), static_cast<uint32_t>(views.size()), options->user);
  } else if (chosen && chosen[0]) {
    for (size_t i = 0; i < archives.size(); ++i) if (archives[i] == chosen) decision = static_cast<int>(i);
  }
  if (decision == -1) return ARCHIVER_OK;
  if (decision == -2) return ARCHIVER_ERR_CANCELLED;
  if (decision != -3 && (decision < 0 || static_cast<size_t>(decision) >= archives.size())) {
    message = "choose archive:";
    for (const auto &rel : archives) message += "\n" + rel;
    return ARCHIVER_ERR_NEED_CHOICE;
  }
  ArchiverResult first_error = ARCHIVER_OK;
  bool partial_branch = false;
  size_t completed_branches = 0;
  for (size_t i = 0; i < archives.size(); ++i) {
    if (decision != -3 && static_cast<size_t>(decision) != i) continue;
    std::string next_dest = dest;
    if (archives.size() > 1) {
      std::string error;
      auto safe = archiver::SafeDest::open(dest, error);
      std::string folder = archives[i] + ".unpacked";
      if (!safe || !safe->mkdir_entry(folder, error)) {
        message = "could not create branch output directory";
        return ARCHIVER_ERR_IO;
      }
      next_dest = join_dir(dest, folder);
    }
    std::string full = join_dir(dest, archives[i]);
    ArchiverResult deeper = nested_extract(full, engine, next_dest, passwords, password_count,
        nullptr, budget, progress, progress_user, message, options);
    if (deeper != ARCHIVER_OK) {
      budget.record("failed [" + std::string(archiver_result_string(deeper)) + "]: " + archives[i] + ": " + message);
      if (decision != -3 || deeper == ARCHIVER_ERR_CANCELLED || deeper == ARCHIVER_ERR_LIMIT ||
          deeper == ARCHIVER_ERR_PATH || deeper == ARCHIVER_ERR_ENGINE) return deeper;
      if (first_error == ARCHIVER_OK) first_error = deeper;
      partial_branch = partial_branch || deeper == ARCHIVER_ERR_PARTIAL;
      continue;
    }
    ++completed_branches;
    // Keep all volume parts together; never leave a broken set by deleting only .001.
    if ((!options || !options->keep_intermediate) && split_kind(basename_lower(full)) == Kind::Unsupported)
      delete_file_at(full);
  }
  if (first_error != ARCHIVER_OK) {
    return completed_branches || partial_branch ? ARCHIVER_ERR_PARTIAL : first_error;
  }
  return ARCHIVER_OK;
}

ArchiverResult archiver_extract_nested(const char *archive_path, const char *engine_path,
                                       const char *dest_dir, const char *const *passwords,
                                       uint32_t password_count, const char *chosen_inner,
                                       ArchiverProgressCallback progress, void *progress_user,
                                       char *message_buf, uint32_t message_cap) {
  if (!archive_path || !archive_path[0] || !dest_dir || !dest_dir[0]) return ARCHIVER_ERR_ARGUMENT;
  NestedBudget budget;
  std::string message;
  ArchiverResult result = nested_extract(archive_path, engine_path, dest_dir, passwords, password_count,
                                          chosen_inner, budget, progress, progress_user, message);
  copy_message(message_buf, message_cap, message);
  return result;
}

ArchiverResult archiver_extract_nested_ex(const char *archive_path, const char *engine_path,
    const char *dest_dir, const char *const *passwords, uint32_t password_count,
    const ArchiverNestedOptions *options, ArchiverProgressCallback progress, void *progress_user,
    char *message_buf, uint32_t message_cap) {
  if (!archive_path || !archive_path[0] || !dest_dir || !dest_dir[0] || !options ||
      options->max_depth < 1 || options->max_depth > 32 || options->max_output_bytes > kWriteCap)
    return ARCHIVER_ERR_ARGUMENT;
  NestedBudget budget;
  std::string message;
  ArchiverResult result = nested_extract(archive_path, engine_path, dest_dir, passwords, password_count,
      nullptr, budget, progress, progress_user, message, options);
  if (!budget.report.empty() || !budget.failures.empty()) {
    message = "archives extracted: " + std::to_string(budget.completed_archives) +
        "\nfailed branch reports: " + std::to_string(budget.failed_branches) + "\n" +
        budget.failures + (message.empty() ? "" : "detail: " + message + "\n") + budget.report;
  }
  copy_message(message_buf, message_cap, message);
  return result;
}

void archiver_close(ArchiverArchive *archive) { delete archive; }
