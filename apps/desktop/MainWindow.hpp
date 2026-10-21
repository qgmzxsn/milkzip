#ifndef LOCAL_ARCHIVER_MAIN_WINDOW_HPP
#define LOCAL_ARCHIVER_MAIN_WINDOW_HPP
#include <QMainWindow>
#include <QString>
#include <QVector>
#include <memory>
class ArchiveModel;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTableView;
class QTimer;
class QAction;
struct ArchiveSession;
struct DesktopSecret;
struct DesktopWorkState;
class MainWindow : public QMainWindow {
  Q_OBJECT
 public:
  explicit MainWindow(QWidget *parent = nullptr);
  ~MainWindow() override;
  void openPath(QString path);
  void quickExtract(const QString &path);
  bool isBusy() const { return busy_; }
 protected:
  void dragEnterEvent(QDragEnterEvent *event) override;
  void dropEvent(QDropEvent *event) override;
  void closeEvent(QCloseEvent *event) override;
 private:
  void browseDirectory(const QString &path);
  void enterArchiveFolder(const QString &prefix);
  void goUp();
  void activateRow(const QModelIndex &index);
  void chooseArchive();
  void addFiles();
  void testArchive();
  void showInfo();
  void showPasswordBook();
  void startOpen(const QString &path, std::shared_ptr<DesktopSecret> secret = {}, bool save = false);
  void startExtract(const QString &sourcePath, const QString &dest, bool nested,
                    std::shared_ptr<DesktopSecret> secret = {}, bool quick = false);
  void setBusy(bool busy, bool extracting = false);
  void refreshProgress();
  void cancelExtraction();
  std::shared_ptr<DesktopSecret> askPassword(bool rejected, bool *save);
  void showError(int result, const QString &detail);
  void fillArchiveRows(const QVector<struct ArchiveRow> &rows);
  ArchiveModel *model_ = nullptr;
  QTableView *table_ = nullptr;
  QLabel *status_ = nullptr;
  QLineEdit *pathBar_ = nullptr;
  QProgressBar *progress_ = nullptr;
  QPushButton *openButton_ = nullptr;
  QPushButton *addButton_ = nullptr;
  QPushButton *extractButton_ = nullptr;
  QPushButton *testButton_ = nullptr;
  QPushButton *infoButton_ = nullptr;
  QPushButton *upButton_ = nullptr;
  QPushButton *cancelButton_ = nullptr;
  QTimer *timer_ = nullptr;
  QAction *reportAction_ = nullptr;
  QString lastExtractReport_;
  std::shared_ptr<ArchiveSession> archive_;
  std::shared_ptr<DesktopWorkState> work_;
  QString archivePath_;
  QString directory_;
  QString archivePrefix_;
  QVector<struct ArchiveRow> archiveRows_;
  int nestedDepth_ = 32;
  int nestedByteLimitGiB_ = 64;
  bool askLayers_ = false;
  bool keepIntermediate_ = false;
  bool busy_ = false;
  bool extracting_ = false;
  bool closeAfterFinish_ = false;
  bool insideArchive_ = false;
};
#endif
