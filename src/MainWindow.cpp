#include "MainWindow.h"

#include "Bookmarks.h"
#include "ConflictDialog.h"
#include "DirectoryModel.h"
#include "FileFilterProxy.h"
#include "FileItemDelegate.h"
#include "FileOperations.h"
#include "HeaderBar.h"
#include "OperationsPanel.h"
#include "PathBar.h"
#include "PreferencesDialog.h"
#include "Thumbnailer.h"
#if defined(KAFTILOUS_TERMINAL)
#include "TerminalWidget.h"
#endif
#include "Toast.h"
#include "Trash.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QLocale>
#include <QActionGroup>
#include <QMenu>
#include <QMenuBar>
#include <QMimeData>
#include <QMessageBox>
#include <QMouseEvent>
#include <QFileSystemWatcher>
#include <QInputDialog>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSocketNotifier>
#include <QRegularExpression>
#include <QScrollBar>
#include <QPainter>
#include <QPointer>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QStyledItemDelegate>
#include <QTabBar>
#include <QTimer>
#include <QTreeView>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>

#if defined(Q_OS_UNIX)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

constexpr int PathRole = Qt::UserRole;
constexpr int SeparatorRole = Qt::UserRole + 1;
constexpr int KindRole = Qt::UserRole + 2;  // SidebarKind
constexpr int EjectRole = Qt::UserRole + 3; // bool: show an eject button
enum SidebarKind { PlaceItem, BookmarkItem, DriveItem };

QStringList localPaths(const QMimeData *mime);

// The eject button's area inside a sidebar row.
QRect ejectRect(const QRect &row)
{
    return QRect(row.right() - 30, row.center().y() - 12, 24, 24);
}
constexpr int ResizeMargin = 5;

// Separator rows are drawn as a thin line; other rows are drawn normally so
// the style sheet's rounded hover/selection applies.
class SidebarDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QColor lineColor;

    QIcon ejectIcon;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        if (!index.data(SeparatorRole).toBool()) {
            QStyleOptionViewItem opt = option;
            if (index.data(EjectRole).toBool())
                opt.rect.setRight(opt.rect.right() - 30); // room for the button
            QStyledItemDelegate::paint(painter, opt, index);
            if (index.data(EjectRole).toBool())
                ejectIcon.paint(painter, ejectRect(option.rect).adjusted(4, 4, -4, -4));
            return;
        }
        const int y = option.rect.center().y();
        painter->save();
        painter->setPen(QPen(lineColor, 1));
        painter->drawLine(option.rect.left() + 4, y, option.rect.right() - 4, y);
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        if (index.data(SeparatorRole).toBool())
            return QSize(0, 13);
        return QStyledItemDelegate::sizeHint(option, index);
    }
};

QString styleSheetFor(const Theme &t)
{
    QString css = QStringLiteral(R"(
QWidget#root { background: @outline; }
QWidget#sidebarPane { background: @sidebar; }
QWidget#contentPane { background: @window; }
QLabel#title { color: @fg; font-weight: bold; }

QToolButton#flat { background: transparent; border: none; border-radius: 6px;
    padding: 5px; color: @fg; }
QToolButton#flat:hover { background: @hover; }
QToolButton#flat:pressed, QToolButton#flat:checked { background: @pressed; }
QToolButton#flat::menu-indicator { image: none; width: 0px; }

QToolButton#win { background: @button; border: none; border-radius: 12px; }
QToolButton#win:hover { background: @buttonHover; }
QToolButton#win:pressed { background: @pressed; }

QFrame#pathBar { background: @pathBg; border-radius: 8px; }
QToolButton#crumb, QToolButton#crumbCurrent { background: transparent; border: none;
    border-radius: 6px; padding: 3px 7px; color: @dim; }
QToolButton#crumbCurrent { color: @fg; font-weight: bold; }
QToolButton#crumb:hover, QToolButton#crumbCurrent:hover { background: @hover; color: @fg; }
QLabel#crumbSep { color: @dim; }
QLineEdit#pathEdit { background: transparent; border: none; color: @fg; padding: 0px 8px;
    selection-background-color: @accent; }
QLineEdit#searchEdit { background: @pathBg; border: none; border-radius: 8px; color: @fg;
    padding: 0px 10px; selection-background-color: @accent; }

QListWidget#sidebar { background: @sidebar; color: @fg; border: none; padding: 0px 6px 6px 6px;
    outline: 0; }
QListWidget#sidebar::item { height: 36px; padding-left: 8px; border: none; border-radius: 6px;
    margin: 1px 0px; color: @fg; }
QListWidget#sidebar::item:hover { background: @hover; }
QListWidget#sidebar::item:selected { background: @selected; color: @fg; }

QListView#files { background: @window; border: none; outline: 0; padding: 6px 6px; }
QTreeView#detailView { background: @window; border: none; outline: 0; padding: 0px 6px 6px 6px; }
QTreeView#detailView::item, QTreeView#detailView::item:selected,
QTreeView#detailView::branch { background: transparent; }
QTreeView#detailView QHeaderView { background: @window; border: none; }
QTreeView#detailView QHeaderView::section { background: @window; color: @dim; border: none;
    padding: 8px 10px 6px 16px; font-weight: bold; }
QTreeView#detailView QHeaderView::section:hover { color: @fg; }
QLabel#floatingBar { background: @popup; color: @fg; border: 1px solid @border;
    border-radius: 8px; padding: 5px 12px; }

QSplitter#mainSplitter::handle { background: @splitter; }

QScrollBar:vertical { background: transparent; width: 10px; margin: 2px 0px; }
QScrollBar::handle:vertical { background: @scroll; border-radius: 3px; min-height: 32px;
    margin: 0px 2px; }
QScrollBar::handle:vertical:hover { background: @scrollHover; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 0px 2px; }
QScrollBar::handle:horizontal { background: @scroll; border-radius: 3px; min-width: 32px;
    margin: 2px 0px; }
QScrollBar::handle:horizontal:hover { background: @scrollHover; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0px; height: 0px; }
QScrollBar::add-page, QScrollBar::sub-page { background: none; }

QMenu { background: @popup; color: @fg; border: 1px solid @border; border-radius: 10px;
    padding: 6px; }
QMenu::item { padding: 7px 32px 7px 28px; border-radius: 6px; }
QMenu::item:selected { background: @hover; }
QMenu::item:disabled { color: @dim; }
QMenu::separator { height: 1px; background: @border; margin: 5px 6px; }

QFrame#opsPanel { background: @popup; border: 1px solid @border; border-radius: 12px; }
QLabel#opsTitle { color: @fg; }
QLabel#opsDetail { color: @dim; }
QProgressBar#opsProgress { background: @pathBg; border: none; border-radius: 3px; }
QProgressBar#opsProgress::chunk { background: @accent; border-radius: 3px; }
QFrame#toast { background: @popup; border: 1px solid @border; border-radius: 20px; }
QLabel#toastText { color: @fg; }
QToolButton#toastAction { background: @button; color: @fg; border: none; border-radius: 14px;
    padding: 5px 14px; font-weight: bold; }
QToolButton#toastAction:hover { background: @buttonHover; }

QMenuBar#menuBar { background: @sidebar; color: @fg; border: none;
    border-bottom: 1px solid @border; padding: 2px 4px; }
QMenuBar#menuBar::item { background: transparent; padding: 4px 10px; border-radius: 6px; }
QMenuBar#menuBar::item:selected { background: @hover; }
QMenuBar#menuBar::item:pressed { background: @pressed; }
QLineEdit#renameEdit { background: @window; color: @fg; border: 2px solid @accent;
    border-radius: 6px; padding: 1px 4px; selection-background-color: @accent;
    selection-color: #ffffff; }

QTabBar#tabs { background: @window; border: none; }
QTabBar#tabs::tab { background: transparent; color: @dim; border: none; border-radius: 6px;
    padding: 5px 8px 5px 14px; margin: 3px 2px; min-width: 120px; }
QTabBar#tabs::tab:selected { background: @pathBg; color: @fg; }
QTabBar#tabs::tab:hover:!selected { background: @hover; }
QWidget#trashBar { background: @pathBg; border-bottom: 1px solid @border; }
QLabel#trashBarText { color: @fg; }
QToolButton#barButton { background: @button; color: @fg; border: none; border-radius: 6px;
    padding: 5px 14px; font-weight: bold; }
QToolButton#barButton:hover { background: @buttonHover; }
QToolButton#barButton:disabled { color: @dim; }
QWidget#terminalPanel { background: @window; }
QWidget#terminalBar { background: @window; border-top: 1px solid @border; }
QLabel#terminalTitle { color: @dim; font-weight: bold; }
QSplitter#contentSplitter::handle { background: @border; }

QDialog#conflictDialog { background: @window; }
QLabel#conflictTitle { color: @fg; font-weight: bold; font-size: 15px; }
QLabel#conflictText { color: @dim; }
QCheckBox#conflictCheck { color: @fg; spacing: 8px; }
QCheckBox#conflictCheck::indicator { width: 14px; height: 14px; border: 1px solid @dim;
    border-radius: 4px; background: @pathBg; }
QCheckBox#conflictCheck::indicator:checked { background: @accent; border-color: @accent; }
QWidget#conflictCard { background: @pathBg; border-radius: 10px; }
QLabel#conflictCardHeading { color: @fg; font-weight: bold; }
QLineEdit#conflictName { background: @pathBg; color: @fg; border: none; border-radius: 6px;
    padding: 6px 8px; selection-background-color: @accent; }
QPushButton#conflictButton { background: @button; color: @fg; border: none; border-radius: 6px;
    padding: 7px 14px; }
QPushButton#conflictButton:hover { background: @buttonHover; }
QPushButton#conflictPrimary { background: @accent; color: #ffffff; border: none;
    border-radius: 6px; padding: 7px 16px; font-weight: bold; }

QDialog#preferences { background: @window; }
QLabel#prefHeading { color: @fg; font-weight: bold; font-size: 15px; }
QLabel#prefHint { color: @dim; }
QToolButton#themeCard { background: transparent; color: @fg; border: 2px solid transparent;
    border-radius: 12px; padding: 8px 8px 6px 8px; }
QToolButton#themeCard:hover { background: @hover; }
QToolButton#themeCard:checked { border-color: @accent; }
)");
    const QList<std::pair<const char *, QColor>> tokens = {
        {"@outline", t.outline},   {"@sidebar", t.sidebar},       {"@window", t.window},
        {"@fg", t.fg},             {"@dim", t.dimFg},             {"@hover", t.hover},
        {"@pressed", t.pressed},   {"@selected", t.selected},     {"@buttonHover", t.buttonHover},
        {"@button", t.button},     {"@pathBg", t.pathBg},         {"@accent", t.accent},
        {"@popup", t.popup},       {"@border", t.border},         {"@scrollHover", t.scrollHover},
        {"@scroll", t.scroll},     {"@splitter", t.dark ? t.sidebar : t.border},
    };
    // Longer tokens first where one is a prefix of another (@button/@buttonHover…).
    for (const auto &[token, color] : tokens)
        css.replace(QLatin1String(token), cssColor(color));
    return css;
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    m_csd = qEnvironmentVariableIsEmpty("KAFTILOUS_NATIVE_DECORATIONS");
#if defined(Q_OS_MACOS)
    m_csd = false; // keep the native traffic-light title bar on macOS
#endif
    if (m_csd)
        setWindowFlag(Qt::FramelessWindowHint);

    resize(1150, 720);
    setWindowTitle("Kaftilous");

    m_ops = new FileOperations(this);
    m_ops->setAskOnConflict(true);
    connect(m_ops, &FileOperations::jobFinished, this, &MainWindow::onJobFinished);
    connect(m_ops, &FileOperations::conflictRaised, this, &MainWindow::askConflict);
    connect(m_ops, &FileOperations::undoChanged, this, &MainWindow::updateEditActions);
    connect(QApplication::clipboard(), &QClipboard::dataChanged, this,
            &MainWindow::updateEditActions);

    m_sidebarRefresh = new QTimer(this);
    m_sidebarRefresh->setSingleShot(true);
    m_sidebarRefresh->setInterval(250);
    connect(m_sidebarRefresh, &QTimer::timeout, this, [this] {
        populateSidebar();
        syncSidebarSelection();
    });

    setupActions();
    setupUi();
    applyTheme(true);
    updateWindowState();
    loadSettings();

    // The sidebar follows bookmark edits (also Nautilus's) and drives being
    // plugged in or removed.
    auto *bookmarksWatcher = new QFileSystemWatcher(this);
    const QString bookmarksFile = Bookmarks::filePath();
    QDir().mkpath(QFileInfo(bookmarksFile).absolutePath());
    bookmarksWatcher->addPath(QFileInfo(bookmarksFile).absolutePath()); // atomic saves replace the file
    connect(bookmarksWatcher, &QFileSystemWatcher::directoryChanged, this,
            [this] { scheduleSidebarRefresh(); updateBookmarkAction(); });
#if defined(Q_OS_LINUX)
    m_mountsFd = ::open("/proc/self/mounts", O_RDONLY | O_CLOEXEC);
    if (m_mountsFd >= 0) {
        auto *mounts = new QSocketNotifier(m_mountsFd, QSocketNotifier::Exception, this);
        connect(mounts, &QSocketNotifier::activated, this, &MainWindow::scheduleSidebarRefresh);
    }
#endif
    if (m_mountsFd < 0) {
        auto *poll = new QTimer(this);
        connect(poll, &QTimer::timeout, this, [this] {
            QStringList now;
            for (const QStorageInfo &vol : QStorageInfo::mountedVolumes())
                now << vol.rootPath();
            if (now != m_lastMounts)
                scheduleSidebarRefresh();
        });
        poll->start(3000);
    }

    if (m_csd) {
        winId(); // create the native window so we can watch its mouse events
        if (QWindow *handle = windowHandle())
            handle->installEventFilter(this);
    }
}

// ---------------------------------------------------------------- setup ---

void MainWindow::setupActions()
{
    m_backAct = new QAction(tr("Back"), this);
    m_backAct->setShortcuts({QKeySequence::Back, QKeySequence(Qt::ALT | Qt::Key_Left)});
    connect(m_backAct, &QAction::triggered, this, &MainWindow::goBack);

    m_forwardAct = new QAction(tr("Forward"), this);
    m_forwardAct->setShortcuts({QKeySequence::Forward, QKeySequence(Qt::ALT | Qt::Key_Right)});
    connect(m_forwardAct, &QAction::triggered, this, &MainWindow::goForward);

    m_upAct = new QAction(tr("Up"), this);
    m_upAct->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Up));
    connect(m_upAct, &QAction::triggered, this, &MainWindow::goUp);

    m_homeAct = new QAction(tr("Home"), this);
    m_homeAct->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Home));
    connect(m_homeAct, &QAction::triggered, this, [this] { navigateTo(QDir::homePath()); });

    m_locationAct = new QAction(tr("Enter Location…"), this);
    m_locationAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_L));
    connect(m_locationAct, &QAction::triggered, this, [this] {
        setSearchActive(false);
        m_pathBar->startEditing();
    });

    m_hiddenAct = new QAction(tr("Show Hidden Files"), this);
    m_hiddenAct->setCheckable(true);
    m_hiddenAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_H));
    connect(m_hiddenAct, &QAction::toggled, this, &MainWindow::setShowHidden);

    m_searchAct = new QAction(tr("Search"), this);
    m_searchAct->setCheckable(true);
    m_searchAct->setShortcut(QKeySequence::Find);
    connect(m_searchAct, &QAction::toggled, this, &MainWindow::setSearchActive);

    m_gridAct = new QAction(tr("Grid"), this);
    m_gridAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_1));
    m_gridAct->setCheckable(true);
    m_gridAct->setChecked(true);
    connect(m_gridAct, &QAction::triggered, this, [this] { setGridMode(true); });
    m_listAct = new QAction(tr("List"), this);
    m_listAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_2));
    m_listAct->setCheckable(true);
    connect(m_listAct, &QAction::triggered, this, [this] { setGridMode(false); });
    auto *viewGroup = new QActionGroup(this);
    viewGroup->addAction(m_gridAct);
    viewGroup->addAction(m_listAct);

    m_sidebarAct = new QAction(tr("Sidebar"), this);
    m_sidebarAct->setShortcut(QKeySequence(Qt::Key_F9));
    m_sidebarAct->setCheckable(true);
    m_sidebarAct->setChecked(true);
    connect(m_sidebarAct, &QAction::toggled, this, [this](bool on) { m_sidebarPane->setVisible(on); });

    // Sorting (also changed by clicking the list view's column headers).
    auto *sortGroup = new QActionGroup(this);
    const char *sortNames[] = {QT_TR_NOOP("By Name"), QT_TR_NOOP("By Size"), QT_TR_NOOP("By Type"),
                               QT_TR_NOOP("By Modification Date")};
    for (int column = 0; column < DirectoryModel::ColumnCount; ++column) {
        auto *act = new QAction(tr(sortNames[column]), this);
        act->setCheckable(true);
        sortGroup->addAction(act);
        connect(act, &QAction::triggered, this,
                [this, column] { sortBy(column, m_model->sortOrder()); });
        m_sortColumnActs.append(act);
    }
    m_sortColumnActs.first()->setChecked(true);
    auto *orderGroup = new QActionGroup(this);
    m_sortAscAct = new QAction(tr("Ascending"), this);
    m_sortDescAct = new QAction(tr("Descending"), this);
    for (QAction *act : {m_sortAscAct, m_sortDescAct}) {
        act->setCheckable(true);
        orderGroup->addAction(act);
    }
    m_sortAscAct->setChecked(true);
    connect(m_sortAscAct, &QAction::triggered, this,
            [this] { sortBy(m_model->sortColumn(), Qt::AscendingOrder); });
    connect(m_sortDescAct, &QAction::triggered, this,
            [this] { sortBy(m_model->sortColumn(), Qt::DescendingOrder); });

#if defined(KAFTILOUS_TERMINAL)
    m_terminalAct = new QAction(tr("Terminal"), this);
    m_terminalAct->setCheckable(true);
    m_terminalAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_QuoteLeft)); // like VS Code
    connect(m_terminalAct, &QAction::toggled, this, [this](bool on) {
        if (on)
            showTerminal(m_current);
        else
            hideTerminal();
    });
    m_openTerminalHereAct = new QAction(tr("Open in Terminal"), this);
    connect(m_openTerminalHereAct, &QAction::triggered, this, [this] {
        showTerminal(m_contextFolder.isEmpty() ? m_current : m_contextFolder);
    });
#endif

    m_newFolderAct = new QAction(tr("New Folder"), this);
    m_newFolderAct->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N));
    connect(m_newFolderAct, &QAction::triggered, this, [this] { createNewItem(true); });

    m_newFileAct = new QAction(tr("New File"), this);
    m_newFileAct->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_N));
    connect(m_newFileAct, &QAction::triggered, this, [this] { createNewItem(false); });

    m_renameAct = new QAction(tr("Rename…"), this);
    m_renameAct->setShortcut(QKeySequence(Qt::Key_F2));
    connect(m_renameAct, &QAction::triggered, this, &MainWindow::renameSelection);

    m_selectAllAct = new QAction(tr("Select All"), this);
    m_selectAllAct->setShortcut(QKeySequence::SelectAll);
    connect(m_selectAllAct, &QAction::triggered, this, [this] { m_view->selectAll(); });

    m_closeAct = new QAction(tr("Close Tab"), this);
    m_closeAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_W)); // the last tab closes the window
    connect(m_closeAct, &QAction::triggered, this, [this] { closeTab(m_activeTab); });

    m_closeWindowAct = new QAction(tr("Close Window"), this);
    m_closeWindowAct->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_W));
    connect(m_closeWindowAct, &QAction::triggered, this, &QWidget::close);

    m_newTabAct = new QAction(tr("New Tab"), this);
    m_newTabAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_T));
    connect(m_newTabAct, &QAction::triggered, this, [this] { openTab(m_current, true); });

    m_reopenTabAct = new QAction(tr("Reopen Closed Tab"), this);
    m_reopenTabAct->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T));
    connect(m_reopenTabAct, &QAction::triggered, this, &MainWindow::reopenClosedTab);

    m_openInNewTabAct = new QAction(tr("Open in New Tab"), this);
    connect(m_openInNewTabAct, &QAction::triggered, this, [this] {
        for (const QModelIndex &index : selectedViewIndexes()) {
            const QModelIndex source = toSource(index);
            if (m_model->isDir(source))
                openTab(m_model->filePath(source), false);
        }
    });

    auto addTabSwitch = [this](const QList<QKeySequence> &keys, int step) {
        auto *act = new QAction(this);
        act->setShortcuts(keys);
        connect(act, &QAction::triggered, this, [this, step] {
            if (m_tabs.size() > 1)
                m_tabBar->setCurrentIndex((m_activeTab + step + int(m_tabs.size())) % int(m_tabs.size()));
        });
        addAction(act);
    };
    addTabSwitch({QKeySequence(Qt::CTRL | Qt::Key_Tab), QKeySequence(Qt::CTRL | Qt::Key_PageDown)}, 1);
    addTabSwitch({QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Backtab),
                  QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab),
                  QKeySequence(Qt::CTRL | Qt::Key_PageUp)},
                 -1);
    for (int n = 1; n <= 9; ++n) {
        auto *act = new QAction(this);
        act->setShortcut(QKeySequence(Qt::ALT | Qt::Key(Qt::Key_0 + n)));
        connect(act, &QAction::triggered, this, [this, n] {
            if (n - 1 < m_tabs.size())
                m_tabBar->setCurrentIndex(n - 1);
        });
        addAction(act);
    }

    m_quitAct = new QAction(tr("Quit"), this);
    m_quitAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Q));
    m_quitAct->setMenuRole(QAction::QuitRole);
    connect(m_quitAct, &QAction::triggered, this, &QApplication::closeAllWindows);

    m_copyLocationAct = new QAction(tr("Copy Location"), this);
    connect(m_copyLocationAct, &QAction::triggered, this, [this] {
        QApplication::clipboard()->setText(QDir::toNativeSeparators(m_current));
        showMessage(tr("Location copied"));
    });

    m_prefsAct = new QAction(tr("Preferences"), this);
    m_prefsAct->setMenuRole(QAction::PreferencesRole);
    m_prefsAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Comma));
    connect(m_prefsAct, &QAction::triggered, this, [this] {
        static QPointer<PreferencesDialog> dialog;
        if (!dialog) {
            dialog = new PreferencesDialog(this);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
        }
        dialog->show();
        dialog->raise();
        dialog->activateWindow();
    });

    m_openAct = new QAction(tr("Open"), this);
    connect(m_openAct, &QAction::triggered, this, &MainWindow::openSelection);

    m_cutAct = new QAction(tr("Cut"), this);
    m_cutAct->setShortcut(QKeySequence::Cut);
    connect(m_cutAct, &QAction::triggered, this, [this] { copySelection(true); });

    m_copyAct = new QAction(tr("Copy"), this);
    m_copyAct->setShortcut(QKeySequence::Copy);
    connect(m_copyAct, &QAction::triggered, this, [this] { copySelection(false); });

    m_pasteAct = new QAction(tr("Paste"), this);
    m_pasteAct->setShortcut(QKeySequence::Paste);
    connect(m_pasteAct, &QAction::triggered, this, &MainWindow::paste);

    m_trashAct = new QAction(tr("Move to Trash"), this);
    m_trashAct->setShortcut(QKeySequence::Delete);
    connect(m_trashAct, &QAction::triggered, this, &MainWindow::trashSelection);

    m_bookmarkAct = new QAction(tr("Bookmark This Folder"), this);
    m_bookmarkAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));
    connect(m_bookmarkAct, &QAction::triggered, this, &MainWindow::toggleBookmark);

    m_deleteAct = new QAction(tr("Delete Permanently…"), this);
    m_deleteAct->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_Delete));
    connect(m_deleteAct, &QAction::triggered, this, &MainWindow::deleteSelection);

    m_restoreAct = new QAction(tr("Restore From Trash"), this);
    connect(m_restoreAct, &QAction::triggered, this, &MainWindow::restoreSelection);

    m_emptyTrashAct = new QAction(tr("Empty Trash…"), this);
    connect(m_emptyTrashAct, &QAction::triggered, this, &MainWindow::emptyTrash);

    m_undoAct = new QAction(tr("Undo"), this);
    m_undoAct->setShortcut(QKeySequence::Undo);
    connect(m_undoAct, &QAction::triggered, m_ops, &FileOperations::undo);

    m_aboutAct = new QAction(tr("About Kaftilous"), this);
    m_aboutAct->setMenuRole(QAction::AboutRole);
    connect(m_aboutAct, &QAction::triggered, this, [this] {
        QMessageBox::about(this, tr("About Kaftilous"),
                           tr("<b>Kaftilous</b> %1<br>A fast, cross-platform file manager.")
                               .arg(QApplication::applicationVersion()));
    });

    addActions({m_backAct, m_forwardAct, m_upAct, m_homeAct, m_locationAct, m_hiddenAct,
                m_searchAct, m_gridAct, m_listAct, m_sidebarAct, m_prefsAct, m_cutAct, m_copyAct,
                m_pasteAct, m_trashAct, m_undoAct, m_newFolderAct, m_newFileAct, m_renameAct,
                m_selectAllAct, m_closeAct, m_quitAct, m_deleteAct, m_bookmarkAct,
                m_newTabAct, m_closeWindowAct, m_reopenTabAct});
    if (m_terminalAct)
        addAction(m_terminalAct);
}

QToolButton *MainWindow::makeButton(const QString &toolTip, const char *objectName)
{
    auto *button = new QToolButton(this);
    button->setObjectName(objectName);
    button->setToolTip(toolTip);
    button->setIconSize(QSize(16, 16));
    button->setFocusPolicy(Qt::NoFocus);
    if (qstrcmp(objectName, "win") == 0)
        button->setFixedSize(24, 24);
    else
        button->setFixedSize(32, 32);
    return button;
}

// Rounded popup menu (needs a translucent, frameless popup window).
QMenu *MainWindow::makeMenu()
{
    auto *menu = new QMenu(this);
    menu->setWindowFlags(menu->windowFlags() | Qt::FramelessWindowHint
                         | Qt::NoDropShadowWindowHint);
    menu->setAttribute(Qt::WA_TranslucentBackground);
    return menu;
}

void MainWindow::setupUi()
{
    m_model = new DirectoryModel(this);
    m_proxy = new FileFilterProxy(this);
    m_proxy->setSourceModel(m_model);
    updateFilter();
    connect(m_model, &DirectoryModel::loaded, this, [this] {
        updateFloatingBar();
        applyPendingSelection();
        if (m_pendingScroll >= 0) { // a tab coming back: where it was
            m_view->verticalScrollBar()->setValue(m_pendingScroll);
            m_pendingScroll = -1;
        }
    });
    // The open folder was deleted or moved: go to the nearest folder left.
    connect(m_model, &DirectoryModel::folderGone, this, [this](const QString &gone) {
        QString path = gone;
        while (!QFileInfo(path).isDir()) {
            const QString parent = QFileInfo(path).absolutePath();
            if (parent == path)
                break;
            path = parent;
        }
        navigateTo(path);
        showMessage(tr("“%1” no longer exists").arg(QFileInfo(gone).fileName()));
    });
    // New files may reach the view a little after a paste finishes.
    connect(m_proxy, &QAbstractItemModel::rowsInserted, this,
            &MainWindow::applyPendingSelection);

    m_thumbnailer = new Thumbnailer(this);

    m_root = new QWidget(this);
    m_root->setObjectName("root");
    m_root->setAttribute(Qt::WA_StyledBackground);
    auto *rootLayout = new QVBoxLayout(m_root);
    rootLayout->setSpacing(0);

#if defined(Q_OS_MACOS)
    setMenuBar(qobject_cast<QMenuBar *>(buildMenuBar())); // global macOS menu bar
#else
    rootLayout->addWidget(buildMenuBar(), 0);
#endif

    m_splitter = new QSplitter(m_root);
    m_splitter->setObjectName("mainSplitter");
    m_splitter->addWidget(buildSidebarPane());
    m_splitter->addWidget(buildContentPane());
    m_splitter->setChildrenCollapsible(false);
    m_splitter->setHandleWidth(1);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setSizes({230, 920});
    rootLayout->addWidget(m_splitter, 1);

    setCentralWidget(m_root);
}

QWidget *MainWindow::buildSidebarPane()
{
    auto *pane = new QWidget(this);
    m_sidebarPane = pane;
    pane->setObjectName("sidebarPane");
    pane->setAttribute(Qt::WA_StyledBackground);
    pane->setMinimumWidth(170);
    auto *layout = new QVBoxLayout(pane);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *header = new HeaderBar(pane);
    m_searchButton = makeButton(tr("Search (Ctrl+F)"));
    m_searchButton->setCheckable(true);
    connect(m_searchButton, &QToolButton::toggled, m_searchAct, &QAction::setChecked);
    connect(m_searchAct, &QAction::toggled, m_searchButton, &QToolButton::setChecked);

    m_titleLabel = new QLabel(tr("Kaftilous"), header);
    m_titleLabel->setObjectName("title");
    m_titleLabel->setAlignment(Qt::AlignCenter);

    m_menuButton = makeButton(tr("Main menu"));
    m_menuButton->setPopupMode(QToolButton::InstantPopup);
    QMenu *mainMenu = makeMenu();
    mainMenu->addAction(m_undoAct);
    mainMenu->addSeparator();
    mainMenu->addAction(m_hiddenAct);
    mainMenu->addSeparator();
    mainMenu->addAction(m_prefsAct);
    mainMenu->addAction(m_aboutAct);
    m_menuButton->setMenu(mainMenu);

    header->row()->addWidget(m_searchButton);
    header->row()->addWidget(m_titleLabel, 1);
    header->row()->addWidget(m_menuButton);
    layout->addWidget(header);

    m_sidebar = new QListWidget(pane);
    m_sidebar->setObjectName("sidebar");
    m_sidebar->setItemDelegate(new SidebarDelegate(m_sidebar));
    m_sidebar->setIconSize(QSize(16, 16));
    m_sidebar->setFrameShape(QFrame::NoFrame);
    m_sidebar->setFocusPolicy(Qt::NoFocus);
    m_sidebar->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_sidebar->setTextElideMode(Qt::ElideRight);
    m_sidebar->viewport()->setAcceptDrops(true); // drop onto places and drives
    m_sidebar->viewport()->installEventFilter(this);
    m_sidebar->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_sidebar, &QWidget::customContextMenuRequested, this, &MainWindow::showSidebarMenu);
    connect(m_sidebar, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        const QString path = item->data(PathRole).toString();
        if (path.isEmpty() || !navigateTo(path))
            syncSidebarSelection();
    });
    layout->addWidget(m_sidebar, 1);
    return pane;
}

QWidget *MainWindow::buildContentPane()
{
    auto *pane = new QWidget(this);
    pane->setObjectName("contentPane");
    pane->setAttribute(Qt::WA_StyledBackground);
    auto *layout = new QVBoxLayout(pane);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *header = new HeaderBar(pane);
    QHBoxLayout *row = header->row();

    m_backButton = makeButton(tr("Back (Alt+Left)"));
    connect(m_backButton, &QToolButton::clicked, m_backAct, &QAction::trigger);
    m_forwardButton = makeButton(tr("Forward (Alt+Right)"));
    connect(m_forwardButton, &QToolButton::clicked, m_forwardAct, &QAction::trigger);
    row->addWidget(m_backButton);
    row->addWidget(m_forwardButton);
    row->addSpacing(4);

    m_pathBar = new PathBar(header);
    connect(m_pathBar, &PathBar::pathActivated, this, [this](const QString &p) { navigateTo(p); });
    QMenu *folderMenu = makeMenu();
    folderMenu->addAction(m_pasteAct);
    folderMenu->addSeparator();
    folderMenu->addAction(m_bookmarkAct);
    folderMenu->addAction(m_copyLocationAct);
    folderMenu->addSeparator();
    folderMenu->addAction(m_hiddenAct);
    m_pathBar->menuButton()->setMenu(folderMenu);

    m_searchEdit = new QLineEdit(header);
    m_searchEdit->setObjectName("searchEdit");
    m_searchEdit->setFixedHeight(32);
    m_searchEdit->setPlaceholderText(tr("Search current folder"));
    m_searchEdit->installEventFilter(this);
    connect(m_searchEdit, &QLineEdit::textChanged, this, &MainWindow::updateFilter);

    m_locationStack = new QStackedWidget(header);
    m_locationStack->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_locationStack->setFixedHeight(32);
    m_locationStack->addWidget(m_pathBar);
    m_locationStack->addWidget(m_searchEdit);
    row->addWidget(m_locationStack, 1);
    row->addSpacing(4);

    if (m_terminalAct) {
        m_terminalButton = makeButton(tr("Terminal (Ctrl+`)"));
        m_terminalButton->setCheckable(true);
        connect(m_terminalButton, &QToolButton::toggled, m_terminalAct, &QAction::setChecked);
        connect(m_terminalAct, &QAction::toggled, m_terminalButton, &QToolButton::setChecked);
        row->addWidget(m_terminalButton);
    }
    m_viewButton = makeButton(QString());
    connect(m_viewButton, &QToolButton::clicked, this, [this] { setGridMode(!m_grid); });
    row->addWidget(m_viewButton);

    if (m_csd) {
        row->addSpacing(10);
        m_minButton = makeButton(tr("Minimize"), "win");
        connect(m_minButton, &QToolButton::clicked, this, &QWidget::showMinimized);
        m_maxButton = makeButton(tr("Maximize"), "win");
        connect(m_maxButton, &QToolButton::clicked, this,
                [this] { isMaximized() ? showNormal() : showMaximized(); });
        m_closeButton = makeButton(tr("Close"), "win");
        connect(m_closeButton, &QToolButton::clicked, this, &QWidget::close);
        for (QToolButton *b : {m_minButton, m_maxButton, m_closeButton}) {
            row->addWidget(b);
            row->addSpacing(6);
        }
        row->addSpacing(-6);
    }
    layout->addWidget(header);

    m_gridDelegate = new FileItemDelegate(m_thumbnailer, this);
    m_listDelegate = new FileItemDelegate(m_thumbnailer, this);
    m_listDelegate->setGridMode(false);
    m_viewStack = new QStackedWidget(pane);
    m_viewStack->setObjectName("viewStack");
    m_viewStack->installEventFilter(this); // resize -> reposition overlays
    m_dragScrollTimer = new QTimer(this);
    m_dragScrollTimer->setInterval(40);
    connect(m_dragScrollTimer, &QTimer::timeout, this, [this] {
        if (m_dragScrollStep == 0) {
            m_dragScrollTimer->stop();
            return;
        }
        QScrollBar *bar = m_view->verticalScrollBar();
        bar->setValue(bar->value() + m_dragScrollStep);
    });

    // Grid.
    m_iconView = new QListView(m_viewStack);
    m_iconView->setObjectName("files");
    m_iconView->setViewMode(QListView::IconMode);
    m_iconView->setMovement(QListView::Static);
    m_iconView->setResizeMode(QListView::Adjust);
    m_iconView->setUniformItemSizes(true);
    m_iconView->setWordWrap(true);
    m_iconView->setSpacing(0);
    setupFileView(m_iconView);
    m_iconView->setItemDelegate(m_gridDelegate);
    m_iconView->setGridSize(m_gridDelegate->gridCellSize(m_iconView->fontMetrics()));

    // List with columns. Both views share the model and the selection.
    m_detailView = new QTreeView(m_viewStack);
    m_detailView->setObjectName("detailView");
    m_detailView->setRootIsDecorated(false);
    m_detailView->setItemsExpandable(false);
    m_detailView->setIndentation(0);
    m_detailView->setUniformRowHeights(true);
    m_detailView->setAllColumnsShowFocus(true);
    m_detailView->setSelectionBehavior(QAbstractItemView::SelectRows);
    setupFileView(m_detailView);
    m_detailView->setItemDelegate(m_listDelegate);
    QItemSelectionModel *unused = m_detailView->selectionModel();
    m_detailView->setSelectionModel(m_iconView->selectionModel());
    delete unused;
    QHeaderView *columns = m_detailView->header();
    columns->setStretchLastSection(false);
    columns->setHighlightSections(false);
    columns->setSectionsMovable(false);
    columns->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    columns->setSectionResizeMode(DirectoryModel::NameColumn, QHeaderView::Stretch);
    columns->resizeSection(DirectoryModel::SizeColumn, 110);
    columns->resizeSection(DirectoryModel::TypeColumn, 170);
    columns->resizeSection(DirectoryModel::ModifiedColumn, 130);
    // Header clicks sort the model (FileFilterProxy forwards sort()).
    columns->setSortIndicator(DirectoryModel::NameColumn, Qt::AscendingOrder);
    m_detailView->setSortingEnabled(true);
    connect(columns, &QHeaderView::sortIndicatorChanged, this, &MainWindow::updateSortActions);

    m_viewStack->addWidget(m_iconView);
    m_viewStack->addWidget(m_detailView);
    m_view = m_iconView;

    QItemSelectionModel *selection = m_iconView->selectionModel();
    connect(selection, &QItemSelectionModel::selectionChanged, this,
            &MainWindow::updateFloatingBar);
    connect(selection, &QItemSelectionModel::selectionChanged, this,
            &MainWindow::updateEditActions);
    connect(m_proxy, &QAbstractItemModel::layoutChanged, this, &MainWindow::updateRenameGeometry);
    // Shown only in the Trash: what it is, plus Restore / Empty Trash.
    m_trashBar = new QWidget(pane);
    m_trashBar->setObjectName("trashBar");
    m_trashBar->setAttribute(Qt::WA_StyledBackground);
    auto *trashRow = new QHBoxLayout(m_trashBar);
    trashRow->setContentsMargins(16, 6, 10, 6);
    auto *trashText = new QLabel(tr("Items in the Trash are deleted forever when you empty it."),
                                 m_trashBar);
    trashText->setObjectName("trashBarText");
    m_trashRestoreButton = new QToolButton(m_trashBar);
    m_trashRestoreButton->setObjectName("barButton");
    m_trashRestoreButton->setDefaultAction(m_restoreAct);
    m_trashRestoreButton->setText(tr("Restore"));
    m_trashEmptyButton = new QToolButton(m_trashBar);
    m_trashEmptyButton->setObjectName("barButton");
    m_trashEmptyButton->setDefaultAction(m_emptyTrashAct);
    for (QToolButton *b : {m_trashRestoreButton, m_trashEmptyButton})
        b->setFocusPolicy(Qt::NoFocus);
    trashRow->addWidget(trashText, 1);
    trashRow->addWidget(m_trashRestoreButton);
    trashRow->addWidget(m_trashEmptyButton);
    m_trashBar->hide();
    m_tabBar = new QTabBar(pane);
    m_tabBar->setObjectName("tabs");
    m_tabBar->setExpanding(true);
    m_tabBar->setMovable(true);
    m_tabBar->setDocumentMode(true);
    m_tabBar->setElideMode(Qt::ElideMiddle);
    m_tabBar->setFocusPolicy(Qt::NoFocus);
    m_tabBar->setAcceptDrops(true); // drop files onto a tab's folder
    m_tabBar->installEventFilter(this);
    m_tabs.append(TabState());
    m_tabBar->addTab(QString());
    addTabCloseButton(0);
    m_tabBar->hide(); // shown from two tabs on
    connect(m_tabBar, &QTabBar::currentChanged, this, &MainWindow::switchToTab);
    connect(m_tabBar, &QTabBar::tabMoved, this, [this](int from, int to) {
        m_tabs.move(from, to);
        if (m_activeTab == from)
            m_activeTab = to;
        else if (from < m_activeTab && to >= m_activeTab)
            --m_activeTab;
        else if (from > m_activeTab && to <= m_activeTab)
            ++m_activeTab;
    });
    layout->addWidget(m_tabBar);
    layout->addWidget(m_trashBar);

    m_contentSplitter = new QSplitter(Qt::Vertical, pane);
    m_contentSplitter->setObjectName("contentSplitter");
    m_contentSplitter->setChildrenCollapsible(false);
    m_contentSplitter->setHandleWidth(1);
    m_contentSplitter->addWidget(m_viewStack);
#if defined(KAFTILOUS_TERMINAL)
    m_contentSplitter->addWidget(buildTerminalPanel());
#endif
    m_contentSplitter->setStretchFactor(0, 1);
    layout->addWidget(m_contentSplitter, 1);

    m_floatingBar = new QLabel(m_viewStack);
    m_floatingBar->setObjectName("floatingBar");
    m_floatingBar->hide();
    m_messageTimer = new QTimer(this);
    m_messageTimer->setSingleShot(true);
    m_messageTimer->setInterval(3000);
    connect(m_messageTimer, &QTimer::timeout, this, [this] {
        m_message.clear();
        updateFloatingBar();
    });

    m_opsPanel = new OperationsPanel(m_ops, m_viewStack);
    m_toast = new Toast(m_viewStack);
    connect(m_opsPanel, &OperationsPanel::layoutChanged, this, [this] {
        m_toast->setAvoidRect(m_opsPanel->isVisible() ? m_opsPanel->geometry() : QRect());
    });
    updateEditActions();

    setGridMode(true);
    return pane;
}

// ---------------------------------------------------------------- theme ---

void MainWindow::applyTheme(bool force)
{
    // The applied theme (built from the application palette for "Follow
    // System"; never from widget palettes, which our style sheet changes).
    const Theme theme = currentTheme();
    if (!force && m_themeId == theme.id)
        return;
    m_theme = theme;
    m_themeId = theme.id;

    setStyleSheet(styleSheetFor(m_theme));

    m_backButton->setIcon(glyphIcon(Glyph::ChevronLeft, m_theme.fg));
    m_forwardButton->setIcon(glyphIcon(Glyph::ChevronRight, m_theme.fg));
    m_searchButton->setIcon(glyphIcon(Glyph::Search, m_theme.fg));
    m_menuButton->setIcon(glyphIcon(Glyph::Menu, m_theme.fg));
    m_viewButton->setIcon(glyphIcon(m_grid ? Glyph::List : Glyph::Grid, m_theme.fg));
    if (m_csd) {
        m_minButton->setIcon(glyphIcon(Glyph::Minimize, m_theme.fg));
        m_closeButton->setIcon(glyphIcon(Glyph::Close, m_theme.fg));
        updateWindowState();
    }

    static_cast<SidebarDelegate *>(m_sidebar->itemDelegate())->lineColor = m_theme.border;
    static_cast<SidebarDelegate *>(m_sidebar->itemDelegate())->ejectIcon =
        glyphIcon(Glyph::Eject, m_theme.fg);
    m_pathBar->setTheme(m_theme);
    m_gridDelegate->setTheme(m_theme);
    m_listDelegate->setTheme(m_theme);
    m_opsPanel->setTheme(m_theme);
    for (int i = 0; m_tabBar && i < m_tabBar->count(); ++i) {
        if (auto *close = qobject_cast<QToolButton *>(m_tabBar->tabButton(i, QTabBar::RightSide)))
            close->setIcon(glyphIcon(Glyph::Close, m_theme.dimFg));
    }
#if defined(KAFTILOUS_TERMINAL)
    m_terminalButton->setIcon(glyphIcon(Glyph::Terminal, m_theme.fg));
    m_terminalCloseButton->setIcon(glyphIcon(Glyph::Close, m_theme.fg));
    m_terminal->setTheme(m_theme);
#endif
    m_toast->setTheme(m_theme);
    m_view->viewport()->update();

    populateSidebar();
    syncSidebarSelection();
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    // Widgets with a style sheet get PaletteChange rather than
    // ApplicationPaletteChange. applyTheme() reads the application palette
    // and returns early if light/dark didn't change, so this can't loop.
    if (event->type() == QEvent::ApplicationPaletteChange
        || event->type() == QEvent::PaletteChange)
        applyTheme();
    else if (event->type() == QEvent::WindowStateChange)
        updateWindowState();
}

// -------------------------------------------------------------- sidebar ---

void MainWindow::populateSidebar()
{
    const int scroll = m_sidebar->verticalScrollBar()->value();
    m_sidebar->clear();
    const QColor iconColor = m_theme.fg;

    auto addItem = [&](const QString &name, const QString &path, Glyph glyph, SidebarKind kind) {
        auto *item = new QListWidgetItem(glyphIcon(glyph, iconColor), name, m_sidebar);
        item->setData(PathRole, path);
        item->setData(KindRole, int(kind));
        item->setToolTip(QDir::toNativeSeparators(path));
        return item;
    };
    auto addSeparator = [&] {
        auto *sep = new QListWidgetItem(m_sidebar);
        sep->setFlags(Qt::NoItemFlags);
        sep->setData(SeparatorRole, true);
    };

    const struct {
        QStandardPaths::StandardLocation loc;
        const char *name;
        Glyph glyph;
    } places[] = {
        {QStandardPaths::HomeLocation, QT_TR_NOOP("Home"), Glyph::Home},
        {QStandardPaths::DesktopLocation, QT_TR_NOOP("Desktop"), Glyph::Desktop},
        {QStandardPaths::DocumentsLocation, QT_TR_NOOP("Documents"), Glyph::Documents},
        {QStandardPaths::DownloadLocation, QT_TR_NOOP("Downloads"), Glyph::Downloads},
        {QStandardPaths::MusicLocation, QT_TR_NOOP("Music"), Glyph::Music},
        {QStandardPaths::PicturesLocation, QT_TR_NOOP("Pictures"), Glyph::Pictures},
        {QStandardPaths::MoviesLocation, QT_TR_NOOP("Videos"), Glyph::Videos},
    };
    QStringList shown;
    for (const auto &p : places) {
        const QString path = QDir::cleanPath(QStandardPaths::writableLocation(p.loc));
        if (path.isEmpty() || shown.contains(path) || !QFileInfo(path).isDir())
            continue;
        shown << path;
        addItem(tr(p.name), path, p.glyph, PlaceItem);
    }

    if (Trash::isSupported()) {
        // Shows every trash folder merged (home and removable drives).
        const QString trash = Trash::homeFilesDir();
        QDir().mkpath(trash);
        QDir().mkpath(QFileInfo(trash).absolutePath() + QStringLiteral("/info"));
        addItem(tr("Trash"), trash, Glyph::Trash, PlaceItem);
    }

    // Bookmarks (shared with Nautilus). Ones that duplicate a place above,
    // or whose folder is gone, aren't shown (but stay in the file).
    bool bookmarkSection = false;
    for (const Bookmarks::Bookmark &b : Bookmarks::load()) {
        if (shown.contains(b.path) || !QFileInfo(b.path).isDir())
            continue;
        if (!bookmarkSection) {
            addSeparator();
            bookmarkSection = true;
        }
        shown << b.path;
        addItem(b.displayName(), b.path, Glyph::Folder, BookmarkItem);
    }

    addSeparator();

    m_lastMounts.clear();
    for (const QStorageInfo &vol : QStorageInfo::mountedVolumes()) {
        if (!vol.isValid() || !vol.isReady())
            continue;
        const QString root = vol.rootPath();
        m_lastMounts << root;
#if defined(Q_OS_LINUX)
        // Skip system/snap mounts; keep "/" and user-visible drives.
        if (root != "/" && !root.startsWith("/media/") && !root.startsWith("/mnt/")
            && !root.startsWith("/run/media/"))
            continue;
#elif defined(Q_OS_MACOS)
        if (root.startsWith("/System/Volumes"))
            continue;
#endif
        const bool system = root == QDir::rootPath();
        QListWidgetItem *item = addItem(system ? tr("Computer") : vol.displayName(), root,
                                        system ? Glyph::Drive : Glyph::Removable, DriveItem);
#if defined(Q_OS_LINUX)
        // Drives mounted by the desktop (udisks) can be ejected.
        if (root.startsWith("/media/") || root.startsWith("/run/media/")) {
            item->setData(EjectRole, true);
            item->setToolTip(QDir::toNativeSeparators(root) + QLatin1Char('\n') + tr("Eject: click ⏏"));
        }
#else
        Q_UNUSED(item)
#endif
    }
    m_sidebar->verticalScrollBar()->setValue(scroll);
}

void MainWindow::scheduleSidebarRefresh()
{
    m_sidebarRefresh->start();
}

void MainWindow::showSidebarMenu(const QPoint &pos)
{
    QListWidgetItem *item = m_sidebar->itemAt(pos);
    if (!item || item->data(SeparatorRole).toBool())
        return;
    const QString path = item->data(PathRole).toString();
    const int kind = item->data(KindRole).toInt();

    QMenu *menu = makeMenu();
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->addAction(tr("Open"), this, [this, path] { navigateTo(path); });
    if (m_openTerminalHereAct) {
        menu->addAction(tr("Open in Terminal"), this, [this, path] {
            m_contextFolder = path;
            m_openTerminalHereAct->trigger();
        });
    }
    if (kind == BookmarkItem) {
        menu->addSeparator();
        menu->addAction(tr("Rename…"), this, [this, path, item] {
            bool ok = false;
            const QString label = QInputDialog::getText(this, tr("Rename Bookmark"), tr("Name"),
                                                        QLineEdit::Normal, item->text(), &ok);
            if (ok)
                Bookmarks::rename(path, label == QFileInfo(path).fileName() ? QString() : label);
            scheduleSidebarRefresh();
        });
        menu->addAction(tr("Remove from Bookmarks"), this, [this, path] {
            Bookmarks::remove(path);
            scheduleSidebarRefresh();
            updateBookmarkAction();
        });
    } else if (kind == DriveItem && item->data(EjectRole).toBool()) {
        menu->addSeparator();
        const QString name = item->text();
        menu->addAction(glyphIcon(Glyph::Eject, m_theme.fg), tr("Eject"), this,
                        [this, path, name] { ejectDrive(path, name); });
    }
    menu->popup(m_sidebar->viewport()->mapToGlobal(pos));
}

// Ctrl+D: add the open folder to the bookmarks, or remove it.
void MainWindow::toggleBookmark()
{
    if (m_current.isEmpty() || m_model->isTrash())
        return;
    if (Bookmarks::contains(m_current)) {
        Bookmarks::remove(m_current);
        showMessage(tr("Removed from bookmarks"));
    } else if (Bookmarks::add(m_current)) {
        showMessage(tr("“%1” added to bookmarks").arg(QFileInfo(m_current).fileName()));
    }
    scheduleSidebarRefresh();
    updateBookmarkAction();
}

void MainWindow::updateBookmarkAction()
{
    const bool marked = Bookmarks::contains(m_current);
    m_bookmarkAct->setText(marked ? tr("Remove Bookmark") : tr("Bookmark This Folder"));
    m_bookmarkAct->setEnabled(!m_current.isEmpty() && !m_model->isTrash());
}

// Unmount and power off a USB drive (GNOME's own tool does it safely).
void MainWindow::ejectDrive(const QString &root, const QString &name)
{
    for (FileJob *job : m_ops->activeJobs()) {
        for (const Transfer &t : job->transfers()) {
            if (t.source.startsWith(root + QLatin1Char('/')) || t.targetDir == root
                || t.targetDir.startsWith(root + QLatin1Char('/'))) {
                showMessage(tr("Wait until copying to or from “%1” is finished").arg(name));
                return;
            }
        }
    }
    if (m_current == root || m_current.startsWith(root + QLatin1Char('/')))
        navigateTo(QDir::homePath()); // don't keep the drive busy ourselves

    const QString program = qEnvironmentVariable("KAFTILOUS_EJECT_PROGRAM", QStringLiteral("gio"));
    auto *process = new QProcess(this);
    connect(process, &QProcess::finished, this, [this, process, name](int code, QProcess::ExitStatus status) {
        if (status == QProcess::NormalExit && code == 0) {
            showMessage(tr("“%1” can be safely removed").arg(name));
        } else {
            QString why = QString::fromLocal8Bit(process->readAllStandardError()).trimmed();
            why = why.section(QLatin1Char('\n'), 0, 0);
            if (why.contains(QLatin1String("busy"), Qt::CaseInsensitive))
                why = tr("it's in use by another program or the terminal");
            showMessage(why.isEmpty() ? tr("Couldn't eject “%1”").arg(name)
                                      : tr("Couldn't eject “%1”: %2").arg(name, why));
        }
        scheduleSidebarRefresh();
        process->deleteLater();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process, name](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            showMessage(tr("Couldn't eject “%1”: the gio tool isn't installed").arg(name));
            process->deleteLater();
        }
    });
    showMessage(tr("Ejecting “%1”…").arg(name));
    process->start(program, {QStringLiteral("mount"), QStringLiteral("--eject"), root});
}

// ------------------------------------------------------------ settings ---

void MainWindow::loadSettings()
{
    QSettings settings;
    if (settings.contains("window/geometry"))
        restoreGeometry(settings.value("window/geometry").toByteArray());
    if (settings.contains("window/sidebarSplitter"))
        m_splitter->restoreState(settings.value("window/sidebarSplitter").toByteArray());
    m_terminalHeight = settings.value("window/terminalHeight", 0).toInt();
    if (settings.contains("view/listHeader"))
        m_detailView->header()->restoreState(settings.value("view/listHeader").toByteArray());
    sortBy(settings.value("view/sortColumn", DirectoryModel::NameColumn).toInt(),
           Qt::SortOrder(settings.value("view/sortOrder", int(Qt::AscendingOrder)).toInt()));
    m_hiddenAct->setChecked(settings.value("view/showHidden", false).toBool());
    m_sidebarAct->setChecked(settings.value("view/sidebar", true).toBool());
    setGridMode(settings.value("view/grid", true).toBool());
}

void MainWindow::saveSettings()
{
    QSettings settings;
    settings.setValue("window/geometry", saveGeometry());
    settings.setValue("window/sidebarSplitter", m_splitter->saveState());
    if (m_terminalPanel && m_terminalPanel->isVisible())
        m_terminalHeight = m_terminalPanel->height();
    settings.setValue("window/terminalHeight", m_terminalHeight);
    settings.setValue("view/listHeader", m_detailView->header()->saveState());
    settings.setValue("view/sortColumn", m_model->sortColumn());
    settings.setValue("view/sortOrder", int(m_model->sortOrder()));
    settings.setValue("view/showHidden", m_showHidden);
    settings.setValue("view/sidebar", m_sidebarAct->isChecked());
    settings.setValue("view/grid", m_grid);
}

// Highlight the sidebar entry matching the current folder, like Nautilus.
void MainWindow::syncSidebarSelection()
{
    for (int i = 0; i < m_sidebar->count(); ++i) {
        QListWidgetItem *item = m_sidebar->item(i);
        const QString path = item->data(PathRole).toString();
        if (!path.isEmpty() && QDir::cleanPath(path) == m_current) {
            m_sidebar->setCurrentItem(item);
            return;
        }
    }
    m_sidebar->clearSelection();
    m_sidebar->setCurrentRow(-1);
}

// ----------------------------------------------------------- navigation ---

bool MainWindow::navigateTo(const QString &path, bool addToHistory)
{
    const QFileInfo info(path);
    if (!info.exists() || !info.isDir()) {
        showMessage(tr("Folder not found: %1").arg(path));
        return false;
    }
    if (!info.isReadable() || !info.isExecutable()) {
        showMessage(tr("You don't have permission to open “%1”").arg(info.fileName()));
        return false;
    }

    const QString clean = QDir::cleanPath(info.absoluteFilePath());
    if (clean == m_current)
        return true;

    setSearchActive(false);
    cancelRename();
    m_pendingSelection.clear();
    m_pendingRename.clear();
    if (addToHistory && !m_current.isEmpty()) {
        m_backHistory.push(m_current);
        m_forwardHistory.clear();
    }
    m_current = clean;

    m_model->setPath(clean);
    m_trashBar->setVisible(m_model->isTrash());
    m_view->scrollToTop();
    m_pathBar->setPath(clean);

    const QString title = m_model->isTrash()        ? tr("Trash")
                          : info.fileName().isEmpty() ? QDir::toNativeSeparators(clean)
                                                      : info.fileName();
    setWindowTitle(tr("%1 — Kaftilous").arg(title));
    if (m_tabBar && m_activeTab < m_tabBar->count()) {
        m_tabBar->setTabText(m_activeTab, title);
        m_tabBar->setTabToolTip(m_activeTab, QDir::toNativeSeparators(clean));
        m_tabs[m_activeTab].path = clean;
    }

    syncSidebarSelection();
    updateActions();
    updateEditActions();
    updateBookmarkAction();
    updateFloatingBar();
    return true;
}

void MainWindow::goBack()
{
    if (m_backHistory.isEmpty())
        return;
    const QString previous = m_current;
    if (navigateTo(m_backHistory.pop(), false))
        m_forwardHistory.push(previous);
    updateActions();
}

void MainWindow::goForward()
{
    if (m_forwardHistory.isEmpty())
        return;
    const QString previous = m_current;
    if (navigateTo(m_forwardHistory.pop(), false))
        m_backHistory.push(previous);
    updateActions();
}

void MainWindow::goUp()
{
    QDir dir(m_current);
    if (dir.cdUp())
        navigateTo(dir.absolutePath());
}

void MainWindow::openIndex(const QModelIndex &viewIndex)
{
    const QModelIndex index = toSource(viewIndex);
    const QString path = m_model->filePath(index);
    if (m_model->isDir(index))
        navigateTo(path);
    else
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void MainWindow::updateActions()
{
    m_backAct->setEnabled(!m_backHistory.isEmpty());
    m_forwardAct->setEnabled(!m_forwardHistory.isEmpty());
    m_upAct->setEnabled(!m_current.isEmpty() && !QDir(m_current).isRoot());
    m_backButton->setEnabled(m_backAct->isEnabled());
    m_forwardButton->setEnabled(m_forwardAct->isEnabled());
}

// ----------------------------------------------------------- view modes ---

void MainWindow::setGridMode(bool grid)
{
    cancelRename();
    m_grid = grid;
    QAbstractItemView *next = grid ? static_cast<QAbstractItemView *>(m_iconView) : m_detailView;
    const bool hadFocus = m_view && m_view->hasFocus();
    const QModelIndex current = m_view ? m_view->currentIndex() : QModelIndex();
    if (!grid) {
        // The grid selects only the first column; the table selects rows.
        QItemSelection rows;
        for (const QModelIndex &index : selectedViewIndexes())
            rows.select(index, index.siblingAtColumn(DirectoryModel::ColumnCount - 1));
        m_applyingSelection = true;
        m_view->selectionModel()->select(rows, QItemSelectionModel::ClearAndSelect);
        m_applyingSelection = false;
    }
    m_viewStack->setCurrentWidget(next);
    m_view = next;
    if (current.isValid())
        m_view->scrollTo(current);
    if (hadFocus)
        m_view->setFocus();
    m_viewButton->setIcon(glyphIcon(grid ? Glyph::List : Glyph::Grid, m_theme.fg));
    if (m_gridAct)
        (grid ? m_gridAct : m_listAct)->setChecked(true);
    updateRenameGeometry();
    m_viewButton->setToolTip(grid ? tr("List view (Ctrl+2)") : tr("Grid view (Ctrl+1)"));
}

void MainWindow::setShowHidden(bool show)
{
    m_showHidden = show;
    updateFilter();
    updateFloatingBar();
}

void MainWindow::setSearchActive(bool active)
{
    if (!m_locationStack)
        return;
    if (m_searchAct->isChecked() != active)
        m_searchAct->setChecked(active); // re-enters with the same value
    if (active) {
        m_locationStack->setCurrentWidget(m_searchEdit);
        m_searchEdit->setFocus(Qt::ShortcutFocusReason);
        m_searchEdit->selectAll();
    } else if (m_locationStack->currentWidget() != m_pathBar) {
        m_searchEdit->clear();
        m_locationStack->setCurrentWidget(m_pathBar);
        m_view->setFocus();
    }
}

// Search = filter the current folder by name (case-insensitive substring),
// done by FileFilterProxy. The model filter only handles hidden files.
void MainWindow::updateFilter()
{
    m_model->setShowHidden(m_showHidden);
    m_proxy->setQuery(m_searchEdit ? m_searchEdit->text().trimmed() : QString());
}

QModelIndex MainWindow::toSource(const QModelIndex &viewIndex) const
{
    return viewIndex.model() == m_proxy ? m_proxy->mapToSource(viewIndex) : viewIndex;
}

QModelIndex MainWindow::toView(const QModelIndex &sourceIndex) const
{
    return m_proxy->mapFromSource(sourceIndex);
}

// -------------------------------------------------------- floating bar ---

void MainWindow::showMessage(const QString &text)
{
    m_message = text;
    m_messageTimer->start();
    updateFloatingBar();
}

void MainWindow::updateFloatingBar()
{
    QString text = m_message;
    if (text.isEmpty()) {
        const QModelIndexList selected = selectedViewIndexes();
        if (selected.size() == 1) {
            const QModelIndex idx = toSource(selected.first());
            text = m_model->isDir(idx)
                       ? tr("“%1” selected").arg(m_model->fileName(idx))
                       : tr("“%1” selected (%2)")
                             .arg(m_model->fileName(idx),
                                  QLocale().formattedDataSize(m_model->size(idx)));
        } else if (selected.size() > 1) {
            qint64 bytes = 0;
            for (const QModelIndex &viewIdx : selected) {
                const QModelIndex idx = toSource(viewIdx);
                if (!m_model->isDir(idx))
                    bytes += m_model->size(idx);
            }
            text = tr("%n items selected", nullptr, int(selected.size()));
            if (bytes > 0)
                text += QStringLiteral(" (%1)").arg(QLocale().formattedDataSize(bytes));
        }
    }

    if (text.isEmpty()) {
        m_floatingBar->hide();
        return;
    }
    m_floatingBar->setText(text);
    m_floatingBar->adjustSize();
    positionFloatingBar();
    m_floatingBar->show();
    m_floatingBar->raise();
}

void MainWindow::positionFloatingBar()
{
    const int margin = 10;
    const int maxWidth = qMax(100, m_viewStack->width() - 2 * margin);
    if (m_floatingBar->width() > maxWidth)
        m_floatingBar->resize(maxWidth, m_floatingBar->height());
    m_floatingBar->move(m_viewStack->width() - m_floatingBar->width() - margin,
                        m_viewStack->height() - m_floatingBar->height() - margin);
}

// ------------------------------------------------- window / decorations ---

void MainWindow::updateWindowState()
{
    const bool framed = m_csd && !isMaximized() && !isFullScreen();
    if (m_root && m_root->layout())
        m_root->layout()->setContentsMargins(framed ? QMargins(1, 1, 1, 1) : QMargins());
    if (m_maxButton) {
        m_maxButton->setIcon(
            glyphIcon(isMaximized() ? Glyph::Restore : Glyph::Maximize, m_theme.fg));
        m_maxButton->setToolTip(isMaximized() ? tr("Restore") : tr("Maximize"));
    }
}

Qt::Edges MainWindow::edgesAt(const QPoint &pos) const
{
    if (!m_csd || isMaximized() || isFullScreen())
        return {};
    Qt::Edges edges;
    if (pos.x() < ResizeMargin)
        edges |= Qt::LeftEdge;
    else if (pos.x() >= width() - ResizeMargin)
        edges |= Qt::RightEdge;
    if (pos.y() < ResizeMargin)
        edges |= Qt::TopEdge;
    else if (pos.y() >= height() - ResizeMargin)
        edges |= Qt::BottomEdge;
    return edges;
}

void MainWindow::updateResizeCursor(Qt::Edges edges)
{
    if (!edges) {
        if (m_resizeCursor) {
            QGuiApplication::restoreOverrideCursor();
            m_resizeCursor = false;
        }
        return;
    }
    Qt::CursorShape shape = Qt::SizeHorCursor;
    if (edges == (Qt::LeftEdge | Qt::TopEdge) || edges == (Qt::RightEdge | Qt::BottomEdge))
        shape = Qt::SizeFDiagCursor;
    else if (edges == (Qt::RightEdge | Qt::TopEdge) || edges == (Qt::LeftEdge | Qt::BottomEdge))
        shape = Qt::SizeBDiagCursor;
    else if (edges & (Qt::TopEdge | Qt::BottomEdge))
        shape = Qt::SizeVerCursor;

    if (m_resizeCursor)
        QGuiApplication::changeOverrideCursor(shape);
    else
        QGuiApplication::setOverrideCursor(shape);
    m_resizeCursor = true;
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    // Inline rename: Enter commits, Esc cancels, clicking elsewhere commits
    // (or cancels if the name isn't valid). Enter must be consumed here:
    // QLineEdit passes it on, and the view would then open the item.
    if (m_renameEdit && watched == m_renameEdit) {
        if (event->type() == QEvent::KeyPress) {
            const int key = static_cast<QKeyEvent *>(event)->key();
            if (key == Qt::Key_Escape) {
                cancelRename();
                return true;
            }
            if (key == Qt::Key_Return || key == Qt::Key_Enter) {
                commitRename();
                return true;
            }
        }
        if (event->type() == QEvent::FocusOut
            && static_cast<QFocusEvent *>(event)->reason() != Qt::PopupFocusReason) {
            if (!commitRename())
                cancelRename();
        }
        return QMainWindow::eventFilter(watched, event);
    }

    switch (event->type()) {
    case QEvent::DragEnter:
    case QEvent::DragMove:
    case QEvent::DragLeave:
    case QEvent::Drop:
        if (isFileViewport(watched))
            return handleViewDrag(watched == m_iconView->viewport()
                                      ? static_cast<QAbstractItemView *>(m_iconView)
                                      : m_detailView,
                                  event);
        if (m_sidebar && watched == m_sidebar->viewport())
            return handleSidebarDrag(event);
        if (m_tabBar && watched == m_tabBar) {
            // Dropping onto a tab: into that tab's folder.
            if (event->type() == QEvent::DragLeave)
                return true;
            auto *drop = static_cast<QDropEvent *>(event);
            const int index = m_tabBar->tabAt(drop->position().toPoint());
            const QStringList sources = localPaths(drop->mimeData());
            const QString target = index >= 0 ? m_tabs.at(index).path : QString();
            const Qt::DropAction action =
                target.isEmpty() ? Qt::IgnoreAction
                                 : dropActionFor(sources, target, drop->modifiers(), drop->possibleActions());
            if (action == Qt::IgnoreAction) {
                if (event->type() == QEvent::DragEnter && !sources.isEmpty())
                    drop->acceptProposedAction();
                else
                    drop->ignore();
                return true;
            }
            drop->setDropAction(action);
            drop->accept();
            if (event->type() == QEvent::Drop)
                performDrop(sources, target, action);
            return true;
        }
        break;
    default:
        break;
    }

    // Empty menu bar space behaves like the header bars: drag to move,
    // double-click to maximise.
    if (m_csd && m_menuBar && watched == m_menuBar) {
        auto *me = static_cast<QMouseEvent *>(event);
        switch (event->type()) {
        case QEvent::MouseButtonPress:
            m_menuPressed = me->button() == Qt::LeftButton
                            && !m_menuBar->actionAt(me->position().toPoint());
            m_menuPressPos = me->position().toPoint();
            break;
        case QEvent::MouseMove:
            if (m_menuPressed && (me->buttons() & Qt::LeftButton)
                && (me->position().toPoint() - m_menuPressPos).manhattanLength()
                       >= QApplication::startDragDistance()) {
                m_menuPressed = false;
                windowHandle()->startSystemMove();
                return true;
            }
            break;
        case QEvent::MouseButtonRelease:
            m_menuPressed = false;
            break;
        case QEvent::MouseButtonDblClick:
            if (!m_menuBar->actionAt(me->position().toPoint())) {
                isMaximized() ? showNormal() : showMaximized();
                return true;
            }
            break;
        default:
            break;
        }
    }

    if (m_csd && watched == windowHandle()) {
        switch (event->type()) {
        case QEvent::MouseMove: {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->buttons() == Qt::NoButton)
                updateResizeCursor(edgesAt(me->position().toPoint()));
            break;
        }
        case QEvent::MouseButtonPress: {
            auto *me = static_cast<QMouseEvent *>(event);
            const Qt::Edges edges = edgesAt(me->position().toPoint());
            if (me->button() == Qt::LeftButton && edges) {
                windowHandle()->startSystemResize(edges);
                return true;
            }
            break;
        }
        case QEvent::Leave:
            updateResizeCursor({});
            break;
        default:
            break;
        }
    } else if (m_sidebar && watched == m_sidebar->viewport()
               && event->type() == QEvent::MouseButtonPress) {
        auto *me = static_cast<QMouseEvent *>(event);
        QListWidgetItem *item = m_sidebar->itemAt(me->position().toPoint());
        if (item && me->button() == Qt::MiddleButton && !item->data(PathRole).toString().isEmpty()) {
            openTab(item->data(PathRole).toString(), false);
            return true;
        }
        if (item && me->button() == Qt::LeftButton && item->data(EjectRole).toBool()
            && ejectRect(m_sidebar->visualItemRect(item)).contains(me->position().toPoint())) {
            ejectDrive(item->data(PathRole).toString(), item->text());
            return true; // don't also open it
        }
    } else if (isFileViewport(watched) && event->type() == QEvent::MouseButtonPress
               && static_cast<QMouseEvent *>(event)->button() == Qt::MiddleButton) {
        auto *view = static_cast<QAbstractItemView *>(static_cast<QWidget *>(watched)->parentWidget());
        const QModelIndex source =
            toSource(view->indexAt(static_cast<QMouseEvent *>(event)->position().toPoint()));
        if (source.isValid() && m_model->isDir(source))
            openTab(m_model->filePath(source), false);
        return true;
    } else if (m_tabBar && watched == m_tabBar && event->type() == QEvent::MouseButtonRelease
               && static_cast<QMouseEvent *>(event)->button() == Qt::MiddleButton) {
        const int index = m_tabBar->tabAt(static_cast<QMouseEvent *>(event)->position().toPoint());
        if (index >= 0)
            closeTab(index);
        return true;
    } else if (m_view
               && ((isFileViewport(watched) && event->type() == QEvent::MouseButtonPress)
                   || ((watched == m_iconView || watched == m_detailView)
                       && event->type() == QEvent::KeyPress))) {
        m_pendingSelection.clear();
        m_pendingRename.clear();
    }
    if (m_renameEdit && isFileViewport(watched) && event->type() == QEvent::Paint)
        updateRenameGeometry(); // item positions are final when the view paints
    if (m_viewStack && watched == m_viewStack && event->type() == QEvent::Resize) {
        if (m_floatingBar->isVisible())
            positionFloatingBar();
        if (m_opsPanel->isVisible()) {
            m_opsPanel->reposition();
            m_toast->setAvoidRect(m_opsPanel->geometry());
        }
        if (m_toast->isVisible())
            m_toast->reposition();
        updateRenameGeometry();
    } else if (watched == m_searchEdit && event->type() == QEvent::KeyPress) {
        if (static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            setSearchActive(false);
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

// ------------------------------------------------------ file operations ---

QStringList MainWindow::selectedPaths() const
{
    QStringList paths;
    for (const QModelIndex &index : selectedViewIndexes())
        paths.append(m_model->filePath(toSource(index)));
    return paths;
}

void MainWindow::openSelection()
{
    const QModelIndexList selected = selectedViewIndexes();
    if (selected.size() == 1) {
        openIndex(selected.first());
        return;
    }
    for (const QModelIndex &index : selected) {
        const QModelIndex source = toSource(index);
        if (!m_model->isDir(source))
            QDesktopServices::openUrl(QUrl::fromLocalFile(m_model->filePath(source)));
    }
}

void MainWindow::copySelection(bool cut)
{
    const QStringList paths = selectedPaths();
    if (paths.isEmpty())
        return;
    FileOperations::setClipboardFiles(paths, cut);
    const int n = int(paths.size());
    if (cut)
        showMessage(n == 1 ? tr("“%1” will be moved when you paste").arg(QFileInfo(paths.first()).fileName())
                           : tr("%1 items will be moved when you paste").arg(n));
    else
        showMessage(n == 1 ? tr("“%1” copied").arg(QFileInfo(paths.first()).fileName())
                           : tr("%1 items copied").arg(n));
}

void MainWindow::paste()
{
    QStringList paths;
    bool cut = false;
    if (!FileOperations::clipboardFiles(&paths, &cut)) {
        showMessage(tr("There is nothing to paste"));
        return;
    }
    if (cut) {
        m_ops->move(paths, m_current);
        QApplication::clipboard()->clear(); // a cut is pasted only once
    } else {
        m_ops->copy(paths, m_current);
    }
}

void MainWindow::trashSelection()
{
    if (m_model->isTrash()) {
        deleteSelection(); // already in the Trash: the only step left is forever
        return;
    }
    const QStringList paths = selectedPaths();
    if (!paths.isEmpty())
        m_ops->trash(paths);
}

bool MainWindow::confirmPermanentDelete(const QStringList &paths, bool wholeTrash)
{
    QString question;
    if (wholeTrash)
        question = tr("Empty all items from the Trash?");
    else if (paths.size() == 1)
        question = tr("Permanently delete “%1”?").arg(QFileInfo(paths.first()).fileName());
    else
        question = tr("Permanently delete %1 selected items?").arg(paths.size());
    QMessageBox box(QMessageBox::Warning, wholeTrash ? tr("Empty Trash") : tr("Delete Permanently"),
                    question, QMessageBox::NoButton, this);
    box.setInformativeText(wholeTrash ? tr("All items in the Trash will be deleted forever.")
                                      : tr("If you delete an item, it will be gone forever."));
    QPushButton *cancel = box.addButton(QMessageBox::Cancel);
    QPushButton *remove =
        box.addButton(wholeTrash ? tr("Empty Trash") : tr("Delete"), QMessageBox::DestructiveRole);
    box.setDefaultButton(cancel);
    box.exec();
    return box.clickedButton() == remove;
}

void MainWindow::deleteSelection()
{
    const QStringList paths = selectedPaths();
    if (!paths.isEmpty() && confirmPermanentDelete(paths, false))
        m_ops->deletePermanently(paths);
}

void MainWindow::restoreSelection()
{
    if (!m_model->isTrash())
        return;
    const QStringList paths = selectedPaths();
    if (!paths.isEmpty())
        m_ops->restore(paths);
}

void MainWindow::emptyTrash()
{
    bool empty = true;
    for (const QString &dir : Trash::filesDirs()) {
        if (!QDir(dir).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System))
            empty = false;
    }
    if (empty) {
        showMessage(tr("The Trash is already empty"));
        return;
    }
    if (confirmPermanentDelete({}, true))
        m_ops->emptyTrash();
}

// A drop into the Trash (its sidebar entry or the Trash view) trashes.
void MainWindow::performDrop(const QStringList &sources, const QString &targetDir,
                             Qt::DropAction action)
{
    if (Trash::isFilesDir(targetDir))
        m_ops->trash(sources);
    else if (action == Qt::MoveAction)
        m_ops->move(sources, targetDir);
    else
        m_ops->copy(sources, targetDir);
}

void MainWindow::updateEditActions()
{
    const bool hasSelection = m_view && m_view->selectionModel()->hasSelection();
    for (QAction *action : {m_openAct, m_cutAct, m_copyAct, m_trashAct})
        action->setEnabled(hasSelection);
    const bool inTrash = m_model && m_model->isTrash();
    const bool writable = !inTrash && !m_current.isEmpty() && QFileInfo(m_current).isWritable();
    m_deleteAct->setEnabled(hasSelection);
    m_restoreAct->setEnabled(inTrash && hasSelection);
    if (m_trashRestoreButton)
        m_trashRestoreButton->setText(tr("Restore")); // setDefaultAction resets it
    m_emptyTrashAct->setEnabled(Trash::isSupported());
    m_pasteAct->setEnabled(writable && FileOperations::clipboardHasFiles());
    m_newFolderAct->setEnabled(writable);
    m_newFileAct->setEnabled(writable);
    m_renameAct->setEnabled(!inTrash && m_view && selectedViewIndexes().size() == 1);
    m_undoAct->setEnabled(m_ops->canUndo());
    m_undoAct->setText(m_ops->undoText());
}

void MainWindow::showContextMenu(const QPoint &pos)
{
    const QModelIndex index = m_view->indexAt(pos);
    QItemSelectionModel *selection = m_view->selectionModel();
    if (index.isValid() && !selection->isSelected(index))
        selection->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect);
    else if (!index.isValid())
        selection->clearSelection();
    updateEditActions();

    QMenu *menu = makeMenu();
    menu->setAttribute(Qt::WA_DeleteOnClose);
    m_contextFolder = m_current;
    if (index.isValid() && m_model->isDir(toSource(index)))
        m_contextFolder = m_model->filePath(toSource(index));
    if (index.isValid() && m_model->isTrash()) {
        menu->addAction(m_restoreAct);
        menu->addAction(m_deleteAct);
        menu->addSeparator();
        menu->addAction(m_openAct);
        menu->addAction(m_copyAct);
    } else if (!index.isValid() && m_model->isTrash()) {
        menu->addAction(m_emptyTrashAct);
        menu->addAction(m_hiddenAct);
    } else if (index.isValid()) {
        menu->addAction(m_openAct);
        if (m_model->isDir(toSource(index)))
            menu->addAction(m_openInNewTabAct);
        if (m_openTerminalHereAct && m_model->isDir(toSource(index)))
            menu->addAction(m_openTerminalHereAct);
        menu->addSeparator();
        menu->addAction(m_renameAct);
        menu->addSeparator();
        menu->addAction(m_cutAct);
        menu->addAction(m_copyAct);
        menu->addSeparator();
        menu->addAction(m_trashAct);
        menu->addAction(m_deleteAct);
    } else {
        menu->addAction(m_newFolderAct);
        menu->addAction(m_newFileAct);
        menu->addSeparator();
        if (m_openTerminalHereAct) {
            menu->addAction(m_openTerminalHereAct);
            menu->addSeparator();
        }
        menu->addAction(m_pasteAct);
        menu->addAction(m_undoAct);
        menu->addSeparator();
        menu->addAction(m_copyLocationAct);
        menu->addAction(m_hiddenAct);
    }
    menu->popup(m_view->viewport()->mapToGlobal(pos));
}

void MainWindow::onJobFinished(FileJob *job)
{
    const QList<TransferResult> results = job->results();
    const QStringList errors = job->errors();
    const int done = int(results.size());
    const bool userAction = job->property(FileOperations::UserActionProperty).toBool();

    if (job->isCanceled()) {
        showMessage(tr("Canceled"));
    } else if (!userAction) {
        if (done > 0)
            showMessage(tr("Undone"));
    } else if (job->kind() == FileJob::Kind::Trash && done > 0) {
        const QString text =
            done == 1 ? tr("“%1” moved to the Trash").arg(QFileInfo(results.first().source).fileName())
                      : tr("%n items moved to the Trash", nullptr, done);
        m_toast->showMessage(text, tr("Undo"), [this] { m_ops->undo(); });
    } else if (job->kind() == FileJob::Kind::Delete && done > 0) {
        showMessage(done == 1 ? tr("1 item deleted") : tr("%1 items deleted").arg(done));
    } else if (job->kind() == FileJob::Kind::Restore && done > 0) {
        const QFileInfo where(results.first().target);
        showMessage(done == 1 ? tr("“%1” restored to “%2”")
                                    .arg(where.fileName(), QDir::toNativeSeparators(where.absolutePath()))
                              : tr("%1 items restored").arg(done));
    } else if (!job->flushedDrives().isEmpty() && done > 0) {
        showMessage(tr("Done — everything is written to “%1”")
                        .arg(job->flushedDrives().join(QStringLiteral("”, “"))));
    } else if (done > 0) {
        const bool copied = job->kind() == FileJob::Kind::Copy;
        if (done == 1)
            showMessage(copied ? tr("1 item copied") : tr("1 item moved"));
        else
            showMessage(copied ? tr("%1 items copied").arg(done) : tr("%1 items moved").arg(done));
    }

    // Highlight what was pasted (or put back by an undo) in the open folder.
    if (job->kind() != FileJob::Kind::Trash) {
        QStringList here;
        for (const TransferResult &r : results) {
            if (QFileInfo(r.target).absolutePath() == m_current)
                here.append(r.target);
        }
        selectPaths(here);
    }

    if (!errors.isEmpty()) {
        QString text = errors.mid(0, 10).join('\n');
        if (errors.size() > 10)
            text += '\n' + tr("…and %n more", nullptr, int(errors.size() - 10));
        QMessageBox::warning(this, tr("Some items couldn't be processed"), text);
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!m_ops->activeJobs().isEmpty()) {
        QMessageBox box(QMessageBox::Question, tr("Cancel file operations?"),
                        tr("Files are still being copied or moved. Closing now will cancel "
                           "them; items already finished are kept."),
                        QMessageBox::NoButton, this);
        QPushButton *keep = box.addButton(tr("Keep Working"), QMessageBox::RejectRole);
        box.addButton(tr("Cancel and Close"), QMessageBox::AcceptRole);
        box.setDefaultButton(keep);
        box.exec();
        if (box.clickedButton() == keep) {
            event->ignore();
            return;
        }
        m_ops->cancelAll();
    }
    saveSettings();
    QMainWindow::closeEvent(event);
}

void MainWindow::selectPaths(const QStringList &paths)
{
    m_pendingSelection = paths;
    m_pendingSince.start();
    applyPendingSelection();
}

void MainWindow::applyPendingSelection()
{
    if (m_pendingSelection.isEmpty())
        return;
    if (m_pendingSince.elapsed() > 5000) { // give up quietly
        m_pendingSelection.clear();
        return;
    }

    QItemSelection selection;
    QModelIndex first;
    int found = 0;
    for (const QString &path : std::as_const(m_pendingSelection)) {
        const QModelIndex index = toView(m_model->indexForPath(path));
        if (!index.isValid())
            continue;
        selection.select(index, index.siblingAtColumn(DirectoryModel::ColumnCount - 1));
        if (!first.isValid())
            first = index;
        ++found;
    }
    if (!first.isValid())
        return;

    m_applyingSelection = true;
    QItemSelectionModel *model = m_view->selectionModel();
    model->select(selection, QItemSelectionModel::ClearAndSelect);
    model->setCurrentIndex(first, QItemSelectionModel::NoUpdate);
    m_view->scrollTo(first);
    m_applyingSelection = false;

    if (found == m_pendingSelection.size())
        m_pendingSelection.clear();

    if (!m_pendingRename.isEmpty()) {
        const QModelIndex index = toView(m_model->indexForPath(m_pendingRename));
        if (index.isValid()) {
            m_pendingRename.clear();
            startRename(index);
        }
    }
}

// ------------------------------------------------------------- menu bar ---

QWidget *MainWindow::buildMenuBar()
{
    m_menuBar = new QMenuBar(this);
    m_menuBar->setObjectName("menuBar");
    m_menuBar->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto addMenu = [this](const QString &title) {
        QMenu *menu = makeMenu();
        menu->setTitle(title);
        m_menuBar->addMenu(menu);
        return menu;
    };

    QMenu *file = addMenu(tr("&File"));
    file->addAction(m_newFolderAct);
    file->addAction(m_newFileAct);
    file->addSeparator();
    file->addAction(m_openAct);
    file->addSeparator();
    if (Trash::isSupported()) {
        file->addAction(m_emptyTrashAct);
        file->addSeparator();
    }
    file->addAction(m_newTabAct);
    file->addAction(m_reopenTabAct);
    file->addSeparator();
    file->addAction(m_closeAct);
    file->addAction(m_closeWindowAct);
    file->addAction(m_quitAct);

    QMenu *edit = addMenu(tr("&Edit"));
    edit->addAction(m_undoAct);
    edit->addSeparator();
    edit->addAction(m_cutAct);
    edit->addAction(m_copyAct);
    edit->addAction(m_pasteAct);
    edit->addSeparator();
    edit->addAction(m_selectAllAct);
    edit->addAction(m_renameAct);
    edit->addAction(m_trashAct);
    edit->addAction(m_deleteAct);
    if (Trash::isSupported())
        edit->addAction(m_restoreAct);
    edit->addSeparator();
    edit->addAction(m_copyLocationAct);
    edit->addSeparator();
    edit->addAction(m_prefsAct);

    QMenu *view = addMenu(tr("&View"));
    view->addAction(m_gridAct);
    view->addAction(m_listAct);
    view->addSeparator();
    QMenu *sortMenu = makeMenu();
    sortMenu->setTitle(tr("Sort Files"));
    sortMenu->addActions(m_sortColumnActs);
    sortMenu->addSeparator();
    sortMenu->addAction(m_sortAscAct);
    sortMenu->addAction(m_sortDescAct);
    view->addMenu(sortMenu);
    view->addSeparator();
    view->addAction(m_hiddenAct);
    view->addAction(m_sidebarAct);
    if (m_terminalAct)
        view->addAction(m_terminalAct);

    QMenu *go = addMenu(tr("&Go"));
    go->addAction(m_backAct);
    go->addAction(m_forwardAct);
    go->addAction(m_upAct);
    go->addAction(m_homeAct);
    go->addSeparator();
    go->addAction(m_bookmarkAct);
    go->addSeparator();
    go->addAction(m_locationAct);
    go->addAction(m_searchAct);

    QMenu *help = addMenu(tr("&Help"));
    help->addAction(m_aboutAct);

    m_menuBar->installEventFilter(this);
    return m_menuBar;
}

// ----------------------------------------------------- new items, rename ---

void MainWindow::createNewItem(bool folder)
{
    if (m_current.isEmpty() || !QFileInfo(m_current).isWritable()) {
        showMessage(tr("You can't create items in this folder"));
        return;
    }
    const QDir dir(m_current);
    const QString base = folder ? tr("New Folder") : tr("New File");
    QString path = dir.filePath(base);
    for (int i = 2; QFileInfo(path).exists() || QFileInfo(path).isSymLink(); ++i)
        path = dir.filePath(base + ' ' + QString::number(i));

    bool ok = false;
    if (folder) {
        ok = dir.mkdir(QFileInfo(path).fileName());
    } else {
        QFile file(path);
        ok = file.open(QIODevice::WriteOnly | QIODevice::NewOnly);
    }
    if (!ok) {
        showMessage(folder ? tr("Couldn't create the folder") : tr("Couldn't create the file"));
        return;
    }
    // Undo trashes it (same as undoing a copy).
    m_ops->recordUndo(FileJob::Kind::Copy, {{QString(), path}},
                      folder ? tr("Undo New Folder") : tr("Undo New File"));
    m_pendingRename = path;
    selectPaths({path});
}

void MainWindow::renameSelection()
{
    const QModelIndexList selected = selectedViewIndexes();
    if (selected.size() == 1)
        startRename(selected.first());
}

void MainWindow::startRename(const QModelIndex &viewIndex)
{
    cancelRename();
    const QModelIndex source = toSource(viewIndex);
    if (!source.isValid())
        return;
    m_renamePath = m_model->filePath(source);
    m_view->scrollTo(viewIndex);

    m_renameEdit = new QLineEdit(m_view->viewport());
    m_renameEdit->setObjectName("renameEdit");
    m_renameEdit->setText(m_model->fileName(source));
    m_renameEdit->setAlignment(m_grid ? Qt::AlignCenter : Qt::AlignLeft | Qt::AlignVCenter);
    m_renameEdit->installEventFilter(this); // Enter / Esc / focus-out
    updateRenameGeometry();
    m_renameEdit->show();
    m_renameEdit->setFocus(Qt::OtherFocusReason);

    // Select the name without its extension, like Nautilus.
    const QString name = m_renameEdit->text();
    int end = int(name.size());
    if (!m_model->isDir(source)) {
        const int dot = int(name.lastIndexOf('.'));
        if (dot > 0) {
            end = dot;
            if (name.left(dot).endsWith(QLatin1String(".tar")))
                end -= 4;
        }
    }
    m_renameEdit->setSelection(0, end);
}

void MainWindow::updateRenameGeometry()
{
    if (!m_renameEdit)
        return;
    const QModelIndex index = toView(m_model->indexForPath(m_renamePath));
    if (!index.isValid()) {
        cancelRename();
        return;
    }
    const FileItemDelegate *delegate = m_grid ? m_gridDelegate : m_listDelegate;
    QRect rect = delegate->nameEditorRect(m_view->visualRect(index), m_view->fontMetrics());
    const int width = m_view->viewport()->width();
    if (rect.right() > width - 3)
        rect.moveRight(width - 3);
    if (rect.left() < 3)
        rect.moveLeft(3);
    m_renameEdit->setGeometry(rect);
}

void MainWindow::cancelRename()
{
    if (!m_renameEdit)
        return;
    QLineEdit *edit = m_renameEdit;
    m_renameEdit = nullptr;
    m_renamePath.clear();
    edit->removeEventFilter(this);
    edit->hide();
    edit->deleteLater();
    m_view->setFocus();
}

// Returns false (and keeps the field open) if the name can't be used.
bool MainWindow::commitRename()
{
    if (!m_renameEdit || m_committingRename)
        return true;
    const QString newName = m_renameEdit->text().trimmed();
    const QFileInfo old(m_renamePath);
    if (newName.isEmpty() || newName == old.fileName()) {
        cancelRename();
        return true;
    }

    QString problem;
    if (newName == QLatin1String(".") || newName == QLatin1String(".."))
        problem = tr("“%1” isn't a valid name").arg(newName);
    else if (newName.contains('/'))
        problem = tr("Names can't contain “/”");
#if defined(Q_OS_WIN)
    else if (newName.contains(QRegularExpression(QStringLiteral(R"([<>:"\\|?*])"))))
        problem = tr("Names can't contain any of: < > : \" \\ | ? *");
#endif
    const QString target = old.absolutePath() + '/' + newName;
    const QFileInfo targetInfo(target);
    if (problem.isEmpty() && (targetInfo.exists() || targetInfo.isSymLink()) && targetInfo != old)
        problem = tr("“%1” already exists").arg(newName);

    bool renamed = false;
    if (problem.isEmpty()) {
        m_committingRename = true;
        renamed = QDir().rename(m_renamePath, target);
        m_committingRename = false;
        if (!renamed)
            problem = tr("Couldn't rename “%1”").arg(old.fileName());
    }
    if (!renamed) {
        showMessage(problem);
        if (m_renameEdit)
            m_renameEdit->setFocus();
        return false;
    }

    m_ops->recordUndo(FileJob::Kind::Move, {{m_renamePath, target}}, tr("Undo Rename"));
    cancelRename();
    selectPaths({target});
    return true;
}

// ---------------------------------------------------------- file views ---

void MainWindow::setupFileView(QAbstractItemView *view)
{
    view->setModel(m_proxy);
    view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view->setFrameShape(QFrame::NoFrame);
    view->setMouseTracking(true);
    view->viewport()->setAttribute(Qt::WA_Hover);
    view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    // Drags start from the view (model provides the uri-list); drops are
    // intercepted in eventFilter -> handleViewDrag.
    view->setDragEnabled(true);
    view->setDragDropMode(QAbstractItemView::DragDrop);
    view->setDefaultDropAction(Qt::MoveAction);
    view->setDropIndicatorShown(false);
    // Qt only delivers drag events to widgets that accept drops; mark the
    // viewport explicitly so they stop there (where eventFilter handles them)
    // with viewport-relative positions.
    view->viewport()->setAcceptDrops(true);
    view->setContextMenuPolicy(Qt::CustomContextMenu);
    // Clicks and keys in the view mean the user is choosing: stop applying a
    // pending "highlight what was pasted" (see eventFilter).
    view->installEventFilter(this);
    view->viewport()->installEventFilter(this);
    connect(view, &QAbstractItemView::activated, this, &MainWindow::openIndex);
    connect(view, &QWidget::customContextMenuRequested, this, &MainWindow::showContextMenu);
    connect(m_thumbnailer, &Thumbnailer::thumbnailReady, view->viewport(),
            qOverload<>(&QWidget::update));
    // Keep an open rename field on its item while scrolling.
    connect(view->verticalScrollBar(), &QScrollBar::valueChanged, this,
            &MainWindow::updateRenameGeometry);
}

QModelIndexList MainWindow::selectedViewIndexes() const
{
    QModelIndexList items;
    for (const QModelIndex &index : m_iconView->selectionModel()->selectedIndexes()) {
        if (index.column() == 0)
            items.append(index);
    }
    return items;
}

bool MainWindow::isFileViewport(const QObject *object) const
{
    return (m_iconView && object == m_iconView->viewport())
           || (m_detailView && object == m_detailView->viewport());
}

void MainWindow::sortBy(int column, Qt::SortOrder order)
{
    m_detailView->sortByColumn(column, order); // updates the header and sorts the model
    updateSortActions();
}

void MainWindow::updateSortActions()
{
    if (m_sortColumnActs.isEmpty())
        return;
    const int column = m_model->sortColumn();
    if (column >= 0 && column < m_sortColumnActs.size())
        m_sortColumnActs.at(column)->setChecked(true);
    (m_model->sortOrder() == Qt::AscendingOrder ? m_sortAscAct : m_sortDescAct)->setChecked(true);
}

// --------------------------------------------------------- drag & drop ---

namespace {

QStringList localPaths(const QMimeData *mime)
{
    QStringList paths;
    if (!mime || !mime->hasUrls())
        return paths;
    for (const QUrl &url : mime->urls()) {
        if (url.isLocalFile())
            paths.append(QDir::cleanPath(url.toLocalFile()));
    }
    return paths;
}

} // namespace

QByteArray MainWindow::deviceOf(const QString &path) const
{
    const QFileInfo info(path);
    const QString dir = info.isDir() ? info.absoluteFilePath() : info.absolutePath();
    auto it = m_deviceCache.constFind(dir);
    if (it == m_deviceCache.constEnd())
        it = m_deviceCache.insert(dir, QStorageInfo(dir).device());
    return *it;
}

// Like Nautilus: same drive moves, another drive copies; Ctrl forces copy,
// Shift forces move. Returns IgnoreAction for drops that make no sense.
Qt::DropAction MainWindow::dropActionFor(const QStringList &sources, const QString &targetDir,
                                         Qt::KeyboardModifiers modifiers,
                                         Qt::DropActions possible) const
{
    if (sources.isEmpty() || targetDir.isEmpty() || !QFileInfo(targetDir).isWritable())
        return Qt::IgnoreAction;
    bool allAlreadyThere = true;
    for (const QString &source : sources) {
        if (targetDir == source || targetDir.startsWith(source + QLatin1Char('/')))
            return Qt::IgnoreAction; // a folder into itself
        if (QFileInfo(source).absolutePath() != targetDir)
            allAlreadyThere = false;
    }

    if (Trash::isFilesDir(targetDir)) {
        for (const QString &source : sources) {
            if (!Trash::filesDirOf(source).isEmpty())
                return Qt::IgnoreAction; // already in the Trash
        }
        return (possible & Qt::MoveAction) ? Qt::MoveAction : Qt::IgnoreAction;
    }

    Qt::DropAction action;
    if (modifiers & Qt::ControlModifier)
        action = Qt::CopyAction;
    else if (modifiers & Qt::ShiftModifier)
        action = Qt::MoveAction;
    else
        action = deviceOf(sources.first()) == deviceOf(targetDir) ? Qt::MoveAction
                                                                   : Qt::CopyAction;
    if (action == Qt::MoveAction && allAlreadyThere)
        return Qt::IgnoreAction; // nothing would change
    if (!(possible & action))
        action = (possible & Qt::CopyAction) ? Qt::CopyAction : Qt::IgnoreAction;
    return action;
}

void MainWindow::setDropTarget(const QModelIndex &viewIndex)
{
    m_gridDelegate->setDropTarget(viewIndex);
    m_listDelegate->setDropTarget(viewIndex);
    m_view->viewport()->update();
}

bool MainWindow::handleViewDrag(QAbstractItemView *view, QEvent *event)
{
    if (event->type() == QEvent::DragLeave) {
        setDropTarget({});
        m_dragScrollTimer->stop();
        return true;
    }
    auto *drop = static_cast<QDropEvent *>(event); // DragEnter/DragMove derive from it
    const QStringList sources = localPaths(drop->mimeData());
    if (sources.isEmpty()) {
        drop->ignore();
        return true;
    }

    // Over a folder (that isn't being dragged itself): drop into it.
    // Anywhere else: drop into the open folder.
    const QPoint pos = drop->position().toPoint();
    QModelIndex target = view->indexAt(pos);
    QString targetDir = m_current;
    if (target.isValid()) {
        const QModelIndex source = toSource(target);
        const QString path = m_model->filePath(source);
        if (m_model->isDir(source) && !sources.contains(path))
            targetDir = path;
        else
            target = QModelIndex();
    }
    const Qt::DropAction action =
        dropActionFor(sources, targetDir, drop->modifiers(), drop->possibleActions());

    if (event->type() == QEvent::Drop) {
        setDropTarget({});
        m_dragScrollTimer->stop();
        if (action == Qt::IgnoreAction) {
            drop->ignore();
            return true;
        }
        drop->setDropAction(action);
        drop->accept();
        performDrop(sources, targetDir, action);
        return true;
    }

    // DragEnter / DragMove: feedback only.
    setDropTarget(action == Qt::IgnoreAction ? QModelIndex() : target.siblingAtColumn(0));
    const int edge = 32;
    const int height = view->viewport()->height();
    m_dragScrollStep = pos.y() < edge ? -20 : (pos.y() > height - edge ? 20 : 0);
    if (m_dragScrollStep != 0 && !m_dragScrollTimer->isActive())
        m_dragScrollTimer->start();
    if (action == Qt::IgnoreAction) {
        // Still accept the enter, so we keep getting move events.
        if (event->type() == QEvent::DragEnter)
            drop->acceptProposedAction();
        else
            drop->ignore();
        return true;
    }
    drop->setDropAction(action);
    drop->accept();
    return true;
}

// Dropping onto a sidebar place or drive copies/moves into it.
bool MainWindow::handleSidebarDrag(QEvent *event)
{
    if (event->type() == QEvent::DragLeave) {
        syncSidebarSelection();
        return true;
    }
    auto *drop = static_cast<QDropEvent *>(event);
    const QStringList sources = localPaths(drop->mimeData());
    QListWidgetItem *item = m_sidebar->itemAt(drop->position().toPoint());

    // Folders dropped on empty space (or a separator) become bookmarks.
    if (!item || item->data(SeparatorRole).toBool()) {
        bool allFolders = !sources.isEmpty();
        for (const QString &source : sources)
            allFolders = allFolders && QFileInfo(source).isDir() && Trash::filesDirOf(source).isEmpty();
        if (!allFolders) {
            if (event->type() == QEvent::DragEnter && !sources.isEmpty())
                drop->acceptProposedAction();
            else
                drop->ignore();
            return true;
        }
        drop->setDropAction(drop->possibleActions() & Qt::LinkAction ? Qt::LinkAction
                                                                     : Qt::CopyAction);
        drop->accept();
        if (event->type() == QEvent::Drop) {
            for (const QString &source : sources)
                Bookmarks::add(source);
            showMessage(sources.size() == 1 ? tr("“%1” added to bookmarks").arg(QFileInfo(sources.first()).fileName())
                                            : tr("%1 folders added to bookmarks").arg(sources.size()));
            scheduleSidebarRefresh();
            updateBookmarkAction();
        }
        return true;
    }
    const QString targetDir = QDir::cleanPath(item->data(PathRole).toString());
    const Qt::DropAction action =
        targetDir.isEmpty()
            ? Qt::IgnoreAction
            : dropActionFor(sources, targetDir, drop->modifiers(), drop->possibleActions());

    if (event->type() == QEvent::Drop) {
        syncSidebarSelection();
        if (action == Qt::IgnoreAction) {
            drop->ignore();
            return true;
        }
        drop->setDropAction(action);
        drop->accept();
        performDrop(sources, targetDir, action);
        return true;
    }

    if (action == Qt::IgnoreAction) {
        syncSidebarSelection();
        if (event->type() == QEvent::DragEnter && !sources.isEmpty())
            drop->acceptProposedAction();
        else
            drop->ignore();
        return true;
    }
    m_sidebar->setCurrentItem(item); // highlight the place under the cursor
    drop->setDropAction(action);
    drop->accept();
    return true;
}

// ------------------------------------------------------------ terminal ---

#if defined(KAFTILOUS_TERMINAL)

QWidget *MainWindow::buildTerminalPanel()
{
    m_terminalPanel = new QWidget(this);
    m_terminalPanel->setObjectName("terminalPanel");
    m_terminalPanel->setAttribute(Qt::WA_StyledBackground);
    auto *layout = new QVBoxLayout(m_terminalPanel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *bar = new QWidget(m_terminalPanel);
    bar->setObjectName("terminalBar");
    bar->setAttribute(Qt::WA_StyledBackground);
    auto *row = new QHBoxLayout(bar);
    row->setContentsMargins(14, 3, 6, 3);
    m_terminalTitle = new QLabel(tr("Terminal"), bar);
    m_terminalTitle->setObjectName("terminalTitle");
    m_terminalCloseButton = makeButton(tr("Hide terminal (Ctrl+`)"));
    m_terminalCloseButton->setFixedSize(26, 26);
    connect(m_terminalCloseButton, &QToolButton::clicked, this,
            [this] { m_terminalAct->setChecked(false); });
    row->addWidget(m_terminalTitle, 1);
    row->addWidget(m_terminalCloseButton);
    layout->addWidget(bar);

    m_terminal = new TerminalWidget(m_terminalPanel);
    m_terminal->setPassthroughShortcuts({m_terminalAct->shortcut()});
    connect(m_terminal, &TerminalWidget::titleChanged, this, [this](const QString &title) {
        m_terminalTitle->setText(title.isEmpty() ? tr("Terminal") : title);
    });
    // `exit` closes the panel; the next one starts a fresh shell.
    connect(m_terminal, &TerminalWidget::finished, this, [this] {
        m_terminalTitle->setText(tr("Terminal"));
        m_terminalAct->setChecked(false);
    });
    layout->addWidget(m_terminal, 1);

    m_terminalPanel->hide();
    return m_terminalPanel;
}

// Show the terminal in `directory`: start a shell there, or `cd` the running
// one (only if it's idle at the prompt, never under a running program).
void MainWindow::showTerminal(const QString &directory)
{
    if (!m_terminal->isRunning()) {
        if (!m_terminal->start(directory)) {
            showMessage(tr("Couldn't start a terminal"));
            const QSignalBlocker block(m_terminalAct);
            m_terminalAct->setChecked(false);
            m_terminalButton->setChecked(false);
            return;
        }
    } else if (!m_terminal->changeDirectory(directory)) {
        showMessage(tr("A program is running in the terminal, so it stays where it is"));
    }

    if (!m_terminalAct->isChecked()) {
        const QSignalBlocker block(m_terminalAct);
        m_terminalAct->setChecked(true);
        m_terminalButton->setChecked(true);
    }
    if (m_terminalPanel->isHidden()) {
        m_terminalPanel->show();
        if (!m_terminalSized) {
            const int total = m_contentSplitter->height();
            const int height = m_terminalHeight > 80 ? qMin(m_terminalHeight, total - 120)
                                                     : total * 38 / 100;
            m_contentSplitter->setSizes({total - height, height});
            m_terminalSized = true;
        }
    }
    m_terminal->setFocus(Qt::ShortcutFocusReason);
}

void MainWindow::hideTerminal()
{
    const bool hadFocus = m_terminal->hasFocus();
    m_terminalPanel->hide();
    if (hadFocus)
        m_view->setFocus();
}

#else

QWidget *MainWindow::buildTerminalPanel()
{
    return nullptr;
}
void MainWindow::showTerminal(const QString &) {}
void MainWindow::hideTerminal() {}

#endif

// ----------------------------------------------------------- conflicts ---

void MainWindow::askConflict(FileJob *job, const QString &source, const QString &existing)
{
    m_conflicts.append({QPointer<FileJob>(job), source, existing});
    if (!m_conflictOpen)
        showNextConflict();
}

void MainWindow::showNextConflict()
{
    while (!m_conflicts.isEmpty()) {
        const PendingConflict next = m_conflicts.takeFirst();
        if (!next.job)
            continue; // cancelled or finished meanwhile
        m_conflictOpen = true;
        ConflictDialog dialog(next.source, next.existing, this);
        dialog.exec();
        m_conflictOpen = false;
        if (next.job)
            next.job->resolveConflict(dialog.resolution());
    }
}

// ---------------------------------------------------------------- tabs ---

QString MainWindow::tabTitle(const QString &path) const
{
    if (!Trash::filesDirOf(path).isEmpty() && Trash::isFilesDir(path))
        return tr("Trash");
    const QString name = QFileInfo(path).fileName();
    return name.isEmpty() ? QDir::toNativeSeparators(path) : name;
}

void MainWindow::addTabCloseButton(int index)
{
    auto *close = new QToolButton(m_tabBar);
    close->setObjectName("flat");
    close->setFixedSize(20, 20);
    close->setIconSize(QSize(12, 12));
    close->setFocusPolicy(Qt::NoFocus);
    close->setToolTip(tr("Close Tab (Ctrl+W)"));
    close->setIcon(glyphIcon(Glyph::Close, m_theme.dimFg));
    connect(close, &QToolButton::clicked, this, [this, close] {
        for (int i = 0; i < m_tabBar->count(); ++i) {
            if (m_tabBar->tabButton(i, QTabBar::RightSide) == close) {
                closeTab(i);
                return;
            }
        }
    });
    m_tabBar->setTabButton(index, QTabBar::RightSide, close);
}

void MainWindow::updateTabBar()
{
    m_tabBar->setVisible(m_tabs.size() > 1);
}

void MainWindow::saveActiveTab()
{
    if (m_activeTab < 0 || m_activeTab >= m_tabs.size())
        return;
    TabState &tab = m_tabs[m_activeTab];
    tab.path = m_current;
    tab.back = m_backHistory;
    tab.forward = m_forwardHistory;
    tab.selection = selectedPaths();
    tab.scroll = m_view->verticalScrollBar()->value();
}

// New tab right after the current one, like browsers. Background tabs
// (middle-click) don't take you there.
void MainWindow::openTab(const QString &path, bool activate)
{
    saveActiveTab();
    TabState tab;
    tab.path = QDir::cleanPath(path);
    const int index = m_activeTab + 1;
    m_switchingTabs = true;
    m_tabs.insert(index, tab);
    m_tabBar->insertTab(index, tabTitle(tab.path));
    m_tabBar->setTabToolTip(index, QDir::toNativeSeparators(tab.path));
    addTabCloseButton(index);
    m_switchingTabs = false;
    updateTabBar();
    if (activate)
        m_tabBar->setCurrentIndex(index);
    else
        showMessage(tr("Opened “%1” in a new tab").arg(tabTitle(tab.path)));
}

void MainWindow::closeTab(int index)
{
    if (index < 0 || index >= m_tabs.size())
        return;
    if (m_tabs.size() == 1) {
        close(); // the last tab closes the window
        return;
    }
    if (index == m_activeTab)
        saveActiveTab();
    m_closedTabs.append(m_tabs.at(index));
    if (m_closedTabs.size() > 10)
        m_closedTabs.removeFirst();

    const bool wasActive = index == m_activeTab;
    m_switchingTabs = true;
    m_tabs.removeAt(index);
    m_tabBar->removeTab(index);
    m_switchingTabs = false;
    if (wasActive) {
        m_activeTab = -1; // nothing to save: it's gone
        switchToTab(m_tabBar->currentIndex());
    } else if (index < m_activeTab) {
        --m_activeTab;
    }
    updateTabBar();
}

void MainWindow::reopenClosedTab()
{
    if (m_closedTabs.isEmpty())
        return;
    const TabState tab = m_closedTabs.takeLast();
    openTab(tab.path, true);
    // Bring back its history and selection too.
    m_backHistory = tab.back;
    m_forwardHistory = tab.forward;
    m_tabs[m_activeTab].back = tab.back;
    m_tabs[m_activeTab].forward = tab.forward;
    updateActions();
    selectPaths(tab.selection);
    m_pendingScroll = tab.scroll;
}

void MainWindow::switchToTab(int index)
{
    if (m_switchingTabs || index < 0 || index >= m_tabs.size() || index == m_activeTab)
        return;
    saveActiveTab();
    m_activeTab = index;
    const TabState tab = m_tabs.at(index);

    if (tab.path != m_current) {
        if (!navigateTo(tab.path, false)) // its folder may be gone by now
            navigateTo(QDir::homePath(), false);
    } else {
        setWindowTitle(tr("%1 — Kaftilous").arg(tabTitle(m_current)));
    }
    m_backHistory = tab.back;
    m_forwardHistory = tab.forward;
    updateActions();
    if (!tab.selection.isEmpty())
        selectPaths(tab.selection);
    else
        m_view->selectionModel()->clearSelection();
    if (m_model->isLoading())
        m_pendingScroll = tab.scroll;
    else
        m_view->verticalScrollBar()->setValue(tab.scroll);
}
