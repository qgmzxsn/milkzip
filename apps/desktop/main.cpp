#include "MainWindow.hpp"

#include <QApplication>
#include <QDir>
#include <QIcon>
#include <QStyleFactory>
#include <QTimer>
#include <QTranslator>
#include "archiver.h"

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  if (QStyle *vista = QStyleFactory::create(QStringLiteral("windowsvista")))
    QApplication::setStyle(vista);
  else if (QStyle *windows = QStyleFactory::create(QStringLiteral("Windows")))
    QApplication::setStyle(windows);
  QApplication::setApplicationName(QStringLiteral("milkzip"));
  QApplication::setApplicationVersion(QStringLiteral(ARCHIVER_VERSION));
  QApplication::setWindowIcon(QIcon(QStringLiteral(":/milkzip.ico")));
  QTranslator translator;
  const QString translationDir = QDir(QApplication::applicationDirPath()).filePath("translations");
  if (translator.load(QStringLiteral("qt_zh_CN"), translationDir) ||
      translator.load(QStringLiteral("qtbase_zh_CN"), translationDir))
    app.installTranslator(&translator);
  MainWindow window;
  window.show();
  const QStringList arguments = app.arguments();
  if (arguments.size() > 1)
    QTimer::singleShot(0, &window, [&window, arguments] { window.openPath(arguments[1]); });
  return app.exec();
}
