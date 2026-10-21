#include "extract_path.hpp"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

namespace {
constexpr int kMaxComponent = 80;
constexpr int kMaxPath = 240;

bool endsWith(const QString &text, const QString &suffix) {
  return text.size() > suffix.size() && text.endsWith(suffix, Qt::CaseInsensitive);
}

QString stripArchiveSuffix(QString name) {
  // Remove a split-volume suffix before stripping the underlying format.
  name.remove(QRegularExpression(QStringLiteral("[.](?:[0-9]{2,4}|[rz][0-9]{2})$"), QRegularExpression::CaseInsensitiveOption));
  name.remove(QRegularExpression(QStringLiteral("[.]part[0-9]+(?=[.]rar$)"), QRegularExpression::CaseInsensitiveOption));
  const QStringList suffixes = {
      QStringLiteral(".tar.gz"), QStringLiteral(".tar.bz2"), QStringLiteral(".tar.xz"),
      QStringLiteral(".tar.zst"), QStringLiteral(".tbz2"), QStringLiteral(".tgz"),
      QStringLiteral(".tbz"), QStringLiteral(".txz"), QStringLiteral(".tzst"),
      QStringLiteral(".zip"), QStringLiteral(".7z"), QStringLiteral(".tar"),
      QStringLiteral(".rar"), QStringLiteral(".mp4"), QStringLiteral(".m4v"),
      QStringLiteral(".mov"), QStringLiteral(".jpg"), QStringLiteral(".jpeg"),
      QStringLiteral(".png"), QStringLiteral(".gif"), QStringLiteral(".bmp"),
      QStringLiteral(".webp"), QStringLiteral(".mkv"), QStringLiteral(".avi"),
      QStringLiteral(".webm"), QStringLiteral(".gz"), QStringLiteral(".bz2"),
      QStringLiteral(".xz"), QStringLiteral(".zst"), QStringLiteral(".zstd"),
      QStringLiteral(".lz4"), QStringLiteral(".lzma"), QStringLiteral(".z"),
      QStringLiteral(".cab"), QStringLiteral(".iso"), QStringLiteral(".udf"),
      QStringLiteral(".wim"), QStringLiteral(".swm"), QStringLiteral(".arj"),
      QStringLiteral(".lzh"), QStringLiteral(".lha"), QStringLiteral(".cpio"),
      QStringLiteral(".ar"), QStringLiteral(".deb"), QStringLiteral(".rpm"),
      QStringLiteral(".dmg"), QStringLiteral(".xar")};
  bool stripped = true;
  while (stripped) {
    stripped = false;
    for (const QString &suffix : suffixes) {
      if (!endsWith(name, suffix)) continue;
      name.chop(suffix.size());
      stripped = true;
      break;
    }
  }
  return name;
}

bool reservedDeviceName(const QString &name) {
  QString stem = name;
  const int dot = stem.indexOf(QLatin1Char('.'));
  if (dot > 0) stem = stem.left(dot);
  stem = stem.toUpper();
  const QStringList reserved = {
      QStringLiteral("CON"), QStringLiteral("PRN"), QStringLiteral("AUX"), QStringLiteral("NUL"),
      QStringLiteral("COM1"), QStringLiteral("COM2"), QStringLiteral("COM3"), QStringLiteral("COM4"),
      QStringLiteral("COM5"), QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
      QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"), QStringLiteral("LPT3"),
      QStringLiteral("LPT4"), QStringLiteral("LPT5"), QStringLiteral("LPT6"), QStringLiteral("LPT7"),
      QStringLiteral("LPT8"), QStringLiteral("LPT9")};
  return reserved.contains(stem);
}

QString shortName() { return QStringLiteral("解压文件"); }

QString parentOf(const QString &path) {
  const QFileInfo info(path);
  const QString parent = info.dir().absolutePath();
  return parent.isEmpty() ? QDir::rootPath() : parent;
}
}

QString sanitizeFolderComponent(const QString &original) {
  QString name = stripArchiveSuffix(original.trimmed());
  const QString illegal = QStringLiteral("\\/:*?\"<>|");
  for (QChar &ch : name) {
    if (ch.unicode() < 32 || illegal.contains(ch)) ch = QLatin1Char('_');
  }
  while (!name.isEmpty() && (name.back() == QLatin1Char(' ') || name.back() == QLatin1Char('.')))
    name.chop(1);
  while (!name.isEmpty() && (name.front() == QLatin1Char(' ') || name.front() == QLatin1Char('.')))
    name.remove(0, 1);
  if (name.size() > kMaxComponent) name = name.left(kMaxComponent);
  while (!name.isEmpty() && (name.back() == QLatin1Char(' ') || name.back() == QLatin1Char('.')))
    name.chop(1);
  if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String("..") ||
      reservedDeviceName(name)) {
    return shortName();
  }
  return name;
}

QString suggestedExtractDirectory(const QString &archivePath) {
  const QFileInfo archive(archivePath);
  const QString parent = archive.absolutePath();
  QString full = QDir(parent).filePath(sanitizeFolderComponent(archive.fileName()));
  const QString archiveAbs = archive.absoluteFilePath();
  if (full.size() > kMaxPath ||
      QFileInfo(full).absoluteFilePath().compare(archiveAbs, Qt::CaseInsensitive) == 0) {
    full = QDir(parent).filePath(shortName());
  }
  return QDir::cleanPath(full);
}

QString normalizeExtractDirectory(const QString &requested, const QString &archivePath) {
  const QString path = QDir::cleanPath(QDir::fromNativeSeparators(requested.trimmed()));
  if (path.isEmpty() || !QDir::isAbsolutePath(path)) return suggestedExtractDirectory(archivePath);
  const QFileInfo info(path);
  const QString archiveAbs = QFileInfo(archivePath).absoluteFilePath();
  if (info.exists() && info.isDir() && info.absoluteFilePath().size() <= kMaxPath &&
      info.absoluteFilePath().compare(archiveAbs, Qt::CaseInsensitive) != 0) {
    return info.absoluteFilePath();
  }
  const QString parent = parentOf(path);
  QString full = QDir(parent).filePath(sanitizeFolderComponent(info.fileName()));
  if (full.size() > kMaxPath ||
      QFileInfo(full).absoluteFilePath().compare(archiveAbs, Qt::CaseInsensitive) == 0) {
    full = QDir(parent).filePath(shortName());
  }
  return QDir::cleanPath(full);
}

QString existingDirectoryForDialog(const QString &suggested) {
  const QFileInfo info(QDir::fromNativeSeparators(suggested));
  if (info.exists() && info.isDir()) return info.absoluteFilePath();
  const QString parent = info.isFile() ? info.absolutePath() : info.dir().absolutePath();
  if (QFileInfo(parent).isDir()) return parent;
  if (QFileInfo(QDir::homePath()).isDir()) return QDir::homePath();
  return QDir::currentPath();
}
