#include "archiver.h"
#include "utf.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <iostream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

namespace {

void wipe(std::string &s) {
  if (!s.empty()) {
    volatile char *p = &s[0];
    for (size_t i = 0; i < s.size(); ++i) p[i] = 0;
  }
  s.clear();
}

void wipe_bytes_local(void *data, size_t size) {
  volatile unsigned char *p = static_cast<volatile unsigned char *>(data);
  while (size--) *p++ = 0;
}

void print_result(ArchiverResult result, const ArchiverArchive *archive) {
  const char *detail = archiver_message(archive);
  switch (result) {
    case ARCHIVER_OK:
      std::cout << "完成\n";
      break;
    case ARCHIVER_ERR_PASSWORD_REQUIRED:
      std::cout << "需要密码。请输入，程序不会保存。\n";
      break;
    case ARCHIVER_ERR_PASSWORD:
      std::cout << "密码不对。\n";
      break;
    case ARCHIVER_ERR_PATH:
      std::cout << "已拒绝不安全的路径。\n";
      break;
    case ARCHIVER_ERR_UNSUPPORTED:
      std::cout << "这个文件打不开。\n";
      break;
    case ARCHIVER_ERR_MISSING_VOLUME:
      std::cout << "缺少分卷。\n";
      break;
    case ARCHIVER_ERR_NEED_CHOICE:
      std::cout << "这一层有多个压缩包，需要指定一个。\n";
      break;
    case ARCHIVER_ERR_LIMIT:
      std::cout << "已达到嵌套解压上限。\n";
      break;
    case ARCHIVER_ERR_CANCELLED:
      std::cout << "已取消。\n";
      break;
    case ARCHIVER_ERR_FORMAT:
      std::cout << "打不开这个压缩包。\n";
      break;
    case ARCHIVER_ERR_PARTIAL:
      std::cout << "部分解压完成，以下分支未能全部解开。\n";
      break;
    case ARCHIVER_ERR_ENGINE:
      std::cout << "找不到 7-Zip 动态库。\n";
      break;
    case ARCHIVER_ERR_IO:
      std::cout << "读写文件失败。\n";
      break;
    default:
      std::cout << "失败。\n";
      break;
  }
  if (detail && detail[0]) std::cout << detail << "\n";
}

bool read_password(std::string &out) {
#if defined(_WIN32)
  HANDLE input = ::GetStdHandle(STD_INPUT_HANDLE);
  DWORD mode = 0;
  if (::GetConsoleMode(input, &mode)) {
    if (!::SetConsoleMode(input, mode & ~ENABLE_ECHO_INPUT)) return false;
    std::fputs("密码（只用于这一次，不会保存）: ", stderr);
    wchar_t buffer[256];
    std::wstring wide;
    bool ok = true;
    for (;;) {
      DWORD count = 0;
      BOOL read = ::ReadConsoleW(input, buffer, 256, &count, nullptr);
      if (!read || count == 0) {
        ::SecureZeroMemory(buffer, sizeof(buffer));
        ok = false;
        break;
      }
      wide.append(buffer, count);
      volatile wchar_t *clear = buffer;
      for (DWORD i = 0; i < count; ++i) clear[i] = 0;
      if (wide.find(L'\n') != std::wstring::npos) break;
    }
    ::SetConsoleMode(input, mode);
    std::fputc('\n', stderr);
    while (!wide.empty() && (wide.back() == L'\n' || wide.back() == L'\r')) wide.pop_back();
    if (ok) out = archiver::utf16_to_utf8(wide.data(), wide.size());
    volatile wchar_t *clear = wide.data();
    for (size_t i = 0; i < wide.size(); ++i) clear[i] = 0;
    return ok;
  }
#else
  if (isatty(STDIN_FILENO)) {
    std::fputs("密码（只用于这一次，不会保存）: ", stderr);
    termios old_term;
    if (tcgetattr(STDIN_FILENO, &old_term) == 0) {
      termios hidden = old_term;
      hidden.c_lflag = static_cast<tcflag_t>(hidden.c_lflag & ~ECHO);
      tcsetattr(STDIN_FILENO, TCSANOW, &hidden);
      std::getline(std::cin, out);
      tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
      std::fputc('\n', stderr);
      return !out.empty() || std::cin.good();
    }
  }
#endif
  if (!std::getline(std::cin, out)) return false;
  if (!out.empty() && out.back() == '\r') out.pop_back();
  return true;
}

void usage() {
  std::cerr
      << "用法:\n"
      << "  archiver-cli list <压缩包> [--engine 7z.so]\n"
      << "  archiver-cli extract <压缩包> <目标文件夹> [--engine 7z.so]\n"
      << "  archiver-cli test <压缩包> [--engine 7z.so]\n"
      << "  archiver-cli try-passwords <压缩包> <密码文件> [--engine 7z.so]\n"
      << "  archiver-cli nested <压缩包> <目标文件夹> [密码文件] [--all] [--depth 1..32] [--keep] [--engine 7z.so]\n"
      << "  archiver-cli create-zip <压缩包> <文件>... [--engine 7z.so]\n"
      << "需要密码时从键盘输入。密码不会写入文件，也不会出现在命令行参数里。\n"
      << "try-passwords 只按文件里的顺序做头部校验，不会询问，也不会打印密码。\n"
      << "不会运行解压出来的程序。\n";
}

const char *take_engine(int &i, int argc, char **argv) {
  if (i + 1 < argc && strcmp(argv[i], "--engine") == 0) {
    i += 1;
    return argv[i];
  }
  return nullptr;
}

bool load_password_file(const char *path, std::vector<std::string> &out) {
  FILE *file = fopen(path, "rb");
  if (!file) return false;
  std::string current;
  char buf[256];
  size_t n = 0;
  while ((n = fread(buf, 1, sizeof(buf), file)) > 0) {
    current.append(buf, n);
    wipe_bytes_local(buf, n);
  }
  fclose(file);
  std::string line;
  for (char c : current) {
    if (c == '\n') {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (!line.empty()) out.push_back(line);
      wipe(line);
    } else {
      line.push_back(c);
    }
  }
  if (!line.empty() && line.back() == '\r') line.pop_back();
  if (!line.empty()) out.push_back(line);
  wipe(line);
  wipe(current);
  return true;
}

const char *engine_after(int argc, char **argv, int index) {
  const char *engine = nullptr;
  for (int i = index; i < argc; ++i) {
    const char *found = take_engine(i, argc, argv);
    if (!found) return nullptr;
    engine = found;
  }
  return engine;
}

int cmd_try_passwords(int argc, char **argv) {
  if (argc < 4) {
    usage();
    return ARCHIVER_ERR_ARGUMENT;
  }
  const char *engine = engine_after(argc, argv, 4);
  if (argc > 4 && !engine && strcmp(argv[4], "--engine") != 0) {
    usage();
    return ARCHIVER_ERR_ARGUMENT;
  }
  std::vector<std::string> passwords;
  if (!load_password_file(argv[3], passwords)) return ARCHIVER_ERR_IO;
  std::vector<const char *> views;
  for (std::string &password : passwords) views.push_back(password.c_str());
  int32_t matched = -1;
  ArchiverArchive *archive = nullptr;
  ArchiverResult result = archiver_open_with_passwords(
      argv[2], engine, views.empty() ? nullptr : views.data(), static_cast<uint32_t>(views.size()),
      &matched, &archive);
  if (result == ARCHIVER_OK) std::cout << "matched " << matched << "\n";
  else print_result(result, archive);
  archiver_close(archive);
  for (std::string &password : passwords) wipe(password);
  return result;
}

int cmd_nested(int argc, char **argv) {
  if (argc < 4) {
    usage();
    return ARCHIVER_ERR_ARGUMENT;
  }
  const char *password_file = nullptr;
  const char *engine = nullptr;
  bool extended = false;
  ArchiverNestedOptions options = {};
  options.max_depth = 32;
  options.all_branches = 1;
  for (int i = 4; i < argc; ++i) {
    if (strcmp(argv[i], "--engine") == 0) {
      if (i + 1 >= argc) return ARCHIVER_ERR_ARGUMENT;
      engine = argv[++i];
    } else if (strcmp(argv[i], "--all") == 0) {
      extended = true;
    } else if (strcmp(argv[i], "--keep") == 0) {
      extended = true;
      options.keep_intermediate = 1;
    } else if (strcmp(argv[i], "--depth") == 0) {
      if (++i >= argc) return ARCHIVER_ERR_ARGUMENT;
      char *end = nullptr;
      long depth = strtol(argv[i], &end, 10);
      if (!end || *end || depth < 1 || depth > 32) return ARCHIVER_ERR_ARGUMENT;
      options.max_depth = static_cast<uint32_t>(depth);
      extended = true;
    } else if (!password_file && argv[i][0] != '-') {
      password_file = argv[i];
    } else {
      usage();
      return ARCHIVER_ERR_ARGUMENT;
    }
  }
  std::vector<std::string> passwords;
  if (password_file && !load_password_file(password_file, passwords)) return ARCHIVER_ERR_IO;
  std::vector<const char *> views;
  for (std::string &password : passwords) views.push_back(password.c_str());
  char message[1024];
  message[0] = 0;
  ArchiverResult result = extended ? archiver_extract_nested_ex(
      argv[2], engine, argv[3], views.empty() ? nullptr : views.data(),
      static_cast<uint32_t>(views.size()), &options, nullptr, nullptr, message, sizeof(message)) :
      archiver_extract_nested(argv[2], engine, argv[3], views.empty() ? nullptr : views.data(),
      static_cast<uint32_t>(views.size()), nullptr, nullptr, nullptr, message, sizeof(message));
  print_result(result, nullptr);
  if (message[0]) std::cout << message << "\n";
  for (std::string &password : passwords) wipe(password);
  return result;
}

int cmd_test_archive(int argc, char **argv) {
  if (argc < 3) {
    usage();
    return ARCHIVER_ERR_ARGUMENT;
  }
  const char *engine = engine_after(argc, argv, 3);
  std::string password;
  ArchiverArchive *archive = nullptr;
  ArchiverResult result = archiver_open(argv[2], engine, nullptr, &archive);
  if (result == ARCHIVER_ERR_PASSWORD_REQUIRED) {
    archiver_close(archive);
    archive = nullptr;
    if (!read_password(password)) return result;
    result = archiver_open(argv[2], engine, password.c_str(), &archive);
  }
  if (result == ARCHIVER_OK) result = archiver_test(archive, password.empty() ? nullptr : password.c_str());
  print_result(result, archive);
  archiver_close(archive);
  wipe(password);
  return result;
}

int cmd_create_zip(int argc, char **argv) {
  if (argc < 4) {
    usage();
    return ARCHIVER_ERR_ARGUMENT;
  }
  std::vector<const char *> files;
  const char *engine = nullptr;
  for (int i = 3; i < argc; ++i) {
    if (strcmp(argv[i], "--engine") == 0) {
      if (i + 1 >= argc) return ARCHIVER_ERR_ARGUMENT;
      engine = argv[++i];
      (void)engine;
    } else {
      files.push_back(argv[i]);
    }
  }
  ArchiverResult result = archiver_create_zip(argv[2], files.data(), static_cast<uint32_t>(files.size()));
  print_result(result, nullptr);
  return result;
}

}  // namespace

int run_main(int argc, char **argv) {
  if (argc >= 2 && strcmp(argv[1], "try-passwords") == 0) return cmd_try_passwords(argc, argv);
  if (argc >= 2 && strcmp(argv[1], "nested") == 0) return cmd_nested(argc, argv);
  if (argc >= 2 && strcmp(argv[1], "test") == 0) return cmd_test_archive(argc, argv);
  if (argc >= 2 && strcmp(argv[1], "create-zip") == 0) return cmd_create_zip(argc, argv);
  if (argc < 3) {
    usage();
    return ARCHIVER_ERR_ARGUMENT;
  }
  const char *cmd = argv[1];
  const char *archive_path = argv[2];
  const char *dest = nullptr;
  const char *engine = nullptr;
  int index = 3;
  if (strcmp(cmd, "extract") == 0) {
    if (argc < 4) {
      usage();
      return ARCHIVER_ERR_ARGUMENT;
    }
    dest = argv[3];
    index = 4;
  } else if (strcmp(cmd, "list") != 0) {
    usage();
    return ARCHIVER_ERR_ARGUMENT;
  }
  for (; index < argc; ++index) {
    const char *found = take_engine(index, argc, argv);
    if (!found) {
      usage();
      return ARCHIVER_ERR_ARGUMENT;
    }
    engine = found;
  }

  std::string password;
  bool have_password = false;
  ArchiverArchive *archive = nullptr;
  const char *pw = nullptr;
  ArchiverResult result = archiver_open(archive_path, engine, pw, &archive);
  if (result == ARCHIVER_ERR_PASSWORD_REQUIRED) {
    archiver_close(archive);
    archive = nullptr;
    if (!read_password(password)) {
      wipe(password);
      print_result(result, nullptr);
      return result;
    }
    have_password = true;
    result = archiver_open(archive_path, engine, password.c_str(), &archive);
  }
  if (result != ARCHIVER_OK) {
    print_result(result, archive);
    archiver_close(archive);
    wipe(password);
    return result;
  }

  if (strcmp(cmd, "list") == 0) {
    uint32_t count = 0;
    archiver_entry_count(archive, &count);
    for (uint32_t i = 0; i < count; ++i) {
      ArchiverEntry entry;
      memset(&entry, 0, sizeof(entry));
      if (archiver_entry_at(archive, i, &entry) != ARCHIVER_OK) continue;
      std::cout << (entry.is_directory ? "dir " : entry.is_symlink ? "link " : "file ")
                << entry.path << "\n";
      archiver_entry_free(&entry);
    }
    archiver_close(archive);
    wipe(password);
    return ARCHIVER_OK;
  }

  result = archiver_extract(archive, dest, have_password ? password.c_str() : nullptr);
  if (result == ARCHIVER_ERR_PASSWORD_REQUIRED) {
    if (!read_password(password)) {
      print_result(result, archive);
      archiver_close(archive);
      wipe(password);
      return result;
    }
    have_password = true;
    result = archiver_extract(archive, dest, password.c_str());
  }
  print_result(result, archive);
  archiver_close(archive);
  wipe(password);
  return result;
}

#if defined(_WIN32)
int wmain(int argc, wchar_t **wide_argv) {
  std::vector<std::string> args;
  std::vector<char *> argv;
  for (int i = 0; i < argc; ++i) {
    args.push_back(archiver::utf16_to_utf8(wide_argv[i], wcslen(wide_argv[i])));
  }
  for (auto &arg : args) argv.push_back(&arg[0]);
  argv.push_back(nullptr);
  return run_main(argc, argv.data());
}
#else
int main(int argc, char **argv) { return run_main(argc, argv); }
#endif
