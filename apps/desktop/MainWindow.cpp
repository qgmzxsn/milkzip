#include "MainWindow.hpp"
#include "extract_path.hpp"
#include "password_book.hpp"
#include "archiver.h"

#include <QAbstractTableModel>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCollator>
#include <QComboBox>
#include <QFile>
#include <QFormLayout>
#include <QPainter>
#include <QSpinBox>
#include <QStyle>
#include <QStyleOptionButton>
#include <QCloseEvent>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHash>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QStatusBar>
#include <QTableView>
#include <QTimer>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <atomic>
#include <exception>
#include <functional>
#include <cstring>

struct DesktopSecret {
  QByteArray bytes;
  ~DesktopSecret() {
    volatile char *data = bytes.data();
    for (qsizetype i = 0; i < bytes.size(); ++i) data[i] = 0;
  }
};
struct ArchiveSession {
  ArchiverArchive *handle = nullptr;
  std::shared_ptr<DesktopSecret> secret;
  ~ArchiveSession() { archiver_close(handle); }
};
struct DesktopWorkState {
  std::atomic<bool> cancel{false};
  std::atomic<uint64_t> completed{0};
  std::atomic<uint64_t> total{0};
};
struct ArchiveRow {
  QString name;
  QString path;
  uint64_t size = 0;
  uint64_t packed = 0;
  qint64 modified = -1;
  bool directory = false;
  bool symlink = false;
  bool parent = false;
  bool archive = false;
  bool hasPacked = false;
};

class ArchiveModel final : public QAbstractTableModel {
 public:
  explicit ArchiveModel(QObject *parent) : QAbstractTableModel(parent) {
    // data() runs for every visible cell on every repaint. Keep the
    // style lookups and locale out of that path.
    fileIcon_ = QApplication::style()->standardIcon(QStyle::SP_FileIcon);
    dirIcon_ = QApplication::style()->standardIcon(QStyle::SP_DirIcon);
    upIcon_ = QApplication::style()->standardIcon(QStyle::SP_ArrowUp);
  }
  int rowCount(const QModelIndex &parent = {}) const override {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
  }
  int columnCount(const QModelIndex &parent = {}) const override {
    return parent.isValid() ? 0 : 4;
  }
  QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
      if (section == 0) return QStringLiteral("名称");
      if (section == 1) return QStringLiteral("大小");
      if (section == 2) return QStringLiteral("压缩后大小");
      if (section == 3) return QStringLiteral("修改时间");
    }
    return QAbstractTableModel::headerData(section, orientation, role);
  }
  QVariant data(const QModelIndex &index, int role) const override {
    if (!index.isValid() || index.row() >= rows_.size()) return {};
    const auto &row = rows_[index.row()];
    if (role == Qt::TextAlignmentRole && (index.column() == 1 || index.column() == 2))
      return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
    if (role == Qt::ToolTipRole) return row.path;
    if (role == Qt::DecorationRole && index.column() == 0)
      return row.parent ? upIcon_ : row.directory ? dirIcon_ : fileIcon_;
    if (role != Qt::DisplayRole) return {};
    if (index.column() == 0) return row.name;
    if (index.column() == 1) {
      if (row.parent || row.directory) return {};
      return locale_.formattedDataSize(row.size);
    }
    if (index.column() == 2) {
      if (!row.hasPacked) return {};
      return locale_.formattedDataSize(row.packed);
    }
    if (row.modified < 0) return {};
    const auto cached = modifiedText_.constFind(row.modified);
    if (cached != modifiedText_.constEnd()) return cached.value();
    const QString text =
        locale_.toString(QDateTime::fromSecsSinceEpoch(row.modified), QLocale::ShortFormat);
    modifiedText_.insert(row.modified, text);
    return text;
  }
  const ArchiveRow *rowAt(int row) const {
    if (row < 0 || row >= rows_.size()) return nullptr;
    return &rows_[row];
  }
  void setRows(QVector<ArchiveRow> rows) {
    beginResetModel();
    modifiedText_.clear();
    rows_ = std::move(rows);
    sortRows();
    endResetModel();
  }
  void sort(int column, Qt::SortOrder order) override {
    beginResetModel();
    sortColumn_ = column;
    sortOrder_ = order;
    sortRows();
    endResetModel();
  }
  uint64_t totalSize() const {
    uint64_t total = 0;
    for (const auto &row : rows_)
      if (!row.parent && !row.directory) total += row.size;
    return total;
  }
 private:
  QIcon fileIcon_;
  QIcon dirIcon_;
  QIcon upIcon_;
  QLocale locale_;
  mutable QHash<qint64, QString> modifiedText_;
  int sortColumn_ = 0;
  Qt::SortOrder sortOrder_ = Qt::AscendingOrder;
  void sortRows() {
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::stable_sort(rows_.begin(), rows_.end(), [&](const ArchiveRow &a, const ArchiveRow &b) {
      if (a.parent != b.parent) return a.parent;
      if (a.directory != b.directory) return a.directory;
      int compare = 0;
      if (sortColumn_ == 1) compare = (a.size > b.size) - (a.size < b.size);
      else if (sortColumn_ == 2) compare = (a.packed > b.packed) - (a.packed < b.packed);
      else if (sortColumn_ == 3) compare = (a.modified > b.modified) - (a.modified < b.modified);
      else compare = collator.compare(a.name, b.name);
      return sortOrder_ == Qt::AscendingOrder ? compare < 0 : compare > 0;
    });
  }
  QVector<ArchiveRow> rows_;
};

namespace {
void chineseButtons(QDialogButtonBox *buttons) {
  for (const auto &pair : {std::make_pair(QDialogButtonBox::Ok, "确定"),
      std::make_pair(QDialogButtonBox::Cancel, "取消"), std::make_pair(QDialogButtonBox::Save, "保存"),
      std::make_pair(QDialogButtonBox::Close, "关闭")})
    if (auto *button = buttons->button(pair.first)) button->setText(QString::fromUtf8(pair.second));
}

QIcon commandIcon(const QString &name) {
  QPixmap pixmap(64, 64);
  pixmap.fill(Qt::transparent);
  QPainter p(&pixmap);
  p.setRenderHint(QPainter::Antialiasing);
  p.scale(2, 2);
  p.setPen(QPen(QColor("#a16a21"), 1.5));
  p.setBrush(QColor("#f3cb78"));
  if (name == QLatin1String("passwordButton")) {
    p.setPen(QPen(QColor("#ac7c1e"), 3, Qt::SolidLine, Qt::RoundCap));
    p.setBrush(QColor("#ffe39b"));
    p.drawEllipse(QRectF(4, 3, 12, 12));
    p.drawLine(QPointF(14, 14), QPointF(27, 27));
    p.drawLine(QPointF(22, 22), QPointF(26, 18));
    p.drawLine(QPointF(25, 25), QPointF(29, 21));
  } else if (name == QLatin1String("infoButton")) {
    p.setPen(Qt::NoPen); p.setBrush(QColor("#4584c1")); p.drawEllipse(QRectF(3, 3, 26, 26));
    p.setPen(Qt::white); QFont font = p.font(); font.setBold(true); font.setPixelSize(23); p.setFont(font);
    p.drawText(QRectF(3, 2, 26, 28), Qt::AlignCenter, QStringLiteral("i"));
  } else if (name == QLatin1String("openButton")) {
    p.drawRoundedRect(QRectF(3, 7, 24, 21), 2, 2);
    p.setBrush(QColor("#ffdf90")); p.drawPolygon(QPolygonF{QPointF(3, 14), QPointF(30, 14), QPointF(25, 28), QPointF(3, 28)});
  } else {
    p.drawRoundedRect(QRectF(4, 6, 24, 22), 2, 2);
    p.setBrush(QColor("#ffe4a5")); p.drawRect(QRectF(4, 6, 24, 6));
    p.setPen(QPen(QColor("#287e48"), 3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (name == QLatin1String("extractButton")) {
      p.drawLine(QPointF(16, 3), QPointF(16, 21));
      p.drawLine(QPointF(10, 15), QPointF(16, 21)); p.drawLine(QPointF(16, 21), QPointF(22, 15));
    } else if (name == QLatin1String("testButton")) {
      p.drawLine(QPointF(9, 19), QPointF(14, 24)); p.drawLine(QPointF(14, 24), QPointF(24, 14));
    } else if (name == QLatin1String("addButton")) {
      p.drawLine(QPointF(16, 14), QPointF(16, 24)); p.drawLine(QPointF(11, 19), QPointF(21, 19));
    }
  }
  return QIcon(pixmap);
}
class CommandButton final : public QPushButton {
 public:
  CommandButton(const QString &text, QWidget *parent) : QPushButton(text, parent) {
    setFixedSize(74, 66);
    setFlat(true);
  }
 protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    if (isEnabled() && (underMouse() || isDown() || hasFocus())) {
      p.setPen(QColor("#b5cce4")); p.setBrush(isDown() ? QColor("#dce9f6") : QColor("#eaf2fa"));
      p.drawRoundedRect(rect().adjusted(2, 2, -2, -2), 3, 3);
    }
    icon().paint(&p, QRect((width() - 30) / 2, 5, 30, 30), Qt::AlignCenter,
                 isEnabled() ? QIcon::Normal : QIcon::Disabled);
    p.setPen(palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled, QPalette::ButtonText));
    p.drawText(QRect(0, 39, width(), 22), Qt::AlignCenter, text());
  }
};
bool savePassword(const QByteArray &secret) {
  QVector<QByteArray> book = PasswordBook::load();
  bool exists = book.contains(secret);
  if (!exists) book.append(secret);
  const bool ok = exists || PasswordBook::store(book);
  for (auto &item : book) {
    volatile char *data = item.data();
    for (qsizetype i = 0; i < item.size(); ++i) data[i] = 0;
  }
  return ok;
}

int reportProgress(uint64_t completed, uint64_t total, void *user) {
  auto *state = static_cast<DesktopWorkState *>(user);
  state->completed.store(completed, std::memory_order_relaxed);
  state->total.store(total, std::memory_order_relaxed);
  return state->cancel.load(std::memory_order_relaxed) ? 1 : 0;
}

bool looksLikeArchive(const QString &name) {
  const QString lower = name.toLower();
  static const QStringList suffixes = {
      QStringLiteral(".zip"), QStringLiteral(".7z"), QStringLiteral(".tar"),
      QStringLiteral(".tgz"), QStringLiteral(".tar.gz"), QStringLiteral(".tbz"),
      QStringLiteral(".tbz2"), QStringLiteral(".tar.bz2"), QStringLiteral(".txz"),
      QStringLiteral(".tar.xz"), QStringLiteral(".tzst"), QStringLiteral(".tar.zst"),
      QStringLiteral(".rar"), QStringLiteral(".mp4"), QStringLiteral(".m4v"),
      QStringLiteral(".mov"), QStringLiteral(".jpg"), QStringLiteral(".jpeg"),
      QStringLiteral(".png"), QStringLiteral(".gif"), QStringLiteral(".bmp"),
      QStringLiteral(".webp"), QStringLiteral(".avi"), QStringLiteral(".mkv"),
      QStringLiteral(".webm"), QStringLiteral(".exe"), QStringLiteral(".sfx"),
      QStringLiteral(".zipsc"), QStringLiteral(".gz"), QStringLiteral(".bz2"),
      QStringLiteral(".xz"), QStringLiteral(".zst"), QStringLiteral(".zstd"),
      QStringLiteral(".lz4"), QStringLiteral(".lzma"), QStringLiteral(".z"),
      QStringLiteral(".cab"), QStringLiteral(".iso"), QStringLiteral(".udf"),
      QStringLiteral(".wim"), QStringLiteral(".swm"), QStringLiteral(".arj"),
      QStringLiteral(".lzh"), QStringLiteral(".lha"), QStringLiteral(".cpio"),
      QStringLiteral(".ar"), QStringLiteral(".deb"), QStringLiteral(".rpm"),
      QStringLiteral(".dmg"), QStringLiteral(".xar")};
  for (const auto &suffix : suffixes)
    if (lower.endsWith(suffix)) return true;
  const int dot = lower.lastIndexOf(QLatin1Char('.'));
  if (dot < 0) return false;
  const QString ext = lower.mid(dot + 1);
  if (ext.size() >= 2 && ext.size() <= 4) {
    bool digits = true;
    for (const QChar ch : ext) digits = digits && ch.isDigit();
    if (digits) return true;
  }
  if (ext.size() == 3 && (ext[0] == QLatin1Char('z') || ext[0] == QLatin1Char('r')) &&
      ext[1].isDigit() && ext[2].isDigit())
    return true;
  return lower.contains(QStringLiteral(".part")) && lower.endsWith(QStringLiteral(".rar"));
}

QString errorText(int code, const QString &detail) {
  switch (code) {
    case ARCHIVER_ERR_PATH:
      return QStringLiteral("已停止：压缩包或输出路径包含不安全的路径或链接。");
    case ARCHIVER_ERR_MISSING_VOLUME:
      return QStringLiteral("缺少分卷。%1").arg(detail);
    case ARCHIVER_ERR_NEED_CHOICE:
      return QStringLiteral("这一层有多个压缩包，需要先选择一个。\n%1").arg(detail);
    case ARCHIVER_ERR_LIMIT:
      return QStringLiteral("已达到嵌套解压上限。%1").arg(detail);
    case ARCHIVER_ERR_UNSUPPORTED:
      if (detail.contains(QStringLiteral("more data")))
        return QStringLiteral("压缩包展开后的大小超过本版本的 64 GiB 上限。");
      if (detail.contains(QStringLiteral("too many entries")))
        return QStringLiteral("压缩包包含超过 100 万个文件条目，暂时无法打开。");
      if (detail.contains(QStringLiteral("no archive")))
        return QStringLiteral("未在这个文件中识别到支持的压缩包。");
      return detail.contains(QStringLiteral("symlink"))
                 ? QStringLiteral("这个压缩包包含当前 Windows 版不支持创建的符号链接。")
                 : QStringLiteral("打不开这个文件。");
    case ARCHIVER_ERR_ENGINE:
      return QStringLiteral("无法加载解压引擎。请完整解压程序包，保留程序旁的所有文件后重试。");
    case ARCHIVER_ERR_IO:
      return detail.contains(QStringLiteral("already exists"))
                 ? QStringLiteral("输出位置已有同名文件，原文件已保留。请选择一个空文件夹后重试。")
                 : QStringLiteral("无法读取或写入文件。请检查文件是否可读、输出位置是否可写，以及磁盘空间。");
    case ARCHIVER_ERR_FORMAT:
      return QStringLiteral("无法完整读取这个压缩包。文件可能损坏、不完整，或与扩展名不符。");
    case ARCHIVER_ERR_PARTIAL:
      return QStringLiteral("部分解压完成。其他可解开的包已继续处理。点击“显示详情”查看未解开的分支。");
    case ARCHIVER_ERR_PASSWORD:
      return QStringLiteral("密码不对。");
    default:
      return QStringLiteral("操作失败，请重新选择文件后再试。");
  }
}

struct OpenResult {
  ArchiverResult code = ARCHIVER_OK;
  QString detail;
  std::shared_ptr<ArchiveSession> session;
  QVector<ArchiveRow> rows;
  uint64_t total = 0;
  int matched = -1;
};
struct ExtractResult {
  bool passwordSaveFailed = false;
  ArchiverResult code = ARCHIVER_OK;
  QString detail;
};

void wipeList(QVector<QByteArray> &items) {
  for (QByteArray &item : items) {
    volatile char *data = item.data();
    for (qsizetype i = 0; i < item.size(); ++i) data[i] = 0;
    item.clear();
  }
  items.clear();
}
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
  setWindowTitle(QStringLiteral("MilkZip"));
  resize(1040, 650);
  menuBar()->setNativeMenuBar(false);
  setStyleSheet(QStringLiteral(
      "QMainWindow { background: #f5f6f8; }"
      "QToolBar { background: #f5f6f8; border: 0; border-bottom: 1px solid #cbd1d9; spacing: 3px; padding: 3px 8px; }"
      "QMenuBar { background: #fafafa; border-bottom: 1px solid #dedfe2; }"
      "QTableView { background: white; alternate-background-color: #f6f8fb; color: #202934; border: 0; selection-background-color: #dceafa; selection-color: #102f54; }"
      "QHeaderView::section { background: #f6f7f9; color: #4b5665; border: 0; border-bottom: 1px solid #d6dbe2; border-right: 1px solid #e2e5e9; padding: 7px 10px; }"
      "QStatusBar { background: #f5f6f8; border-top: 1px solid #d6dbe2; }"
      "QLineEdit#pathBar { background: white; color: #253247; border: 1px solid #cbd1d9; border-radius: 3px; padding: 5px 9px; }"
      "QPushButton#upButton { padding: 4px 8px; }"));
  setMinimumSize(640, 400);
  setAcceptDrops(true);

  auto *fileMenu = menuBar()->addMenu(QStringLiteral("文件"));
  auto *openAction = fileMenu->addAction(QStringLiteral("打开…"));
  openAction->setShortcut(QKeySequence::Open);
  connect(openAction, &QAction::triggered, this, &MainWindow::chooseArchive);
  connect(fileMenu->addAction(QStringLiteral("添加…")), &QAction::triggered, this, &MainWindow::addFiles);
  connect(fileMenu->addAction(QStringLiteral("解压…")), &QAction::triggered, this, [this] {
    extractButton_->click();
  });
  connect(fileMenu->addAction(QStringLiteral("测试")), &QAction::triggered, this, &MainWindow::testArchive);
  fileMenu->addSeparator();
  connect(fileMenu->addAction(QStringLiteral("退出")), &QAction::triggered, this, &QWidget::close);

  auto *editMenu = menuBar()->addMenu(QStringLiteral("编辑"));
  connect(editMenu->addAction(QStringLiteral("全选")), &QAction::triggered, this, [this] {
    table_->selectAll();
  });
  auto *selectAll = editMenu->actions().last();
  selectAll->setShortcut(QKeySequence::SelectAll);
  auto *viewMenu = menuBar()->addMenu(QStringLiteral("查看"));
  connect(viewMenu->addAction(QStringLiteral("向上")), &QAction::triggered, this, &MainWindow::goUp);
  auto *toolsMenu = menuBar()->addMenu(QStringLiteral("工具"));
  connect(toolsMenu->addAction(QStringLiteral("密码本…")), &QAction::triggered, this,
          &MainWindow::showPasswordBook);
  reportAction_ = toolsMenu->addAction(QStringLiteral("上次解压结果…"));
  reportAction_->setObjectName(QStringLiteral("extractionReportAction"));
  reportAction_->setEnabled(false);
  connect(reportAction_, &QAction::triggered, this, [this] {
    QMessageBox dialog(QMessageBox::Information, QStringLiteral("上次解压结果"),
        QStringLiteral("点击“显示详情”查看已解开的包和未完成分支。"), QMessageBox::Ok, this);
    dialog.setDetailedText(lastExtractReport_);
    dialog.exec();
  });
  auto *helpMenu = menuBar()->addMenu(QStringLiteral("帮助"));
  connect(helpMenu->addAction(QStringLiteral("关于")), &QAction::triggered, this, [this] {
    QMessageBox::about(
        this, QStringLiteral("关于 MilkZip"),
        QStringLiteral("MilkZip %1\n\n查看和解压本机压缩包。密码本只写在 Windows 凭据管理器里。\n"
                       "不会创建 RAR，也不会运行解压出来的程序。")
            .arg(QLatin1String(ARCHIVER_VERSION)));
  });

  auto *toolbar = addToolBar(QStringLiteral("标准"));
  toolbar->setMovable(false);
  toolbar->setFloatable(false);
  auto addFlat = [&](const QString &text, const char *name) {
    auto *button = new CommandButton(text, toolbar);
    button->setIcon(commandIcon(QLatin1String(name)));
    button->setToolTip(text);
    button->setObjectName(name);
    button->setFlat(true);
    toolbar->addWidget(button);
    return button;
  };
  addButton_ = addFlat(QStringLiteral("添加"), "addButton");
  extractButton_ = addFlat(QStringLiteral("解压"), "extractButton");
  testButton_ = addFlat(QStringLiteral("测试"), "testButton");
  toolbar->addSeparator();
  openButton_ = addFlat(QStringLiteral("打开"), "openButton");
  infoButton_ = addFlat(QStringLiteral("信息"), "infoButton");
  auto *passwordButton = addFlat(QStringLiteral("密码本"), "passwordButton");
  connect(passwordButton, &QPushButton::clicked, this, &MainWindow::showPasswordBook);

  auto *central = new QWidget(this);
  auto *layout = new QVBoxLayout(central);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  pathBar_ = new QLineEdit(central);
  pathBar_->setObjectName(QStringLiteral("pathBar"));
  pathBar_->setClearButtonEnabled(false);
  auto *navigation = new QHBoxLayout();
  navigation->setContentsMargins(8, 7, 8, 7);
  navigation->setSpacing(7);
  upButton_ = new QPushButton(central);
  upButton_->setIcon(style()->standardIcon(QStyle::SP_ArrowUp));
  upButton_->setObjectName(QStringLiteral("upButton"));
  upButton_->setToolTip(QStringLiteral("上一级（Alt+↑）"));
  upButton_->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Up));
  navigation->addWidget(upButton_);
  navigation->addWidget(pathBar_, 1);
  layout->addLayout(navigation);
  model_ = new ArchiveModel(this);
  table_ = new QTableView(central);
  table_->setObjectName(QStringLiteral("archiveTable"));
  table_->setModel(model_);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  table_->setShowGrid(false);
  table_->setWordWrap(false);
  table_->setTextElideMode(Qt::ElideRight);
  table_->setAlternatingRowColors(true);
  table_->setSortingEnabled(true);
  table_->sortByColumn(0, Qt::AscendingOrder);
  table_->verticalHeader()->setDefaultSectionSize(29);
  table_->setIconSize(QSize(18, 18));
  table_->verticalHeader()->hide();
  table_->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  table_->horizontalHeader()->setStretchLastSection(true);
  table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  table_->setColumnWidth(1, 110);
  table_->setColumnWidth(2, 110);
  table_->setColumnWidth(3, 160);
  layout->addWidget(table_, 1);
  setCentralWidget(central);

  progress_ = new QProgressBar(this);
  progress_->setObjectName(QStringLiteral("progressBar"));
  progress_->setVisible(false);
  cancelButton_ = new QPushButton(QStringLiteral("取消解压"), this);
  cancelButton_->setObjectName(QStringLiteral("cancelButton"));
  cancelButton_->setVisible(false);
  status_ = new QLabel(this);
  status_->setObjectName(QStringLiteral("statusLabel"));
  status_->setTextFormat(Qt::PlainText);
  statusBar()->addWidget(status_, 1);
  auto *offline = new QLabel(QStringLiteral("● 本地处理 · 无需联网"), this);
  offline->setStyleSheet(QStringLiteral("color: #417155; padding: 4px 10px;"));
  statusBar()->addPermanentWidget(offline);
  statusBar()->addPermanentWidget(progress_);
  statusBar()->addPermanentWidget(cancelButton_);

  connect(openButton_, &QPushButton::clicked, this, &MainWindow::chooseArchive);
  connect(addButton_, &QPushButton::clicked, this, &MainWindow::addFiles);
  connect(extractButton_, &QPushButton::clicked, this, [this] {
    if (busy_ || !archive_) return;
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("解压"));
    auto *box = new QVBoxLayout(&dialog);
    auto *edit = new QLineEdit(QDir::toNativeSeparators(suggestedExtractDirectory(archivePath_)), &dialog);
    edit->setObjectName(QStringLiteral("destinationEdit"));
    auto *browse = new QPushButton(QStringLiteral("浏览…"), &dialog);
    auto *row = new QHBoxLayout();
    row->addWidget(edit, 1);
    row->addWidget(browse);
    box->addWidget(new QLabel(QStringLiteral("解压到"), &dialog));
    box->addLayout(row);
    dialog.resize(560, 330);
    auto *nested = new QCheckBox(QStringLiteral("自动解开内层压缩包，直到没有压缩包"), &dialog);
    nested->setChecked(true);
    nested->setObjectName(QStringLiteral("nestedCheck"));
    box->addWidget(nested);
    auto *form = new QFormLayout();
    auto *depth = new QSpinBox(&dialog);
    depth->setObjectName(QStringLiteral("maxDepthSpin"));
    depth->setRange(1, 32);
    depth->setValue(nestedDepth_);
    depth->setSuffix(QStringLiteral(" 层"));
    form->addRow(QStringLiteral("最多解开"), depth);
    auto *mode = new QComboBox(&dialog);
    mode->setObjectName(QStringLiteral("nestedModeCombo"));
    mode->addItems({QStringLiteral("全部自动解开（多个包分别存放）"), QStringLiteral("每层让我选择继续或停止")});
    mode->setCurrentIndex(askLayers_ ? 1 : 0);
    form->addRow(QStringLiteral("内层处理"), mode);
    auto *limit = new QComboBox(&dialog);
    limit->addItems({QStringLiteral("64 GiB"), QStringLiteral("4 GiB"), QStringLiteral("16 GiB")});
    limit->setCurrentIndex(nestedByteLimitGiB_ == 4 ? 1 : nestedByteLimitGiB_ == 16 ? 2 : 0);
    form->addRow(QStringLiteral("累计解压上限"), limit);
    box->addLayout(form);
    auto *keep = new QCheckBox(QStringLiteral("保留解出的中间压缩包"), &dialog);
    keep->setChecked(keepIntermediate_);
    box->addWidget(keep);
    auto *note = new QLabel(QStringLiteral("原始压缩包始终保留。遇到缺卷或同名文件会停止，已解出的文件可保留。"), &dialog);
    note->setWordWrap(true);
    box->addWidget(note);
    connect(nested, &QCheckBox::toggled, &dialog, [=](bool enabled) {
      depth->setEnabled(enabled); mode->setEnabled(enabled); keep->setEnabled(enabled);
    });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    chineseButtons(buttons);
    auto *ok = buttons->button(QDialogButtonBox::Ok);
    ok->setObjectName(QStringLiteral("extractOk"));
    ok->setText(QStringLiteral("确定"));
    box->addWidget(buttons);
    connect(browse, &QPushButton::clicked, &dialog, [this, edit] {
      const QString start = existingDirectoryForDialog(edit->text());
      const QString path =
          QFileDialog::getExistingDirectory(this, QStringLiteral("选择输出文件夹"), start);
      if (!path.isEmpty()) edit->setText(QDir::toNativeSeparators(path));
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    nestedDepth_ = depth->value();
    askLayers_ = mode->currentIndex() == 1;
    keepIntermediate_ = keep->isChecked();
    nestedByteLimitGiB_ = limit->currentIndex() == 1 ? 4 : limit->currentIndex() == 2 ? 16 : 64;
    startExtract(archivePath_, normalizeExtractDirectory(edit->text(), archivePath_), nested->isChecked());
  });
  connect(testButton_, &QPushButton::clicked, this, &MainWindow::testArchive);
  connect(infoButton_, &QPushButton::clicked, this, &MainWindow::showInfo);
  connect(upButton_, &QPushButton::clicked, this, &MainWindow::goUp);
  connect(cancelButton_, &QPushButton::clicked, this, &MainWindow::cancelExtraction);
  connect(pathBar_, &QLineEdit::returnPressed, this, [this] {
    const QString path = QDir::fromNativeSeparators(pathBar_->text().trimmed());
    QFileInfo info(path);
    if (info.isDir()) browseDirectory(info.absoluteFilePath());
    else if (info.isFile()) openPath(info.absoluteFilePath());
  });
  connect(table_, &QTableView::doubleClicked, this, &MainWindow::activateRow);
  auto *enter = new QAction(this);
  enter->setShortcut(QKeySequence(Qt::Key_Return));
  enter->setShortcutContext(Qt::WidgetShortcut);
  table_->addAction(enter);
  connect(enter, &QAction::triggered, this, [this] { activateRow(table_->currentIndex()); });
  timer_ = new QTimer(this);
  timer_->setInterval(100);
  connect(timer_, &QTimer::timeout, this, &MainWindow::refreshProgress);
  setBusy(false);
  browseDirectory(QDir::homePath());
}

MainWindow::~MainWindow() = default;

void MainWindow::setBusy(bool busy, bool extracting) {
  busy_ = busy;
  extracting_ = extracting;
  openButton_->setEnabled(!busy);
  addButton_->setEnabled(!busy && !insideArchive_);
  extractButton_->setEnabled(!busy && archive_ != nullptr);
  testButton_->setEnabled(!busy && archive_ != nullptr);
  infoButton_->setEnabled(!busy);
  upButton_->setEnabled(!busy);
  pathBar_->setEnabled(!busy);
  cancelButton_->setVisible(busy && extracting);
  cancelButton_->setEnabled(busy && extracting);
  progress_->setVisible(busy);
  if (busy) {
    progress_->setRange(0, 0);
    timer_->start();
  } else {
    timer_->stop();
  }
}

void MainWindow::browseDirectory(const QString &path) {
  QFileInfo info(path);
  if (!info.isDir()) return;
  insideArchive_ = false;
  archive_.reset();
  archiveRows_.clear();
  archivePrefix_.clear();
  directory_ = info.absoluteFilePath();
  pathBar_->setText(QDir::toNativeSeparators(directory_));
  setWindowTitle(QStringLiteral("MilkZip"));
  QVector<ArchiveRow> rows;
  if (!info.isRoot()) {
    ArchiveRow parent;
    parent.name = QStringLiteral("..");
    parent.path = info.dir().absolutePath();
    parent.directory = true;
    parent.parent = true;
    rows.push_back(parent);
  }
  const auto entries = QDir(directory_).entryInfoList(
      QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDir::DirsFirst | QDir::Name);
  for (const QFileInfo &entry : entries) {
    ArchiveRow row;
    row.name = entry.fileName();
    row.path = entry.absoluteFilePath();
    row.size = entry.isDir() ? 0 : static_cast<uint64_t>(entry.size());
    row.modified = entry.lastModified().toSecsSinceEpoch();
    row.directory = entry.isDir();
    row.symlink = entry.isSymLink();
    row.archive = !entry.isDir() && looksLikeArchive(entry.fileName());
    rows.push_back(row);
  }
  model_->setRows(rows);
  status_->setText(QStringLiteral("%1 个对象").arg(entries.size()));
  setBusy(false);
}

void MainWindow::fillArchiveRows(const QVector<ArchiveRow> &rows) {
  QVector<ArchiveRow> view;
  if (!archivePrefix_.isEmpty()) {
    ArchiveRow parent;
    parent.name = QStringLiteral("..");
    parent.parent = true;
    parent.directory = true;
    view.push_back(parent);
  }
  // A ZIP may list a directory explicitly and also list files below it.
  // Both paths must produce exactly one row for that directory.
  QSet<QString> folders;
  for (const ArchiveRow &row : rows) {
    if (!row.path.startsWith(archivePrefix_)) continue;
    const QString rest = row.path.mid(archivePrefix_.size());
    if (rest.isEmpty()) continue;
    const int slash = rest.indexOf(QLatin1Char('/'));
    if (slash >= 0) {
      const QString folder = rest.left(slash);
      if (folders.contains(folder)) continue;
      folders.insert(folder);
      ArchiveRow dir;
      dir.name = folder;
      dir.path = archivePrefix_ + folder + QLatin1Char('/');
      dir.directory = true;
      view.push_back(dir);
    } else if (row.directory) {
      if (folders.contains(rest)) continue;
      folders.insert(rest);
      ArchiveRow dir = row;
      dir.name = rest;
      view.push_back(dir);
    } else {
      ArchiveRow file = row;
      file.name = rest;
      view.push_back(file);
    }
  }
  model_->setRows(view);
}

void MainWindow::enterArchiveFolder(const QString &prefix) {
  archivePrefix_ = prefix;
  pathBar_->setText(QDir::toNativeSeparators(archivePath_) + QLatin1Char('/') +
                    archivePrefix_.left(archivePrefix_.size() - (archivePrefix_.endsWith(QLatin1Char('/')) ? 1 : 0)));
  fillArchiveRows(archiveRows_);
}

void MainWindow::goUp() {
  if (busy_) return;
  if (insideArchive_) {
    if (!archivePrefix_.isEmpty()) {
      QString prefix = archivePrefix_;
      if (prefix.endsWith(QLatin1Char('/'))) prefix.chop(1);
      const int slash = prefix.lastIndexOf(QLatin1Char('/'));
      enterArchiveFolder(slash < 0 ? QString() : prefix.left(slash + 1));
      return;
    }
    const QString back = QFileInfo(archivePath_).absolutePath();
    browseDirectory(back);
    return;
  }
  QDir dir(directory_);
  if (dir.cdUp()) browseDirectory(dir.absolutePath());
}

void MainWindow::activateRow(const QModelIndex &index) {
  if (busy_) return;
  const ArchiveRow *row = model_->rowAt(index.row());
  if (!row) return;
  if (row->parent) {
    goUp();
    return;
  }
  if (insideArchive_) {
    if (row->directory) {
      const QString path = row->path;
      enterArchiveFolder(path.endsWith(QLatin1Char('/')) ? path : path + QLatin1Char('/'));
    } else {
      // The inner entry cannot be opened directly; run the same default
      // nested extraction as the toolbar button on the containing archive.
      const QString path = archivePath_;
      quickExtract(path);
    }
    return;
  }
  if (row->directory) {
    const QString path = row->path;
    browseDirectory(path);
  } else {
    // Copy before any model reset; rows_ owns the original string.
    const QString path = row->path;
    quickExtract(path);
  }
}

void MainWindow::chooseArchive() {
  if (busy_) return;
  const QString path = QFileDialog::getOpenFileName(
      this, QStringLiteral("打开"), directory_,
      QStringLiteral("所有文件 (*);;压缩包 (*.zip *.7z *.tar *.tgz *.tar.gz *.tbz *.tbz2 *.tar.bz2 "
                     "*.txz *.tar.xz *.tzst *.tar.zst *.rar *.001 *.gz *.bz2 *.xz *.zst *.lz4 *.lzma *.Z *.cab *.iso *.udf *.wim *.swm *.arj *.lzh *.lha *.cpio *.ar *.deb *.rpm *.dmg *.xar *.mp4 *.mkv *.avi *.mov *.m4v *.webm *.jpg *.jpeg *.png *.gif *.bmp *.webp *.exe *.sfx *.zipsc)"));
  if (!path.isEmpty()) openPath(path);
}

void MainWindow::addFiles() {
  if (busy_ || insideArchive_) return;
  const QStringList files = QFileDialog::getOpenFileNames(this, QStringLiteral("添加到压缩包"), directory_);
  if (files.isEmpty()) return;
  const QString zip = QFileDialog::getSaveFileName(this, QStringLiteral("保存 ZIP"), directory_,
                                                   QStringLiteral("ZIP (*.zip)"));
  if (zip.isEmpty()) return;
  QByteArray zipBytes = zip.toUtf8();
  QVector<QByteArray> stored;
  QVector<const char *> views;
  for (const QString &file : files) {
    stored.append(file.toUtf8());
    views.append(stored.back().constData());
  }
  const int code = archiver_create_zip(zipBytes.constData(), views.data(), static_cast<uint32_t>(views.size()));
  if (code != ARCHIVER_OK) showError(code, {});
  else {
    status_->setText(QStringLiteral("已创建 %1").arg(QDir::toNativeSeparators(zip)));
    browseDirectory(QFileInfo(zip).absolutePath());
  }
}

void MainWindow::testArchive() {
  if (busy_ || !archive_) return;
  auto session = archive_;
  setBusy(true);
  status_->setText(QStringLiteral("正在测试压缩包…"));
  auto *watcher = new QFutureWatcher<ExtractResult>(this);
  connect(watcher, &QFutureWatcher<ExtractResult>::finished, this, [this, watcher] {
    const auto result = watcher->result();
    watcher->deleteLater();
    setBusy(false);
    if (closeAfterFinish_) { close(); return; }
    if (result.code == ARCHIVER_OK) status_->setText(QStringLiteral("测试通过。"));
    else showError(result.code, result.detail);
  });
  watcher->setFuture(QtConcurrent::run([session] {
    ExtractResult result;
    result.code = archiver_test(session->handle, session->secret ? session->secret->bytes.constData() : nullptr);
    result.detail = QString::fromUtf8(archiver_message(session->handle));
    return result;
  }));
}

void MainWindow::showInfo() {
  if (insideArchive_) {
    uint32_t count = 0;
    archiver_entry_count(archive_->handle, &count);
    uint64_t bytes = 0;
    uint64_t packed = 0;
    for (const ArchiveRow &row : archiveRows_) {
      if (!row.directory) {
        bytes += row.size;
        packed += row.packed;
      }
    }
    QMessageBox::information(
        this, QStringLiteral("信息"),
        QStringLiteral("文件：%1\n大小：%2\n压缩后大小：%3\n%4")
            .arg(count)
            .arg(QLocale().formattedDataSize(bytes))
            .arg(QLocale().formattedDataSize(packed))
            .arg(QDir::toNativeSeparators(archivePath_)));
    return;
  }
  status_->setText(QStringLiteral("%1").arg(QDir::toNativeSeparators(directory_)));
}

void MainWindow::showPasswordBook() {
  if (busy_) return;
  if (!PasswordBook::available()) {
    QMessageBox::information(this, QStringLiteral("密码本"),
                             QStringLiteral("当前发布版使用 Windows 凭据管理器保存密码。其他系统暂不提供持久密码本。"));
    return;
  }
  QVector<QByteArray> items = PasswordBook::load();
  QDialog dialog(this);
  dialog.setWindowTitle(QStringLiteral("本地密码本"));
  dialog.resize(480, 400);
  auto *layout = new QVBoxLayout(&dialog);
  auto *hint = new QLabel(QStringLiteral("按列表顺序尝试，全部失败后再询问。最多 64 条，每条不超过 1024 UTF-8 字节。"), &dialog);
  hint->setWordWrap(true);
  layout->addWidget(hint);
  auto *list = new QListWidget(&dialog);
  list->setObjectName(QStringLiteral("passwordList"));
  auto refill = [&] {
    list->clear();
    for (int i = 0; i < items.size(); ++i)
      list->addItem(QStringLiteral("密码 %1    ••••••••").arg(i + 1));
  };
  auto add = [&](const QByteArray &value) {
    if (value.isEmpty() || value.size() > PasswordBook::maxPasswordBytes || value.contains('\0') ||
        items.contains(value) || items.size() >= PasswordBook::maxPasswords) return false;
    items.append(value);
    return true;
  };
  refill();
  layout->addWidget(list);
  auto *row = new QHBoxLayout();
  auto *create = new QPushButton(QStringLiteral("添加…"), &dialog);
  create->setObjectName(QStringLiteral("addPasswordButton"));
  auto *edit = new QPushButton(QStringLiteral("修改…"), &dialog);
  auto *import = new QPushButton(QStringLiteral("导入文本…"), &dialog);
  auto *remove = new QPushButton(QStringLiteral("删除"), &dialog);
  auto *up = new QPushButton(QStringLiteral("上移"), &dialog);
  auto *down = new QPushButton(QStringLiteral("下移"), &dialog);
  for (auto *button : {create, edit, import, remove, up, down}) row->addWidget(button);
  layout->addLayout(row);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    chineseButtons(buttons);
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
    if (PasswordBook::store(items)) dialog.accept();
    else QMessageBox::warning(&dialog, QStringLiteral("保存失败"), QStringLiteral("无法写入 Windows 凭据管理器。原密码本已保留。"));
  });
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  auto input = [&](bool editing) {
    const int selected = list->currentRow();
    if (editing && selected < 0) return;
    bool accepted = false;
    QString value = QInputDialog::getText(&dialog, editing ? QStringLiteral("修改密码") : QStringLiteral("添加密码"),
        QStringLiteral("密码（保留前后空格）"), QLineEdit::Password, {}, &accepted);
    if (accepted) {
      QByteArray bytes = value.toUtf8();
      if (editing && !bytes.isEmpty() && bytes.size() <= PasswordBook::maxPasswordBytes && !bytes.contains('\0')) {
        volatile char *data = items[selected].data();
        for (qsizetype i = 0; i < items[selected].size(); ++i) data[i] = 0;
        items[selected] = bytes;
      } else if (!editing && add(bytes)) {
        // Added above.
      } else QMessageBox::information(&dialog, QStringLiteral("未添加"), QStringLiteral("密码为空、重复、过长或列表已满。"));
      volatile char *data = bytes.data();
      for (qsizetype i = 0; i < bytes.size(); ++i) data[i] = 0;
      refill();
    }
    value.fill(QChar(0));
  };
  connect(create, &QPushButton::clicked, &dialog, [&] { input(false); });
  connect(edit, &QPushButton::clicked, &dialog, [&] { input(true); });
  connect(import, &QPushButton::clicked, &dialog, [&] {
    const QString path = QFileDialog::getOpenFileName(&dialog, QStringLiteral("导入 UTF-8 密码文本（每行一个）"), {}, QStringLiteral("文本 (*.txt);;所有文件 (*)"));
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) {
      QMessageBox::warning(&dialog, QStringLiteral("无法导入"), QStringLiteral("请选择可读且不超过 1 MiB 的 UTF-8 文本。"));
      return;
    }
    QByteArray content = file.readAll();
    if (content.startsWith("\xef\xbb\xbf")) content.remove(0, 3);
    auto lines = content.split('\n');
    int added = 0;
    for (auto &line : lines) {
      if (line.endsWith('\r')) line.chop(1);
      // Reject invalid UTF-8 instead of silently changing a password.
      if (QString::fromUtf8(line).toUtf8() == line && add(line)) ++added;
      volatile char *data = line.data();
      for (qsizetype i = 0; i < line.size(); ++i) data[i] = 0;
    }
    volatile char *data = content.data();
    for (qsizetype i = 0; i < content.size(); ++i) data[i] = 0;
    refill();
    QMessageBox::information(&dialog, QStringLiteral("导入完成"), QStringLiteral("已添加 %1 条。空行、重复项、过长项及超出容量的行已跳过。点击保存后生效。").arg(added));
  });
  connect(up, &QPushButton::clicked, &dialog, [&] {
    const int index = list->currentRow();
    if (index <= 0) return;
    std::swap(items[index], items[index - 1]); refill(); list->setCurrentRow(index - 1);
  });
  connect(down, &QPushButton::clicked, &dialog, [&] {
    const int index = list->currentRow();
    if (index < 0 || index + 1 >= items.size()) return;
    std::swap(items[index], items[index + 1]); refill(); list->setCurrentRow(index + 1);
  });
  connect(remove, &QPushButton::clicked, &dialog, [&] {
    const int index = list->currentRow();
    if (index < 0) return;
    volatile char *data = items[index].data();
    for (qsizetype i = 0; i < items[index].size(); ++i) data[i] = 0;
    items.removeAt(index); refill();
  });
  dialog.exec();
  wipeList(items);
}

void MainWindow::openPath(QString path) {
  if (busy_) return;
  if (!QFileInfo(path).isFile()) {
    showError(ARCHIVER_ERR_IO, {});
    return;
  }
  // path is a copy: clearing the model below must not invalidate it.
  archive_.reset();
  model_->setRows({});
  archiveRows_.clear();
  archivePrefix_.clear();
  insideArchive_ = false;
  archivePath_ = QFileInfo(path).absoluteFilePath();
  directory_ = QFileInfo(path).absolutePath();
  pathBar_->setText(QDir::toNativeSeparators(archivePath_));
  setWindowTitle(QFileInfo(path).fileName() + QStringLiteral(" — MilkZip"));
  startOpen(archivePath_);
}

void MainWindow::quickExtract(const QString &source) {
  if (busy_) return;
  const QString path = source;
  if (!QFileInfo(path).isFile()) {
    showError(ARCHIVER_ERR_IO, {});
    return;
  }
  QString dest = suggestedExtractDirectory(path);
  if (QFileInfo::exists(dest)) {
    const QFileInfo base(dest);
    const QString parent = base.absolutePath();
    const QString name = base.fileName();
    int suffix = 2;
    do {
      dest = QDir(parent).filePath(QStringLiteral("%1 (%2)").arg(name).arg(suffix));
      ++suffix;
    } while (QFileInfo::exists(dest) && suffix < 1000);
  }
  // Defaults: nested to the depth limit, no layer prompts, no intermediate
  // archives. The only dialog this can open is the password prompt.
  startExtract(path, dest, true, {}, true);
}

std::shared_ptr<DesktopSecret> MainWindow::askPassword(bool rejected, bool *save) {
  if (save) *save = false;
  QDialog dialog(this);
  dialog.setObjectName(QStringLiteral("archivePasswordDialog"));
  dialog.setWindowTitle(QStringLiteral("压缩包密码"));
  dialog.resize(400, 180);
  auto *layout = new QVBoxLayout(&dialog);
  layout->addWidget(new QLabel(rejected ? QStringLiteral("密码不正确，请重试。")
      : QStringLiteral("已尝试本次密码和本地密码本，请输入其他密码。"), &dialog));
  auto *edit = new QLineEdit(&dialog);
  edit->setObjectName(QStringLiteral("passwordEdit"));
  edit->setEchoMode(QLineEdit::Normal);
  layout->addWidget(edit);
  auto *keep = new QCheckBox(QStringLiteral("验证成功后保存到本地密码本"), &dialog);
  keep->setVisible(PasswordBook::available());
  layout->addWidget(keep);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    chineseButtons(buttons);
  buttons->button(QDialogButtonBox::Ok)->setObjectName(QStringLiteral("passwordOk"));
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  edit->setFocus();
  if (dialog.exec() != QDialog::Accepted) { edit->clear(); return {}; }
  if (save) *save = keep->isChecked();
  auto secret = std::make_shared<DesktopSecret>();
  secret->bytes = edit->text().toUtf8();
  edit->clear();
  return secret;
}

void MainWindow::startOpen(const QString &path, std::shared_ptr<DesktopSecret> secret, bool save) {
  work_ = std::make_shared<DesktopWorkState>();
  auto state = work_;
  setBusy(true);
  status_->setText(QStringLiteral("正在打开…"));
  auto *watcher = new QFutureWatcher<OpenResult>(this);
  connect(watcher, &QFutureWatcher<OpenResult>::finished, this, [this, watcher, path, secret, save] {
    OpenResult result = watcher->result();
    watcher->deleteLater();
    if (result.code == ARCHIVER_OK) {
      archive_ = result.session;
      insideArchive_ = true;
      archiveRows_ = result.rows;
      if (save && secret && result.session->secret && result.session->secret->bytes == secret->bytes && !savePassword(secret->bytes))
        QMessageBox::warning(this, QStringLiteral("密码未保存"), QStringLiteral("密码已验证，但密码本已满或无法写入。"));
    }
    setBusy(false);
    if (closeAfterFinish_) {
      close();
      return;
    }
    if (result.code == ARCHIVER_ERR_PASSWORD_REQUIRED || result.code == ARCHIVER_ERR_PASSWORD) {
      bool save = false;
      auto nextSecret = askPassword(secret != nullptr || result.code == ARCHIVER_ERR_PASSWORD, &save);
      if (nextSecret) {
        startOpen(path, std::move(nextSecret), save);
      } else {
        insideArchive_ = false;
        model_->setRows({});
        status_->setText(QStringLiteral("已取消密码输入。"));
      }
      return;
    }
    if (result.code != ARCHIVER_OK) {
      insideArchive_ = false;
      showError(result.code, result.detail);
      browseDirectory(QFileInfo(path).absolutePath());
      return;
    }
    archivePrefix_.clear();
    fillArchiveRows(archiveRows_);
    status_->setText(QStringLiteral("共 %1 项 · %2")
                         .arg(QLocale().toString(static_cast<qlonglong>(result.rows.size())))
                         .arg(QLocale().formattedDataSize(result.total)));
  });
  watcher->setFuture(QtConcurrent::run([path, secret, state] {
    OpenResult result;
    result.session = std::make_shared<ArchiveSession>();
    QVector<QByteArray> book = PasswordBook::load();
    if (secret && !secret->bytes.isEmpty()) book.prepend(secret->bytes);
    QVector<const char *> views;
    for (const QByteArray &item : book) views.append(item.constData());
    try {
      const QByteArray pathBytes = path.toUtf8();
      int32_t matched = -1;
      result.code = archiver_open_with_passwords(
          pathBytes.constData(), nullptr, views.isEmpty() ? nullptr : views.data(),
          static_cast<uint32_t>(views.size()), &matched, &result.session->handle);
      result.detail = QString::fromUtf8(archiver_message(result.session->handle));
      result.matched = matched;
      if (result.code == ARCHIVER_OK && matched >= 0 && matched < book.size()) {
        result.session->secret = std::make_shared<DesktopSecret>();
        result.session->secret->bytes = book.at(matched);
      }
      if (result.code == ARCHIVER_OK) {
        uint32_t count = 0;
        result.code = archiver_entry_count(result.session->handle, &count);
        result.rows.reserve(static_cast<int>(count));
        for (uint32_t i = 0; result.code == ARCHIVER_OK && i < count; ++i) {
          if (state->cancel.load()) {
            result.code = ARCHIVER_ERR_CANCELLED;
            break;
          }
          ArchiverEntry entry = {};
          result.code = archiver_entry_at(result.session->handle, i, &entry);
          if (result.code != ARCHIVER_OK) break;
          ArchiveRow row;
          row.path = QString::fromUtf8(entry.path);
          row.name = row.path;
          row.size = entry.size;
          row.packed = entry.packed_size;
          row.hasPacked = true;
          row.modified = entry.modified_unix;
          row.directory = entry.is_directory != 0;
          row.symlink = entry.is_symlink != 0;
          archiver_entry_free(&entry);
          if (!row.directory) result.total += row.size;
          result.rows.push_back(std::move(row));
        }
      }
    } catch (const std::exception &) {
      result.code = ARCHIVER_ERR_IO;
    } catch (...) {
      result.code = ARCHIVER_ERR_FORMAT;
    }
    wipeList(book);
    return result;
  }));
}

void MainWindow::startExtract(const QString &sourcePath, const QString &dest, bool nested,
                              std::shared_ptr<DesktopSecret> secret, bool quick) {
  if (busy_ || sourcePath.isEmpty()) return;
  if (dest.isEmpty() || !QDir::isAbsolutePath(dest)) {
    status_->setText(QStringLiteral("请选择一个完整的输出文件夹路径。"));
    return;
  }
  auto session = archive_;
  if (!secret && session) secret = session->secret;
  work_ = std::make_shared<DesktopWorkState>();
  auto state = work_;
  const QString path = sourcePath;
  const int depth = quick ? 32 : (nested ? nestedDepth_ : 1);
  const bool askLayers = !quick && nested && askLayers_;
  const bool keep = quick ? false : keepIntermediate_;
  const uint64_t byteLimit = quick ? (64ull << 30)
                                   : static_cast<uint64_t>(nestedByteLimitGiB_) << 30;
  setBusy(true, true);
  status_->setText(QStringLiteral("正在解压…"));
  auto *watcher = new QFutureWatcher<ExtractResult>(this);
  connect(watcher, &QFutureWatcher<ExtractResult>::finished, this, [this, watcher, dest] {
    const ExtractResult result = watcher->result();
    watcher->deleteLater();
    setBusy(false);
    lastExtractReport_ = QStringLiteral("输出位置：%1\n").arg(QDir::toNativeSeparators(dest)) + result.detail;
    lastExtractReport_.replace(QStringLiteral("archives extracted: "), QStringLiteral("已解开的包数："));
    lastExtractReport_.replace(QStringLiteral("failed branch reports: "), QStringLiteral("未完成分支记录数："));
    lastExtractReport_.replace(QStringLiteral("extracted: "), QStringLiteral("已解开："));
    lastExtractReport_.replace(QStringLiteral("kept container: "), QStringLiteral("保留成品文件："));
    lastExtractReport_.replace(QStringLiteral("failed [format]: "), QStringLiteral("未解开（损坏或不完整）："));
    lastExtractReport_.replace(QStringLiteral("failed [missing-volume]: "), QStringLiteral("未解开（缺少分卷）："));
    lastExtractReport_.replace(QStringLiteral("failed [password-required]: "), QStringLiteral("未解开（需要密码）："));
    lastExtractReport_.replace(QStringLiteral("failed [unsupported]: "), QStringLiteral("未解开（暂不支持）："));
    reportAction_->setEnabled(true);
    if (closeAfterFinish_) { close(); return; }
    if (result.code == ARCHIVER_ERR_CANCELLED)
      status_->setText(QStringLiteral("已取消解压。已完成的文件和文件夹可能保留。"));
    else if (result.code != ARCHIVER_OK) showError(result.code, lastExtractReport_);
    else status_->setText(QStringLiteral("解压完成。文件已保存到 %1").arg(QDir::toNativeSeparators(dest)));
    if (result.passwordSaveFailed)
      QMessageBox::warning(this, QStringLiteral("密码未保存"), QStringLiteral("密码已验证，但密码本已满或无法写入。"));
  });
  watcher->setFuture(QtConcurrent::run([this, secret, state, dest, path, depth, askLayers, keep, byteLimit] {
    ExtractResult result;
    QVector<QByteArray> book = PasswordBook::load();
    if (secret && !book.contains(secret->bytes)) book.prepend(secret->bytes);
    QVector<const char *> views;
    for (const QByteArray &item : book) views.append(item.constData());
    struct UiCallbacks {
      std::function<int(const char *, bool, char *, uint32_t)> password;
      std::function<int(const char *const *, uint32_t)> choose;
      std::shared_ptr<DesktopSecret> last;
      bool save = false;
      bool saveFailed = false;
    } ui;
    ui.password = [&](const char *archive, bool rejected, char *buffer, uint32_t capacity) {
      if (state->cancel.load()) return 0;
      const QString current = QFileInfo(QString::fromUtf8(archive)).fileName();
      std::shared_ptr<DesktopSecret> entered;
      bool save = false;
      QMetaObject::invokeMethod(this, [&] {
        if (closeAfterFinish_ || state->cancel.load()) return;
        status_->setText(QStringLiteral("等待密码：%1").arg(current));
        entered = askPassword(rejected, &save);
      }, Qt::BlockingQueuedConnection);
      if (!entered || state->cancel.load() || static_cast<uint32_t>(entered->bytes.size()) >= capacity) return 0;
      memcpy(buffer, entered->bytes.constData(), entered->bytes.size());
      buffer[entered->bytes.size()] = 0;
      ui.last = entered; ui.save = save;
      return 1;
    };
    ui.choose = [&](const char *const *paths, uint32_t count) {
      int decision = -2;
      QStringList choices;
      for (uint32_t i = 0; i < count; ++i) choices.append(QString::fromUtf8(paths[i]));
      QMetaObject::invokeMethod(this, [&] {
        if (closeAfterFinish_ || state->cancel.load()) return;
        QDialog dialog(this);
        dialog.setWindowTitle(QStringLiteral("发现内层压缩包"));
        dialog.resize(520, 340);
        auto *layout = new QVBoxLayout(&dialog);
        layout->addWidget(new QLabel(QStringLiteral("选择继续解开的压缩包，或保留当前内容并停止。"), &dialog));
        auto *list = new QListWidget(&dialog);
        list->setObjectName(QStringLiteral("nestedChoiceList"));
        list->addItems(choices); list->setCurrentRow(0); layout->addWidget(list);
        auto *buttons = new QDialogButtonBox(&dialog);
    chineseButtons(buttons);
        auto *selected = buttons->addButton(QStringLiteral("解开选中包"), QDialogButtonBox::AcceptRole);
        auto *all = buttons->addButton(QStringLiteral("全部解开"), QDialogButtonBox::ActionRole);
        auto *stop = buttons->addButton(QStringLiteral("在此停止"), QDialogButtonBox::RejectRole);
        selected->setObjectName(QStringLiteral("continueNestedButton"));
        stop->setObjectName(QStringLiteral("stopNestedButton"));
        layout->addWidget(buttons);
        connect(selected, &QPushButton::clicked, &dialog, [&] { decision = list->currentRow(); dialog.accept(); });
        connect(all, &QPushButton::clicked, &dialog, [&] { decision = -3; dialog.accept(); });
        connect(stop, &QPushButton::clicked, &dialog, [&] { decision = -1; dialog.accept(); });
        dialog.exec();
      }, Qt::BlockingQueuedConnection);
      return state->cancel.load() ? -2 : decision;
    };
    try {
      if (!QDir().mkpath(dest)) result.code = ARCHIVER_ERR_IO;
      else {
        const QByteArray pathBytes = path.toUtf8();
        const QByteArray destBytes = dest.toUtf8();
        char message[65536] = {};
        ArchiverNestedOptions options = {};
        options.max_depth = static_cast<uint32_t>(depth);
        options.max_output_bytes = byteLimit;
        options.keep_intermediate = keep;
        options.all_branches = 1;
        options.user = &ui;
        options.password = [](const char *archive, int rejected, char *buffer, uint32_t capacity, void *user) {
          return static_cast<UiCallbacks *>(user)->password(archive, rejected != 0, buffer, capacity);
        };
        options.password_accepted = [](const char *password, void *user) {
          auto *ui = static_cast<UiCallbacks *>(user);
          if (ui->save && ui->last && ui->last->bytes == password && !savePassword(ui->last->bytes)) ui->saveFailed = true;
          ui->save = false;
        };
        if (askLayers) options.choose = [](const char *const *paths, uint32_t count, void *user) {
          return static_cast<UiCallbacks *>(user)->choose(paths, count);
        };
        result.code = archiver_extract_nested_ex(pathBytes.constData(), nullptr, destBytes.constData(),
            views.data(), static_cast<uint32_t>(views.size()), &options, reportProgress, state.get(), message, sizeof(message));
        result.detail = QString::fromUtf8(message);
        result.passwordSaveFailed = ui.saveFailed;
      }
    } catch (...) { result.code = ARCHIVER_ERR_IO; }
    wipeList(book);
    return result;
  }));
}

void MainWindow::refreshProgress() {
  if (!busy_ || !extracting_ || !work_ || work_->cancel.load()) return;
  const uint64_t total = work_->total.load();
  const uint64_t completed = work_->completed.load();
  if (total) {
    progress_->setRange(0, 1000);
    progress_->setValue(static_cast<int>(std::min<uint64_t>(1000, completed * 1000 / total)));
    status_->setText(QStringLiteral("正在解压… %1 / %2")
                         .arg(QLocale().formattedDataSize(completed), QLocale().formattedDataSize(total)));
  }
}

void MainWindow::cancelExtraction() {
  if (!busy_ || !extracting_ || !work_) return;
  work_->cancel.store(true);
  cancelButton_->setEnabled(false);
  status_->setText(QStringLiteral("正在取消，清理当前未完成的文件…"));
}

void MainWindow::showError(int result, const QString &detail) {
  const QString text = errorText(result, detail);
  status_->setText(text);
  QMessageBox dialog(QMessageBox::Warning, QStringLiteral("MilkZip"), text, QMessageBox::Ok, this);
  if (!detail.isEmpty()) dialog.setDetailedText(detail);
  dialog.exec();
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event) {
  const auto urls = event->mimeData()->urls();
  if (!busy_ && urls.size() == 1 && urls.first().isLocalFile()) event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event) {
  const auto urls = event->mimeData()->urls();
  if (!busy_ && urls.size() == 1 && urls.first().isLocalFile()) {
    event->acceptProposedAction();
    openPath(urls.first().toLocalFile());
  }
}

void MainWindow::closeEvent(QCloseEvent *event) {
  if (busy_) {
    closeAfterFinish_ = true;
    if (work_) work_->cancel.store(true);
    status_->setText(QStringLiteral("正在结束当前任务，完成清理后关闭窗口…"));
    event->ignore();
  } else {
    event->accept();
  }
}
