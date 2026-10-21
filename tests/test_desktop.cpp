#include "MainWindow.hpp"
#include "extract_path.hpp"
#include <QAbstractItemModel>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDialog>
#include <QCheckBox>
#include <QComboBox>
#include <QSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QTableView>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <QtTest>
#include <cstdio>

static QString sevenZip;
static QString engine;

class DesktopTests : public QObject {
  Q_OBJECT
 private:
  bool makeArchive(const QString &directory, const QString &archive,
                   const QByteArray &data, const QString &password = {}) {
    QFile file(directory + "/hello.txt");
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) return false;
    file.close();
    QProcess process;
    process.setWorkingDirectory(directory);
    QStringList args{"a", archive.endsWith(".zip") ? "-tzip" : "-t7z", archive, "hello.txt"};
    if (!password.isEmpty()) args << "-p" + password << "-mhe=on";
    process.start(sevenZip, args);
    return process.waitForFinished(15000) && process.exitCode() == 0;
  }
 private slots:
  void init() {
    std::fprintf(stderr, "[desktop] %s\n", QTest::currentTestFunction());
    std::fflush(stderr);
  }
  void cleanup() {
    std::fprintf(stderr, "[desktop] %s %s\n", QTest::currentTestFunction(),
                 QTest::currentTestFailed() ? "FAILED" : "ok");
    std::fflush(stderr);
  }
  void initTestCase() { qputenv("ARCHIVER_7ZIP", engine.toUtf8()); }
  void openAndExtract() {
    QTemporaryDir temp(QDir::currentPath() + "/desktop-test-XXXXXX");
    QVERIFY(temp.isValid());
    const QString path = temp.path() + QStringLiteral("/测试😀.zip");
    QVERIFY(makeArchive(temp.path(), path, "gui contents"));
    MainWindow window;
    auto *extract = window.findChild<QPushButton *>("extractButton");
    QVERIFY(!extract->isEnabled());
    window.openPath(path);
    QVERIFY(window.isBusy());
    QVERIFY(!window.findChild<QPushButton *>("openButton")->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 10000);
    QCOMPARE(window.findChild<QTableView *>("archiveTable")->model()->rowCount(), 1);
    QVERIFY(extract->isEnabled());
    const QString dest = temp.path() + QStringLiteral("/输出😀");
    QTimer acceptExtract;
    connect(&acceptExtract, &QTimer::timeout, [&] {
      if (auto *edit = QApplication::activeModalWidget()
                           ? QApplication::activeModalWidget()->findChild<QLineEdit *>("destinationEdit")
                           : nullptr) {
        edit->setText(dest);
        if (auto *ok = edit->window()->findChild<QPushButton *>("extractOk")) ok->click();
      }
    });
    acceptExtract.start(10);
    extract->click();
    QVERIFY(window.isBusy());
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 10000);
    QFile output(dest + "/hello.txt");
    QVERIFY(output.open(QIODevice::ReadOnly));
    QCOMPARE(output.readAll(), QByteArray("gui contents"));
    QVERIFY(window.findChild<QLabel *>("statusLabel")->text().contains(QStringLiteral("解压完成")));
  }
  void encryptedArchiveUsesOneSessionPassword() {
    QTemporaryDir temp(QDir::currentPath() + "/desktop-test-XXXXXX");
    QVERIFY(temp.isValid());
    const QString password = QUuid::createUuid().toString() + QStringLiteral("🔑");
    const QString path = temp.path() + "/encrypted.7z";
    QVERIFY(makeArchive(temp.path(), path, "secret", password));
    int prompts = 0;
    QTimer answer;
    connect(&answer, &QTimer::timeout, [&] {
      if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
        if (auto *edit = dialog->findChild<QLineEdit *>("passwordEdit")) {
          ++prompts;
          QCOMPARE(edit->echoMode(), QLineEdit::Normal);
          edit->setText(password);
          dialog->accept();
        }
      }
    });
    answer.start(10);
    MainWindow window;
    window.openPath(path);
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 10000);
    QCOMPARE(prompts, 1);
    const QString dest = temp.path() + "/out";
    QTimer acceptExtract;
    connect(&acceptExtract, &QTimer::timeout, [&] {
      if (auto *dialog = QApplication::activeModalWidget()) {
        if (auto *edit = dialog->findChild<QLineEdit *>("destinationEdit")) edit->setText(dest);
        if (auto *ok = dialog->findChild<QPushButton *>("extractOk")) ok->click();
      }
    });
    acceptExtract.start(10);
    window.findChild<QPushButton *>("extractButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 10000);
    QCOMPARE(prompts, 1);
    QFile output(dest + "/hello.txt");
    QVERIFY(output.open(QIODevice::ReadOnly));
    QCOMPARE(output.readAll(), QByteArray("secret"));
  }
  void cancelKeepsUiUsable() {
    QTemporaryDir temp(QDir::currentPath() + "/desktop-test-XXXXXX");
    QVERIFY(temp.isValid());
    const QString path = temp.path() + "/cancel.zip";
    QVERIFY(makeArchive(temp.path(), path, QByteArray(16 * 1024 * 1024, 'x')));
    MainWindow window;
    window.openPath(path);
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 10000);
    QTimer acceptExtract;
    connect(&acceptExtract, &QTimer::timeout, [&] {
      if (auto *dialog = QApplication::activeModalWidget()) {
        if (auto *edit = dialog->findChild<QLineEdit *>("destinationEdit"))
          edit->setText(temp.path() + "/out");
        if (auto *ok = dialog->findChild<QPushButton *>("extractOk")) ok->click();
      }
    });
    acceptExtract.start(10);
    window.findChild<QPushButton *>("extractButton")->click();
    window.findChild<QPushButton *>("cancelButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 10000);
    QVERIFY(window.findChild<QLabel *>("statusLabel")->text().contains(QStringLiteral("取消")));
    QVERIFY(!QFile::exists(temp.path() + "/out/hello.txt"));
    QVERIFY(window.findChild<QPushButton *>("extractButton")->isEnabled());
  }
  void cancelledPasswordClearsPreviousList() {
    QTemporaryDir temp(QDir::currentPath() + "/desktop-test-XXXXXX");
    QVERIFY(temp.isValid());
    const QString plain = temp.path() + "/plain.zip";
    const QString encrypted = temp.path() + "/encrypted.7z";
    QVERIFY(makeArchive(temp.path(), plain, "plain"));
    QVERIFY(makeArchive(temp.path(), encrypted, "secret", QUuid::createUuid().toString()));
    MainWindow window;
    window.openPath(plain);
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 10000);
    QCOMPARE(window.findChild<QTableView *>("archiveTable")->model()->rowCount(), 1);
    QTimer reject;
    connect(&reject, &QTimer::timeout, [&] {
      if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
        if (dialog->findChild<QLineEdit *>("passwordEdit")) dialog->reject();
    });
    reject.start(10);
    window.openPath(encrypted);
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 10000);
    QCOMPARE(window.findChild<QTableView *>("archiveTable")->model()->rowCount(), 0);
    QVERIFY(!window.findChild<QPushButton *>("extractButton")->isEnabled());
  }
  void nestedPasswordRetriesAtCurrentLayer() {
    QTemporaryDir temp(QDir::currentPath() + "/desktop-test-XXXXXX");
    QVERIFY(temp.isValid());
    const QString password = QUuid::createUuid().toString() + QStringLiteral("🔑");
    const QString inner = temp.path() + "/inner.7z";
    QVERIFY(makeArchive(temp.path(), inner, "inner secret", password));
    QFile note(temp.path() + "/note.txt");
    QVERIFY(note.open(QIODevice::WriteOnly)); note.write("outer note"); note.close();
    const QString outer = temp.path() + "/outer.zip";
    QProcess process;
    process.setWorkingDirectory(temp.path());
    process.start(sevenZip, {"a", "-tzip", outer, "inner.7z", "note.txt"});
    QVERIFY(process.waitForFinished(15000)); QCOMPARE(process.exitCode(), 0);
    MainWindow window;
    window.openPath(outer);
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 10000);
    int prompts = 0;
    QTimer answer;
    connect(&answer, &QTimer::timeout, [&] {
      auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      if (!dialog) return;
      if (auto *edit = dialog->findChild<QLineEdit *>("destinationEdit")) {
        edit->setText(temp.path() + "/out");
        dialog->findChild<QPushButton *>("extractOk")->click();
      } else if (auto *edit = dialog->findChild<QLineEdit *>("passwordEdit")) {
        edit->setText(++prompts == 1 ? "wrong" : password);
        dialog->accept();
      }
    });
    answer.start(10);
    window.findChild<QPushButton *>("extractButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 15000);
    QCOMPARE(prompts, 2);
    QFile output(temp.path() + "/out/hello.txt");
    QVERIFY(output.open(QIODevice::ReadOnly)); QCOMPARE(output.readAll(), QByteArray("inner secret"));
    QFile outerNote(temp.path() + "/out/note.txt");
    QVERIFY(outerNote.open(QIODevice::ReadOnly)); QCOMPARE(outerNote.readAll(), QByteArray("outer note"));
    QVERIFY(!QFile::exists(temp.path() + "/out/inner.7z"));
  }
  void nestedChoiceCanStopAfterOneLayer() {
    QTemporaryDir temp(QDir::currentPath() + "/desktop-test-XXXXXX");
    QVERIFY(temp.isValid());
    const QString inner = temp.path() + "/inner.zip";
    QVERIFY(makeArchive(temp.path(), inner, "inner"));
    const QString outer = temp.path() + "/outer.zip";
    QProcess process;
    process.setWorkingDirectory(temp.path());
    process.start(sevenZip, {"a", "-tzip", outer, "inner.zip"});
    QVERIFY(process.waitForFinished(15000)); QCOMPARE(process.exitCode(), 0);
    MainWindow window;
    window.openPath(outer);
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 10000);
    int choices = 0;
    QTimer answer;
    connect(&answer, &QTimer::timeout, [&] {
      auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      if (!dialog) return;
      if (auto *edit = dialog->findChild<QLineEdit *>("destinationEdit")) {
        edit->setText(temp.path() + "/out");
        dialog->findChild<QComboBox *>("nestedModeCombo")->setCurrentIndex(1);
        dialog->findChild<QPushButton *>("extractOk")->click();
      } else if (auto *stop = dialog->findChild<QPushButton *>("stopNestedButton")) {
        ++choices; stop->click();
      }
    });
    answer.start(10);
    window.findChild<QPushButton *>("extractButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 10000);
    QCOMPARE(choices, 1);
    QVERIFY(QFile::exists(temp.path() + "/out/inner.zip"));
    QVERIFY(!QFile::exists(temp.path() + "/out/hello.txt"));
    QVERIFY(window.findChild<QLabel *>("statusLabel")->text().contains(QStringLiteral("解压完成")));
  }
  void quickExtractUsesDefaultFolder() {
    QTemporaryDir temp(QDir::currentPath() + "/desktop-test-XXXXXX");
    QVERIFY(temp.isValid());
    const QString archive = temp.path() + "/quick.zip";
    QVERIFY(makeArchive(temp.path(), archive, "quick contents"));
    MainWindow window;
    window.quickExtract(archive);
    QVERIFY(window.isBusy());
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 15000);
    QFile output(temp.path() + "/quick/hello.txt");
    QVERIFY(output.open(QIODevice::ReadOnly));
    QCOMPARE(output.readAll(), QByteArray("quick contents"));
    auto *report = window.findChild<QAction *>("extractionReportAction");
    QVERIFY(report && report->isEnabled());
  }
  void doubleClickRenamedArchiveWithoutChangingSuffix() {
    QTemporaryDir temp(QDir::currentPath() + "/desktop-test-XXXXXX");
    QVERIFY(temp.isValid());
    const QString original = temp.path() + "/original.zip";
    const QString renamed = temp.path() + "/archive.txt";
    QVERIFY(makeArchive(temp.path(), original, "renamed payload"));
    QVERIFY(QFile::rename(original, renamed));
    MainWindow window;
    auto *pathBar = window.findChild<QLineEdit *>("pathBar");
    pathBar->setText(temp.path());
    QVERIFY(QMetaObject::invokeMethod(pathBar, "returnPressed", Qt::DirectConnection));
    auto *table = window.findChild<QTableView *>("archiveTable");
    QModelIndex target;
    for (int row = 0; row < table->model()->rowCount(); ++row) {
      const auto index = table->model()->index(row, 0);
      if (index.data().toString() == "archive.txt") target = index;
    }
    QVERIFY(target.isValid());
    QString unexpectedError;
    QTimer closeUnexpected;
    connect(&closeUnexpected, &QTimer::timeout, [&] {
      if (auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
        unexpectedError = dialog->text() + "\n" + dialog->detailedText();
        dialog->accept();
      }
    });
    closeUnexpected.start(10);
    QVERIFY(QMetaObject::invokeMethod(table, "doubleClicked", Qt::DirectConnection, Q_ARG(QModelIndex, target)));
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 15000);
    QVERIFY2(unexpectedError.isEmpty(), qPrintable(unexpectedError));
    QFile output(suggestedExtractDirectory(renamed) + "/hello.txt");
    QVERIFY(output.open(QIODevice::ReadOnly));
    QCOMPARE(output.readAll(), QByteArray("renamed payload"));
    QVERIFY(QFile::exists(renamed));
  }
  void partialExtractionContinuesAndExposesReport() {
    QTemporaryDir temp(QDir::currentPath() + "/desktop-test-XXXXXX");
    QVERIFY(temp.isValid());
    QVERIFY(makeArchive(temp.path(), temp.path() + "/good.zip", "independent branch"));
    QFile bad(temp.path() + "/bad.zip");
    QVERIFY(bad.open(QIODevice::WriteOnly)); bad.write("not ZIP"); bad.close();
    QProcess process;
    process.setWorkingDirectory(temp.path());
    process.start(sevenZip, {"a", "-tzip", "outer.zip", "good.zip", "bad.zip"});
    QVERIFY(process.waitForFinished(15000)); QCOMPARE(process.exitCode(), 0);
    MainWindow window;
    QString report;
    QTimer closeWarning;
    connect(&closeWarning, &QTimer::timeout, [&] {
      if (auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
        report = dialog->detailedText(); dialog->accept();
      }
    });
    closeWarning.start(10);
    window.quickExtract(temp.path() + "/outer.zip");
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 15000);
    QFile output(temp.path() + "/outer/good.zip.unpacked/hello.txt");
    QVERIFY(output.open(QIODevice::ReadOnly));
    QCOMPARE(output.readAll(), QByteArray("independent branch"));
    QVERIFY(QFile::exists(temp.path() + "/outer/bad.zip"));
    QVERIFY(window.findChild<QLabel *>("statusLabel")->text().contains(QStringLiteral("部分解压完成")));
    QVERIFY(report.contains("bad.zip")); QVERIFY(report.contains("good.zip"));
    QVERIFY(window.findChild<QAction *>("extractionReportAction")->isEnabled());
  }
  void uiPreview() {
    const QString outputDir = qEnvironmentVariable("ARCHIVER_UI_PREVIEW_DIR");
    if (outputDir.isEmpty()) QSKIP("Set ARCHIVER_UI_PREVIEW_DIR to export actual Qt window previews");
    QVERIFY(QDir().mkpath(outputDir));
    QTemporaryDir temp(QDir::currentPath() + "/desktop-test-XXXXXX");
    const QString archive = temp.path() + QStringLiteral("/项目资料.zip");
    QStringList names{QStringLiteral("文档/使用说明.txt"), QStringLiteral("图片/封面.png"),
        QStringLiteral("视频/演示.mp4"), QStringLiteral("素材包.7z"), QStringLiteral("README.txt"), QStringLiteral("文件清单.csv")};
    for (const auto &name : names) {
      QVERIFY(QDir().mkpath(QFileInfo(temp.path() + "/" + name).absolutePath()));
      QFile file(temp.path() + "/" + name);
      QVERIFY(file.open(QIODevice::WriteOnly)); file.write(QByteArray(2048, 'x'));
    }
    QProcess process;
    process.setWorkingDirectory(temp.path());
    QStringList args{"a", "-tzip", archive}; args.append(names);
    process.start(sevenZip, args);
    QVERIFY(process.waitForFinished(15000)); QCOMPARE(process.exitCode(), 0);
    MainWindow window;
    window.show(); window.openPath(archive);
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 10000);
    QTest::qWait(60);
    QVERIFY(window.grab().save(outputDir + "/main-window.png"));
    bool saved = false;
    QTimer capture;
    connect(&capture, &QTimer::timeout, [&] {
      if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
        if (dialog->findChild<QLineEdit *>("destinationEdit")) {
          saved = dialog->grab().save(outputDir + "/extraction-options.png");
          dialog->reject();
        }
      }
    });
    capture.start(50);
    window.findChild<QPushButton *>("extractButton")->click();
    QVERIFY(saved);
  }
  void disguisedMp4WithLongTitleExtractsToLegalFolder() {
    const QString leaf = QStringLiteral("【3D】元梦之星人物手办雕刻 zbrush二次元游戏角色模型制作教程！ 【3D】元梦之星人物手办雕刻 zbrush二次元游戏角色模型制作教程！_1.mp4");
    QCOMPARE(sanitizeFolderComponent(leaf), QStringLiteral("【3D】元梦之星人物手办雕刻 zbrush二次元游戏角色模型制作教程！ 【3D】元梦之星人物手办雕刻 zbrush二次元游戏角色模型制作教程！_1"));
    QCOMPARE(sanitizeFolderComponent(QStringLiteral("数据.zip.001")), QStringLiteral("数据"));
    QCOMPARE(sanitizeFolderComponent(QStringLiteral("数据.part02.rar")), QStringLiteral("数据"));
    QCOMPARE(sanitizeFolderComponent(QStringLiteral("数据.iso")), QStringLiteral("数据"));
    QCOMPARE(sanitizeFolderComponent(QStringLiteral("数据.txt.gz")), QStringLiteral("数据.txt"));
    QCOMPARE(sanitizeFolderComponent(QStringLiteral("bad:name*.mp4")), QStringLiteral("bad_name_"));
    QCOMPARE(sanitizeFolderComponent(QStringLiteral("keep. .mp4")), QStringLiteral("keep"));
    QCOMPARE(sanitizeFolderComponent(QStringLiteral("CON.mp4")), QStringLiteral("解压文件"));

    QTemporaryDir temp(QDir::currentPath() + "/desktop-test-XXXXXX");
    QVERIFY(temp.isValid());
    const QString zip = temp.path() + "/plain.zip";
    QVERIFY(makeArchive(temp.path(), zip, "from-mp4-name"));
    const QString mp4 = temp.path() + "/" + leaf;
    QVERIFY(QFile::rename(zip, mp4));
    QCOMPARE(existingDirectoryForDialog(mp4), QFileInfo(mp4).absolutePath());
    QVERIFY(QFileInfo(existingDirectoryForDialog(mp4)).isDir());
    const QString suggested = suggestedExtractDirectory(mp4);
    const QString folderName = QFileInfo(suggested).fileName();
    QVERIFY(folderName.contains(QStringLiteral("元梦之星")));
    QVERIFY(!folderName.endsWith(QStringLiteral(".mp4"), Qt::CaseInsensitive));
    QVERIFY(!folderName.endsWith(QLatin1Char(' ')));
    QVERIFY(!folderName.endsWith(QLatin1Char('.')));
    for (const QChar ch : QStringLiteral("\\/:*?\"<>|")) QVERIFY(!folderName.contains(ch));
    QVERIFY(suggested.size() <= 240);
    QVERIFY(QFileInfo(suggested).absoluteFilePath().compare(QFileInfo(mp4).absoluteFilePath(),
                                                            Qt::CaseInsensitive) != 0);

    MainWindow window;
    window.openPath(mp4);
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 10000);
    QCOMPARE(window.findChild<QTableView *>("archiveTable")->model()->rowCount(), 1);
    QString used;
    QTimer acceptExtract;
    connect(&acceptExtract, &QTimer::timeout, [&] {
      if (auto *dialog = QApplication::activeModalWidget()) {
        if (auto *edit = dialog->findChild<QLineEdit *>("destinationEdit")) used = edit->text();
        if (auto *ok = dialog->findChild<QPushButton *>("extractOk")) ok->click();
      }
    });
    acceptExtract.start(10);
    window.findChild<QPushButton *>("extractButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 10000);
    QVERIFY(window.findChild<QLabel *>("statusLabel")->text().contains(QStringLiteral("解压完成")));
    const QString dest = QDir::fromNativeSeparators(used);
    QCOMPARE(QFileInfo(dest).fileName(), folderName);
    QFile output(dest + "/hello.txt");
    QVERIFY(output.open(QIODevice::ReadOnly));
    QCOMPARE(output.readAll(), QByteArray("from-mp4-name"));
    std::fprintf(stderr, "[desktop] disguisedMp4 done\n");
    std::fflush(stderr);
  }
};

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  const auto args = app.arguments();
  if (args.size() != 3) return 2;
  sevenZip = args[1];
  engine = args[2];
  DesktopTests tests;
  const int result = QTest::qExec(&tests, 1, argv);
  std::fprintf(stderr, "[desktop] qExec returned %d\n", result);
  std::fflush(stdout);
  std::fflush(stderr);
  return result;
}
#include "test_desktop.moc"
