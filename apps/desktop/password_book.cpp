#include "password_book.hpp"
#include <QUuid>
#include <stdint.h>
#include <string.h>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincred.h>
#endif

namespace {
#if defined(_WIN32)
constexpr uint32_t kMagic = 0x3142504cu; // legacy LPB1
constexpr uint32_t kMagic2 = 0x3242504cu;
/* Keep the pre-rename credential target so passwords saved by older versions
   survive the MilkZip rename. */
#ifdef ARCHIVER_PASSWORD_BOOK_TEST
const wchar_t *kTarget = L"local-archiver/tests/password-book";
#else
const wchar_t *kTarget = L"local-archiver/book";
#endif
void wipe(QByteArray &bytes) {
  volatile char *data = bytes.data();
  for (qsizetype i = 0; i < bytes.size(); ++i) data[i] = 0;
  bytes.clear();
}
QByteArray readCredential(const std::wstring &target) {
  PCREDENTIALW cred = nullptr;
  if (!::CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &cred) || !cred) return {};
  QByteArray bytes(reinterpret_cast<const char *>(cred->CredentialBlob), cred->CredentialBlobSize);
  if (cred->CredentialBlobSize) ::SecureZeroMemory(cred->CredentialBlob, cred->CredentialBlobSize);
  ::CredFree(cred);
  return bytes;
}
bool writeCredential(const std::wstring &target, const QByteArray &bytes) {
  CREDENTIALW cred = {};
  cred.Type = CRED_TYPE_GENERIC;
  cred.TargetName = const_cast<LPWSTR>(target.c_str());
  cred.CredentialBlobSize = static_cast<DWORD>(bytes.size());
  cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char *>(bytes.constData()));
  cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
  cred.UserName = const_cast<LPWSTR>(L"local-archiver");
  return ::CredWriteW(&cred, 0) != 0;
}
std::wstring entryTarget(const QByteArray &generation, uint32_t i) {
  return (QString::fromWCharArray(kTarget) + QStringLiteral("/%1/%2")).arg(QString::fromLatin1(generation)).arg(i).toStdWString();
}
bool manifest(const QByteArray &bytes, QByteArray &generation, uint32_t &count) {
  if (bytes.size() != 44) return false;
  uint32_t magic = 0;
  memcpy(&magic, bytes.constData(), 4);
  memcpy(&count, bytes.constData() + 4, 4);
  generation = bytes.mid(8);
  if (magic != kMagic2 || count > PasswordBook::maxPasswords) return false;
  const QUuid uuid(QString::fromLatin1(generation));
  return !uuid.isNull() && uuid.toString(QUuid::WithoutBraces).toLatin1() == generation;
}
void removeGeneration(const QByteArray &generation, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i)
    ::CredDeleteW(entryTarget(generation, i).c_str(), CRED_TYPE_GENERIC, 0);
}
#endif
}

bool PasswordBook::available() {
#if defined(_WIN32)
  return true;
#else
  return false;
#endif
}

QVector<QByteArray> PasswordBook::load() {
  QVector<QByteArray> out;
#if defined(_WIN32)
  QByteArray bytes = readCredential(kTarget);
  QByteArray generation;
  uint32_t count = 0;
  if (manifest(bytes, generation, count)) {
    for (uint32_t i = 0; i < count; ++i) {
      QByteArray password = readCredential(entryTarget(generation, i));
      if (password.isEmpty() || password.size() > maxPasswordBytes) {
        for (auto &item : out) wipe(item);
        out.clear();
        wipe(password);
        break;
      }
      out.append(password);
    }
  } else if (bytes.size() >= 8) {
    // Read the original single-credential book; the next store migrates it.
    uint32_t magic = 0;
    memcpy(&magic, bytes.constData(), 4);
    memcpy(&count, bytes.constData() + 4, 4);
    qsizetype offset = 8;
    if (magic == kMagic && count <= maxPasswords) {
      bool valid = true;
      for (uint32_t i = 0; i < count; ++i) {
        uint32_t len = 0;
        if (offset + 4 > bytes.size()) { valid = false; break; }
        memcpy(&len, bytes.constData() + offset, 4);
        offset += 4;
        if (!len || len > maxPasswordBytes || offset + len > bytes.size()) { valid = false; break; }
        out.append(bytes.mid(offset, len));
        offset += len;
      }
      if (!valid || offset != bytes.size()) {
        for (auto &item : out) wipe(item);
        out.clear();
      }
    }
  }
  wipe(bytes);
#endif
  return out;
}

bool PasswordBook::store(const QVector<QByteArray> &passwords) {
#if defined(_WIN32)
  if (passwords.size() > maxPasswords) return false;
  for (const auto &password : passwords)
    if (password.isEmpty() || password.size() > maxPasswordBytes || password.contains('\0')) return false;
  QByteArray old = readCredential(kTarget);
  QByteArray oldGeneration;
  uint32_t oldCount = 0;
  const bool hasOld = manifest(old, oldGeneration, oldCount);
  const QByteArray generation = QUuid::createUuid().toString(QUuid::WithoutBraces).toLatin1();
  uint32_t count = static_cast<uint32_t>(passwords.size());
  for (uint32_t i = 0; i < count; ++i) {
    if (!writeCredential(entryTarget(generation, i), passwords[i])) {
      removeGeneration(generation, i);
      wipe(old);
      return false;
    }
  }
  QByteArray bytes(44, '\0');
  memcpy(bytes.data(), &kMagic2, 4);
  memcpy(bytes.data() + 4, &count, 4);
  memcpy(bytes.data() + 8, generation.constData(), 36);
  const bool ok = writeCredential(kTarget, bytes);
  if (ok && hasOld) removeGeneration(oldGeneration, oldCount);
  if (!ok) removeGeneration(generation, count);
  wipe(old);
  wipe(bytes);
  return ok;
#else
  (void)passwords;
  return false;
#endif
}
