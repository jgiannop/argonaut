#include "MainWindow.h"

#if defined(ARGONAUT_DBUS)
#include "ChooserBar.h"
#include "ChooserMode.h"
#endif

#include "AboutDialog.h"
#include "AnalyzeView.h"
#include "Access.h"
#include "BatchRenameDialog.h"
#include "CommandPalette.h"
#include "ConnectDialog.h"
#include "DesktopActions.h"
#include "OrganizerDialog.h"
#include "Remote.h"
#include "HelpDialog.h"
#include "InlineRename.h"
#include "MountActions.h"
#include "Naming.h"

#include "Format.h"

#include "Bookmarks.h"
#include "ConflictDialog.h"
#include "CompressDialog.h"
#include "ContentType.h"
#include "DirectoryModel.h"
#include "DiskUsageModel.h"
#include "FileFilterProxy.h"
#include "FileItemDelegate.h"
#include "FileOperations.h"
#include "HeaderBar.h"
#include "OpenWith.h"
#include "OpenWithDialog.h"
#include "OperationsPanel.h"
#include "PathBar.h"
#include "ExtensionsDialog.h"
#include "FadeOverlay.h"
#include "PreferencesDialog.h"
#include "RecentFiles.h"
#include "Starred.h"
#include "PropertiesDialog.h"
#include "QuickLook.h"
#include "Sidebar.h"
#include "StyleSheet.h"
#include "TabStrip.h"
#include "Thumbnailer.h"
#if defined(ARGONAUT_TERMINAL)
#include "TerminalWidget.h"
#endif
#include "Toast.h"
#include "Tools.h"
#include "Trash.h"

#include <QAction>
#include <functional>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileIconProvider>
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
#include <QMimeDatabase>
#include <QMessageBox>
#include <QMouseEvent>
#include <QInputDialog>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QRegularExpression>
#include <QScrollBar>
#include <QPainter>
#include <QCursor>
#include <QGuiApplication>
#include <QPointer>
#include <QScreen>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTabBar>
#include <QSignalBlocker>
#include <QTimer>
#include <QWheelEvent>
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


QStringList localPaths(const QMimeData *mime);

constexpr int ResizeMargin = 5;

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    m_csd = qEnvironmentVariableIsEmpty("ARGONAUT_NATIVE_DECORATIONS");
#if defined(Q_OS_MACOS)
    m_csd = false; // keep the native traffic-light title bar on macOS
#endif
    if (m_csd)
        setWindowFlag(Qt::FramelessWindowHint);

    resize(1150, 720);
    setWindowTitle("Argonaut");

    m_ops = new FileOperations(this);
    m_ops->setAskOnConflict(true);
    connect(m_ops, &FileOperations::jobFinished, this, &MainWindow::onJobFinished);
    // What an extension has to say ("Staged", "Background changed"): in the window in front.
    connect(Extensions::Host::instance(), &Extensions::Host::message, this, [this](const QString &, const QString &text) {
        if (!QApplication::activeWindow() || QApplication::activeWindow() == this)
            showMessage(text);
    });
    connect(m_ops, &FileOperations::conflictRaised, this, &MainWindow::askConflict);
    connect(m_ops, &FileOperations::undoChanged, this, &MainWindow::updateEditActions);
    connect(QApplication::clipboard(), &QClipboard::dataChanged, this,
            &MainWindow::updateEditActions);

    m_mounts = new MountActions(this);
    connect(m_mounts, &MountActions::message, this, [this](const QString &text) { showMessage(text); });
    connect(m_mounts, &MountActions::connected, this, [this](const QString &folder) { navigateTo(folder); });
    connect(m_mounts, &MountActions::mountsChanged, this, [this] { m_sidebar->refreshSoon(); });

    InlineRename::Host rename;
    rename.nameRect = [this](const QString &path) { return nameRectOf(path); };
    rename.showMessage = [this](const QString &text) { showMessage(text); };
    m_rename = new InlineRename(rename, this);
    connect(m_rename, &InlineRename::closed, this, [this] { m_pane->view->setFocus(); });
    connect(m_rename, &InlineRename::renamed, this, [this](const QString &from, const QString &to) {
        Starred::instance().renamed(from, to); // a star follows its item
        m_ops->recordUndo(FileJob::Kind::Move, {{from, to}}, tr("Rename"));
        selectPaths({to});
    });

    setupActions();
    setupUi();
    applyTheme(true);
    updateWindowState();
    loadSettings();

    // The sidebar rebuilds itself when bookmarks, drives or servers change.
    connect(m_sidebar, &Sidebar::refreshed, this, [this] {
        syncSidebarSelection();
        updateBookmarkAction();
    });

    // Watch the native window's mouse events: back/forward mouse buttons
    // anywhere in the window, plus edge resizing when frameless.
    winId();
    if (QWindow *handle = windowHandle())
        handle->installEventFilter(this);
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
    m_analyzeAct = new QAction(tr("Analyze Disk Usage"), this);
    m_analyzeAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_3));
    m_analyzeAct->setCheckable(true);
    connect(m_analyzeAct, &QAction::triggered, this, [this] { setAnalyzeMode(true); });
    viewGroup->addAction(m_analyzeAct);
    m_analyzeFilesAct = new QAction(tr("Show Files When Analyzing"), this);
    m_analyzeFilesAct->setCheckable(true);
    connect(m_analyzeFilesAct, &QAction::toggled, this, [this](bool show) { m_analyzeView->setShowFiles(show); });

    m_sidebarAct = new QAction(tr("Sidebar"), this);
    m_sidebarAct->setShortcut(QKeySequence(Qt::Key_F9));
    m_sidebarAct->setCheckable(true);
    m_sidebarAct->setChecked(true);
    connect(m_sidebarAct, &QAction::toggled, this, [this](bool on) { m_sidebarPane->setVisible(on); });

    // Sorting (also changed by clicking the list view's column headers).
    auto *sortGroup = new QActionGroup(this);
    const char *sortNames[] = {QT_TR_NOOP("By Name"), QT_TR_NOOP("By Size"), QT_TR_NOOP("By Type"),
                               QT_TR_NOOP("By Modification Date")};
    for (int column = 0; column < int(std::size(sortNames)); ++column) { // Owner… sort from the header
        auto *act = new QAction(tr(sortNames[column]), this);
        act->setCheckable(true);
        sortGroup->addAction(act);
        connect(act, &QAction::triggered, this,
                [this, column] { sortBy(column, m_pane->model->sortOrder()); });
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
            [this] { sortBy(m_pane->model->sortColumn(), Qt::AscendingOrder); });
    connect(m_sortDescAct, &QAction::triggered, this,
            [this] { sortBy(m_pane->model->sortColumn(), Qt::DescendingOrder); });

#if defined(ARGONAUT_TERMINAL)
    m_terminalAct = new QAction(tr("Terminal"), this);
    m_terminalAct->setCheckable(true);
    m_terminalAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_QuoteLeft)); // like VS Code
    connect(m_terminalAct, &QAction::toggled, this, [this](bool on) {
        if (on)
            showTerminal(m_pane->current);
        else
            hideTerminal();
    });
    m_openTerminalHereAct = new QAction(tr("Open in Terminal"), this);
    connect(m_openTerminalHereAct, &QAction::triggered, this, [this] {
        showTerminal(m_contextFolder.isEmpty() ? m_pane->current : m_contextFolder);
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
    connect(m_selectAllAct, &QAction::triggered, this, [this] {
        if (!m_analyzing)
            m_pane->view->selectAll();
    });

    m_selectMatchingAct = new QAction(tr("Select Items Matching…"), this);
    m_selectMatchingAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_S));
    connect(m_selectMatchingAct, &QAction::triggered, this, &MainWindow::selectMatching);

    m_invertSelectionAct = new QAction(tr("Invert Selection"), this);
    m_invertSelectionAct->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_I));
    connect(m_invertSelectionAct, &QAction::triggered, this, &MainWindow::invertSelection);

    m_newDocumentMenu = makeMenu();
    m_newDocumentMenu->setTitle(tr("New Document"));
    connect(m_newDocumentMenu, &QMenu::aboutToShow, this, &MainWindow::fillNewDocumentMenu);

    m_closeAct = new QAction(tr("Close Tab"), this);
    m_closeAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_W)); // the last tab closes the window
    connect(m_closeAct, &QAction::triggered, this, [this] { m_tabBar->closeActive(); });

    m_closeWindowAct = new QAction(tr("Close Window"), this);
    m_closeWindowAct->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_W));
    connect(m_closeWindowAct, &QAction::triggered, this, &QWidget::close);

    m_newTabAct = new QAction(tr("New Tab"), this);
    m_newTabAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_T));
    connect(m_newTabAct, &QAction::triggered, this, [this] { openTab(m_pane->current, true); });

    m_reopenTabAct = new QAction(tr("Reopen Closed Tab"), this);
    m_reopenTabAct->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T));
    connect(m_reopenTabAct, &QAction::triggered, this, [this] { m_tabBar->reopenClosed(); });

    m_openLocationAct = new QAction(tr("Open Item Location"), this);
    connect(m_openLocationAct, &QAction::triggered, this, [this] {
        const QStringList paths = selectedPaths();
        if (paths.isEmpty())
            return;
        const QString item = paths.first();
        if (navigateTo(QFileInfo(item).absolutePath()))
            selectPaths({item});
    });

    m_openInNewTabAct = new QAction(tr("Open in New Tab"), this);
    connect(m_openInNewTabAct, &QAction::triggered, this, [this] {
        for (const QModelIndex &index : selectedViewIndexes()) {
            const QModelIndex source = toSource(index);
            if (m_pane->model->isDir(source))
                openTab(m_pane->model->filePath(source), false);
        }
    });

    auto addTabSwitch = [this](const QList<QKeySequence> &keys, int step) {
        auto *act = new QAction(this);
        act->setShortcuts(keys);
        connect(act, &QAction::triggered, this, [this, step] { m_tabBar->step(step); });
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
        connect(act, &QAction::triggered, this, [this, n] { m_tabBar->activate(n - 1); });
        addAction(act);
    }

    m_quitAct = new QAction(tr("Quit"), this);
    m_quitAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Q));
    m_quitAct->setMenuRole(QAction::QuitRole);
    connect(m_quitAct, &QAction::triggered, this, &QApplication::closeAllWindows);

    m_copyLocationAct = new QAction(tr("Copy Location"), this);
    connect(m_copyLocationAct, &QAction::triggered, this, [this] {
        QApplication::clipboard()->setText(QDir::toNativeSeparators(m_pane->current));
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
    m_extensionsAct = new QAction(tr("Extensions"), this);
    m_extensionsAct->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_X));
    connect(m_extensionsAct, &QAction::triggered, this, [this] {
        static QPointer<ExtensionsDialog> dialog;
        if (!dialog) {
            dialog = new ExtensionsDialog(this);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            // Its folder opens in the window it was opened from.
            connect(dialog, &ExtensionsDialog::openFolderRequested, this, [this](const QString &folder) {
                QDir().mkpath(folder);
                navigateTo(folder);
                raise();
                activateWindow();
            });
            connect(dialog, &ExtensionsDialog::guideRequested, this, [this] { showGuide(QStringLiteral("extensions")); });
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

    m_propertiesAct = new QAction(tr("Properties"), this);
    m_propertiesAct->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_I),
                                   QKeySequence(Qt::ALT | Qt::Key_Return)});
    connect(m_propertiesAct, &QAction::triggered, this, [this] {
        QStringList paths = selectedPaths();
        if (paths.isEmpty())
            paths << m_pane->current; // nothing selected: the open folder
        (new PropertiesDialog(paths, this))->show();
    });

    m_compressAct = new QAction(tr("Compress…"), this);
    connect(m_compressAct, &QAction::triggered, this, &MainWindow::compressSelection);
    m_extractHereAct = new QAction(tr("Extract Here"), this);
    connect(m_extractHereAct, &QAction::triggered, this, [this] {
        const QStringList archives = selectedArchives();
        if (!archives.isEmpty())
            m_ops->extract(archives, m_pane->current);
    });
    m_extractToAct = new QAction(tr("Extract to…"), this);
    connect(m_extractToAct, &QAction::triggered, this, [this] {
        const QStringList archives = selectedArchives();
        if (archives.isEmpty())
            return;
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Extract to"), m_pane->current);
        if (!dir.isEmpty())
            m_ops->extract(archives, QDir::fromNativeSeparators(dir));
    });

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

    m_restoreAllAct = new QAction(tr("Restore All"), this);
    connect(m_restoreAllAct, &QAction::triggered, this, [this] {
        if (!m_pane->model->isTrash())
            return;
        QStringList all;
        for (int row = 0; row < m_pane->model->rowCount(); ++row)
            all << m_pane->model->filePath(m_pane->model->index(row, 0));
        if (!all.isEmpty())
            m_ops->restore(all); // back where they came from; never overwrites
    });

    m_undoAct = new QAction(tr("Undo"), this);
    m_undoAct->setShortcut(QKeySequence::Undo);
    connect(m_undoAct, &QAction::triggered, m_ops, &FileOperations::undo);
    m_previewAct = new QAction(tr("Preview"), this);
    // Shown as "Space" in menus; the key itself is handled in eventFilter, so
    // typing a name with spaces still works and line edits keep their spaces.
    m_previewAct->setShortcut(QKeySequence(Qt::Key_Space));
    m_previewAct->setShortcutContext(Qt::WidgetShortcut);
    connect(m_previewAct, &QAction::triggered, this, &MainWindow::toggleQuickLook);

    m_splitAct = new QAction(tr("Split View"), this);
    m_splitAct->setShortcut(QKeySequence(Qt::Key_F3));
    m_splitAct->setCheckable(true);
    connect(m_splitAct, &QAction::toggled, this, &MainWindow::setSplitView);
    m_copyToOtherAct = new QAction(tr("Copy to Other Pane"), this);
    m_copyToOtherAct->setShortcut(QKeySequence(Qt::Key_F5));
    connect(m_copyToOtherAct, &QAction::triggered, this, [this] { transferToOtherPane(false); });
    m_moveToOtherAct = new QAction(tr("Move to Other Pane"), this);
    m_moveToOtherAct->setShortcut(QKeySequence(Qt::Key_F6));
    connect(m_moveToOtherAct, &QAction::triggered, this, [this] { transferToOtherPane(true); });

    m_starAct = new QAction(tr("Star"), this);
    connect(m_starAct, &QAction::triggered, this, [this] {
        const QStringList paths = selectedPaths();
        if (paths.isEmpty())
            return;
        const bool all = selectionIsStarred(); // unstar only if everything is starred
        Starred::instance().setStarred(paths, !all);
        showMessage(all ? (paths.size() == 1 ? tr("Unstarred") : tr("%1 items unstarred").arg(paths.size()))
                        : (paths.size() == 1 ? tr("Starred") : tr("%1 items starred").arg(paths.size())));
    });
    m_removeRecentAct = new QAction(tr("Remove from Recent"), this);
    connect(m_removeRecentAct, &QAction::triggered, this, [this] {
        const QStringList paths = selectedPaths();
        if (!paths.isEmpty() && RecentFiles::remove(paths))
            m_pane->model->refresh(); // also picked up by the watcher; this is just sooner
    });

    m_redoAct = new QAction(tr("Redo"), this);
    m_redoAct->setShortcuts({QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z), QKeySequence(Qt::CTRL | Qt::Key_Y)});
    connect(m_redoAct, &QAction::triggered, m_ops, &FileOperations::redo);

    m_zoomInAct = new QAction(tr("Zoom In"), this);
    m_zoomInAct->setShortcuts({QKeySequence::ZoomIn, QKeySequence(Qt::CTRL | Qt::Key_Equal)});
    connect(m_zoomInAct, &QAction::triggered, this, [this] { zoom(+1); });
    m_zoomOutAct = new QAction(tr("Zoom Out"), this);
    m_zoomOutAct->setShortcuts({QKeySequence::ZoomOut, QKeySequence(Qt::CTRL | Qt::Key_Underscore)});
    connect(m_zoomOutAct, &QAction::triggered, this, [this] { zoom(-1); });
    m_zoomResetAct = new QAction(tr("Normal Size"), this);
    m_zoomResetAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));
    connect(m_zoomResetAct, &QAction::triggered, this, [this] { zoom(0); });

    m_aboutAct = new QAction(tr("About Argonaut"), this);
    m_aboutAct->setMenuRole(QAction::AboutRole);
    connect(m_aboutAct, &QAction::triggered, this, [this] {
        static QPointer<AboutDialog> dialog;
        if (!dialog)
            dialog = new AboutDialog(this);
        dialog->show();
        dialog->raise();
        dialog->activateWindow();
    });

    // Search also inside files (the "Inside files" button while searching).
    m_searchContentsAct = new QAction(tr("Search Inside Files"), this);
    m_searchContentsAct->setCheckable(true);
    m_searchContentsAct->setToolTip(tr("Also find files by the words inside them: text files, and documents "
                                       "in the folders GNOME's search indexes"));
    m_searchContentsAct->setChecked(QSettings().value("search/contents", false).toBool());
    connect(m_searchContentsAct, &QAction::toggled, this, [this](bool on) {
        QSettings().setValue("search/contents", on);
        for (int i = 0; i < m_paneCount; ++i) {
            m_panes[i].model->setSearchContents(on);
        }
        updateFloatingBar();
    });
    m_connectAct = new QAction(tr("Connect to Server…"), this);
    connect(m_connectAct, &QAction::triggered, this, [this] {
        ConnectDialog dialog(this);
        if (dialog.exec() == QDialog::Accepted)
            m_mounts->connectToServer(dialog.address());
    });

    m_organizeAct = new QAction(tr("Organize with Claude…"), this);
    connect(m_organizeAct, &QAction::triggered, this, &MainWindow::organizeWithClaude);

    m_paletteAct = new QAction(tr("Command Palette…"), this);
    m_paletteAct->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P));
    connect(m_paletteAct, &QAction::triggered, this, &MainWindow::showCommandPalette);

    m_guideAct = new QAction(tr("User Guide"), this);
    m_guideAct->setShortcut(QKeySequence::HelpContents); // F1
    connect(m_guideAct, &QAction::triggered, this, [this] { showGuide(QStringLiteral("start")); });

    m_shortcutsAct = new QAction(tr("Keyboard Shortcuts"), this);
    m_shortcutsAct->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_Question),
                                  QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Slash)});
    connect(m_shortcutsAct, &QAction::triggered, this, [this] { showGuide(QStringLiteral("shortcuts")); });

    addActions({m_backAct, m_forwardAct, m_upAct, m_homeAct, m_locationAct, m_hiddenAct,
                m_searchAct, m_gridAct, m_listAct, m_analyzeAct, m_analyzeFilesAct, m_sidebarAct, m_prefsAct, m_extensionsAct, m_cutAct, m_copyAct,
                m_pasteAct, m_trashAct, m_undoAct, m_redoAct, m_zoomInAct, m_zoomOutAct,
                m_zoomResetAct, m_newFolderAct, m_newFileAct, m_renameAct,
                m_selectAllAct, m_selectMatchingAct, m_invertSelectionAct, m_closeAct, m_quitAct, m_deleteAct, m_bookmarkAct,
                m_newTabAct, m_closeWindowAct, m_reopenTabAct, m_propertiesAct, m_compressAct,
                m_extractHereAct, m_extractToAct, m_openLocationAct, m_starAct, m_removeRecentAct,
                m_splitAct, m_copyToOtherAct, m_moveToOtherAct, m_guideAct, m_shortcutsAct, m_paletteAct});
    if (m_terminalAct)
        addAction(m_terminalAct);
}

void MainWindow::addWindowButtons(QHBoxLayout *row)
{
    m_minButton = makeButton(tr("Minimize"), "win");
    connect(m_minButton, &QToolButton::clicked, this, &QWidget::showMinimized);
    m_maxButton = makeButton(tr("Maximize"), "win");
    connect(m_maxButton, &QToolButton::clicked, this,
            [this] { isMaximized() ? showNormal() : showMaximized(); });
    m_closeButton = makeButton(tr("Close"), "win");
    connect(m_closeButton, &QToolButton::clicked, this, &QWidget::close);
    row->addSpacing(10);
    for (QToolButton *b : {m_minButton, m_maxButton, m_closeButton}) {
        row->addWidget(b, 0, Qt::AlignVCenter);
        if (b != m_closeButton)
            row->addSpacing(6);
    }
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

    m_thumbnailer = new Thumbnailer(this);

    m_root = new QWidget(this);
    m_root->setObjectName("root");
    m_root->setAttribute(Qt::WA_StyledBackground);
    auto *rootLayout = new QVBoxLayout(m_root);
    rootLayout->setSpacing(0);

#if defined(Q_OS_MACOS)
    setMenuBar(qobject_cast<QMenuBar *>(buildMenuBar())); // global macOS menu bar
#else
    // The menu bar row also holds the window buttons, at its right end.
    auto *menuRow = new QWidget(m_root);
    menuRow->setObjectName("menuRow");
    menuRow->setAttribute(Qt::WA_StyledBackground);
    auto *menuLayout = new QHBoxLayout(menuRow);
    menuLayout->setContentsMargins(0, 0, 8, 0);
    menuLayout->setSpacing(0);
    menuLayout->addWidget(buildMenuBar(), 1); // its empty space drags the window
    if (m_csd)
        addWindowButtons(menuLayout);
    // The row drags too: the menu bar can be lower than the row (and is only
    // a thin band when its menus are hidden, as in an Open/Save window).
    m_menuRow = menuRow;
    menuRow->installEventFilter(this);
    rootLayout->addWidget(menuRow, 0);
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

    m_titleLabel = new QLabel(tr("Argonaut"), header);
    m_titleLabel->setObjectName("title");
    m_titleLabel->setAlignment(Qt::AlignCenter);

    m_menuButton = makeButton(tr("Main menu"));
    m_menuButton->setPopupMode(QToolButton::InstantPopup);
    QMenu *mainMenu = makeMenu();
    mainMenu->addAction(m_undoAct);
    mainMenu->addAction(m_redoAct);
    mainMenu->addSeparator();
    mainMenu->addAction(m_hiddenAct);
    mainMenu->addSeparator();
    mainMenu->addAction(m_extensionsAct);
    mainMenu->addAction(m_prefsAct);
    mainMenu->addAction(m_guideAct);
    mainMenu->addAction(m_aboutAct);
    m_menuButton->setMenu(mainMenu);

    header->row()->addWidget(m_searchButton);
    header->row()->addWidget(m_titleLabel, 1);
    header->row()->addWidget(m_menuButton);
    layout->addWidget(header);

    m_sidebar = new Sidebar(pane);
    m_sidebar->viewport()->setAcceptDrops(true); // drop onto places and drives
    m_sidebar->viewport()->installEventFilter(this);
    m_sidebar->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_sidebar, &QWidget::customContextMenuRequested, this, &MainWindow::showSidebarMenu);
    connect(m_sidebar, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        const QString path = Sidebar::pathOf(item);
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
    m_contentLayout = layout; // an Open/Save window adds its bar at the bottom

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
    m_searchEdit->setPlaceholderText(tr("Search this folder and its sub-folders"));
    m_searchEdit->installEventFilter(this);
    connect(m_searchEdit, &QLineEdit::textChanged, this, &MainWindow::updateFilter);
    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(150);
    connect(m_searchTimer, &QTimer::timeout, this, [this] {
        m_pane->model->setSearch(m_searchEdit->text().simplified());
        updateFloatingBar();
    });

    // Search also inside files: next to the field while searching.
    auto *searchBox = new QWidget(header);
    auto *searchRow = new QHBoxLayout(searchBox);
    searchRow->setContentsMargins(0, 0, 0, 0);
    searchRow->setSpacing(4);
    searchRow->addWidget(m_searchEdit, 1);
    auto *contentsButton = new QToolButton(searchBox);
    contentsButton->setObjectName("searchOption");
    m_searchContentsAct->setIconText(tr("Inside files")); // the button's label; menus show the full text
    contentsButton->setDefaultAction(m_searchContentsAct);
    contentsButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    contentsButton->setFixedHeight(32);
    contentsButton->setCursor(Qt::PointingHandCursor);
    searchRow->addWidget(contentsButton);

    // Filters: by type and by date; they also search on their own.
    m_filterButton = new QToolButton(searchBox);
    m_filterButton->setObjectName("searchOption");
    m_filterButton->setFixedHeight(32);
    m_filterButton->setPopupMode(QToolButton::InstantPopup);
    m_filterButton->setCursor(Qt::PointingHandCursor);
    QMenu *filters = makeMenu();
    filters->addSection(tr("Type"));
    m_typeFilters = new QActionGroup(this);
    const QList<std::pair<DirectoryModel::TypeFilter, QString>> types = {
        {DirectoryModel::TypeFilter::Any, tr("Any type")},       {DirectoryModel::TypeFilter::Folders, tr("Folders")},
        {DirectoryModel::TypeFilter::Documents, tr("Documents")}, {DirectoryModel::TypeFilter::Images, tr("Images")},
        {DirectoryModel::TypeFilter::Audio, tr("Audio")},         {DirectoryModel::TypeFilter::Video, tr("Video")},
        {DirectoryModel::TypeFilter::Archives, tr("Archives")},   {DirectoryModel::TypeFilter::Text, tr("Text and code")}};
    for (const auto &[type, label] : types) {
        QAction *a = filters->addAction(label);
        a->setCheckable(true);
        a->setData(int(type));
        a->setChecked(type == DirectoryModel::TypeFilter::Any);
        m_typeFilters->addAction(a);
    }
    filters->addSection(tr("Changed"));
    m_dateFilters = new QActionGroup(this);
    const QList<std::pair<DirectoryModel::DateFilter, QString>> dates = {
        {DirectoryModel::DateFilter::Any, tr("Any time")},        {DirectoryModel::DateFilter::Today, tr("Today")},
        {DirectoryModel::DateFilter::Week, tr("In the last 7 days")}, {DirectoryModel::DateFilter::Month, tr("In the last 30 days")},
        {DirectoryModel::DateFilter::Year, tr("This year")},      {DirectoryModel::DateFilter::OlderThanYear, tr("Over a year ago")}};
    for (const auto &[date, label] : dates) {
        QAction *a = filters->addAction(label);
        a->setCheckable(true);
        a->setData(int(date));
        a->setChecked(date == DirectoryModel::DateFilter::Any);
        m_dateFilters->addAction(a);
    }
    connect(m_typeFilters, &QActionGroup::triggered, this, &MainWindow::applySearchFilters);
    connect(m_dateFilters, &QActionGroup::triggered, this, &MainWindow::applySearchFilters);
    m_filterButton->setMenu(filters);
    m_filterButton->setText(tr("Filters"));
    searchRow->addWidget(m_filterButton);

    m_locationStack = new QStackedWidget(header);
    m_locationStack->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_locationStack->setFixedHeight(32);
    m_locationStack->addWidget(m_pathBar);
    m_locationStack->addWidget(searchBox);
    row->addWidget(m_locationStack, 1);
    row->addSpacing(4);

    if (m_terminalAct) {
        m_terminalButton = makeButton(tr("Terminal (Ctrl+`)"));
        m_terminalButton->setCheckable(true);
        connect(m_terminalButton, &QToolButton::toggled, m_terminalAct, &QAction::setChecked);
        connect(m_terminalAct, &QAction::toggled, m_terminalButton, &QToolButton::setChecked);
        row->addWidget(m_terminalButton);
    }
    m_analyzeButton = makeButton(tr("Analyze disk usage (Ctrl+3)"));
    m_analyzeButton->setCheckable(true);
    connect(m_analyzeButton, &QToolButton::clicked, this, [this](bool on) {
        on ? setAnalyzeMode(true) : setGridMode(m_pane->grid);
    });
    row->addWidget(m_analyzeButton);
    m_splitButton = makeButton(tr("Split view (F3)"));
    m_splitButton->setCheckable(true);
    connect(m_splitButton, &QToolButton::clicked, m_splitAct, &QAction::setChecked);
    row->addWidget(m_splitButton);
    m_viewButton = makeButton(QString());
    connect(m_viewButton, &QToolButton::clicked, this, [this] {
        setGridMode(m_analyzing ? m_pane->grid : !m_pane->grid); // from Analyze: back to the file view
    });
    row->addWidget(m_viewButton);

    layout->addWidget(header);

    // The first file pane (a second one comes with the split view).
    m_paneSplitter = new QSplitter(Qt::Horizontal, pane);
    m_paneSplitter->setObjectName("paneSplitter");
    m_paneSplitter->setChildrenCollapsible(false);
    m_paneSplitter->setHandleWidth(1);
    m_paneSplitter->addWidget(buildPane(m_paneSplitter, 0));
    m_springTimer = new QTimer(this);
    m_springTimer->setSingleShot(true);
    m_springTimer->setInterval(800);
    connect(m_springTimer, &QTimer::timeout, this, [this] {
        const QString folder = m_springPath;
        m_springPath.clear();
        if (folder.isEmpty())
            return;
        springOver(folder, m_springPane);
    });
    m_dragScrollTimer = new QTimer(this);
    m_dragScrollTimer->setInterval(40);
    connect(m_dragScrollTimer, &QTimer::timeout, this, [this] {
        if (m_dragScrollStep == 0) {
            m_dragScrollTimer->stop();
            return;
        }
        if (m_dragScrollView) { // the pane being dragged over (maybe not the active one)
            QScrollBar *bar = m_dragScrollView->verticalScrollBar();
            bar->setValue(bar->value() + m_dragScrollStep);
        }
    });


    // Analyze: its own view, model and selection; never m_pane->view.
    m_analyzeView = new AnalyzeView(m_pane->viewStack);
    connect(m_analyzeView, &QTreeView::customContextMenuRequested, this, &MainWindow::showAnalyzeMenu);
    connect(m_analyzeView, &AnalyzeView::statusChanged, this, &MainWindow::updateFloatingBar);
    // A folder opens in the Analyze view, a file opens in its app (with the
    // usual real-type checks).
    connect(m_analyzeView, &AnalyzeView::fileActivated, this, &MainWindow::openFile);
    connect(m_analyzeView, &AnalyzeView::folderActivated, this, [this](const QString &path) {
        if (path != m_pane->current)
            navigateTo(path);
    });
    connect(m_analyzeView, &AnalyzeView::trashRequested, this, [this](const QString &path) {
        if (path != m_pane->current) // not the folder being analysed itself
            m_ops->trash({path});
    });
    m_pane->viewStack->addWidget(m_analyzeView); // in the first pane only

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
    m_trashRestoreAllButton = new QToolButton(m_trashBar);
    m_trashRestoreAllButton->setObjectName("barButton");
    m_trashRestoreAllButton->setDefaultAction(m_restoreAllAct);
    for (QToolButton *b : {m_trashRestoreButton, m_trashRestoreAllButton, m_trashEmptyButton})
        b->setFocusPolicy(Qt::NoFocus);
    trashRow->addWidget(trashText, 1);
    trashRow->addWidget(m_trashRestoreButton);
    trashRow->addWidget(m_trashRestoreAllButton);
    trashRow->addWidget(m_trashEmptyButton);
    m_trashBar->hide();
    TabStrip::Host tabs;
    tabs.prepare = [this] {
        if (paneIndex() != 0)
            activatePane(0); // tabs belong to the first pane
    };
    tabs.capture = [this] { return captureTab(); };
    tabs.restore = [this](const TabStrip::Tab &tab) { restoreTab(tab); };
    tabs.title = [this](const QString &path) { return titleFor(path); };
    tabs.closeWindow = [this] { close(); };
    m_tabBar = new TabStrip(tabs, pane);
    m_tabBar->setAcceptDrops(true); // drop files onto a tab's folder
    m_tabBar->installEventFilter(this);
    layout->addWidget(m_tabBar);
    layout->addWidget(m_trashBar);

    m_contentSplitter = new QSplitter(Qt::Vertical, pane);
    m_contentSplitter->setObjectName("contentSplitter");
    m_contentSplitter->setChildrenCollapsible(false);
    m_contentSplitter->setHandleWidth(1);
    m_contentSplitter->addWidget(m_paneSplitter);
#if defined(ARGONAUT_TERMINAL)
    m_contentSplitter->addWidget(buildTerminalPanel());
#endif
    m_contentSplitter->setStretchFactor(0, 1);
    // The preview pane (Space) is added at the right on first use.
    m_previewSplitter = new QSplitter(Qt::Horizontal, pane);
    m_previewSplitter->setObjectName("previewSplitter");
    m_previewSplitter->setChildrenCollapsible(false);
    m_previewSplitter->setHandleWidth(1);
    m_previewSplitter->addWidget(m_contentSplitter);
    m_previewSplitter->setStretchFactor(0, 1);
    layout->addWidget(m_previewSplitter, 1);


    m_opsPanel = new OperationsPanel(m_ops, m_pane->viewStack);
    m_toast = new Toast(m_pane->viewStack);
    connect(m_opsPanel, &OperationsPanel::layoutChanged, this, [this] {
        m_toast->setAvoidRect(m_opsPanel->isVisible() ? m_opsPanel->geometry() : QRect());
    });
    updateFilter();
    updateEditActions();

    setGridMode(true);
    return pane;
}


// ---------------------------------------------------------------- panes ---

// While alive, the window works on pane `index` instead of the user's pane
// (m_pane points at it): for handling what happened in the other pane (its
// folder loaded, a drop into it…) with the same code. Header, title and
// actions are left alone meanwhile (inBackground()).
class MainWindow::PaneScope
{
public:
    enum Building { Built, BeingBuilt }; // BeingBuilt: the pane isn't counted in m_paneCount yet

    PaneScope(MainWindow *window, int index, Building building = Built)
        : m_window(window), m_previous(window->m_pane)
    {
        const int panes = building == BeingBuilt ? int(std::size(window->m_panes)) : window->m_paneCount;
        if (index >= 0 && index < panes)
            m_window->m_pane = &m_window->m_panes[index];
    }
    ~PaneScope() { m_window->m_pane = m_previous; }
    PaneScope(const PaneScope &) = delete;
    PaneScope &operator=(const PaneScope &) = delete;

private:
    MainWindow *m_window;
    Pane *m_previous;
};

int MainWindow::paneIndex() const
{
    return int(m_pane - m_panes);
}

bool MainWindow::inBackground() const
{
    return m_pane != &m_panes[m_activePane];
}

int MainWindow::paneOf(const QObject *object) const
{
    for (int i = 0; i < m_paneCount; ++i) {
        const Pane &p = m_panes[i];
        if (object == p.iconView || object == p.detailView || object == p.viewStack || object == p.caption
            || (p.iconView && object == p.iconView->viewport())
            || (p.detailView && object == p.detailView->viewport()))
            return i;
    }
    return -1;
}

void MainWindow::activatePane(int index)
{
    if (index == m_activePane || index < 0 || index >= m_paneCount || inBackground())
        return;
    m_rename->cancel();
    setSearchActive(false); // the search field belongs to the active pane
    if (m_analyzing)
        setGridMode(m_pane->grid); // Analyze is the first pane's
    m_activePane = index;
    m_pane = &m_panes[index];
    refreshChrome();
    if (quickLookVisible())
        updateQuickLook(); // the preview shows the pane you are in
    if (!m_pane->view->hasFocus())
        m_pane->view->setFocus();
}

QString MainWindow::titleFor(const QString &path) const
{
    switch (DirectoryModel::placeOf(path)) {
    case DirectoryModel::Place::Recent:
        return tr("Recent");
    case DirectoryModel::Place::Starred:
        return tr("Starred");
    case DirectoryModel::Place::Folder:
        break;
    }
    if (Trash::isFilesDir(path))
        return tr("Trash");
    if (const Remote::Mount server = Remote::mountOf(path); server.localPath == path)
        return server.name; // the top of a server, not "sftp:host=…"
    const QString name = QFileInfo(path).fileName();
    return name.isEmpty() ? QDir::toNativeSeparators(path) : name;
}

// The header, title, sidebar and actions show the active pane.
void MainWindow::refreshChrome()
{
    if (inBackground())
        return;
    m_trashBar->setVisible(m_pane->model->isTrash());
    m_pathBar->setPath(m_pane->current);
    updateGitBadge();
#if defined(ARGONAUT_DBUS)
    if (m_chooser) {
        setWindowTitle(m_chooser->windowTitle());
        m_titleLabel->setText(m_chooser->shortTitle()); // short: apps' titles can be long
    } else
#endif
        setWindowTitle(tr("%1 — Argonaut").arg(titleFor(m_pane->current)));
    syncSidebarSelection();
    updateActions();
    updateEditActions();
    updateBookmarkAction();
    updateSortActions();
    updateViewChrome();
    updatePaneCaptions();
    updateFloatingBar();
}

// Ctrl+Shift+P: every enabled command of the menu bar, labelled with its
// menu, plus the ones that only live in right-click menus.
void MainWindow::showCommandPalette()
{
    if (!m_palette)
        m_palette = new CommandPalette(this);
    updateEditActions();
    m_starAct->setText(selectionIsStarred() ? tr("Unstar") : tr("Star"));

    QList<CommandPalette::Command> commands;
    QSet<QAction *> seen{m_paletteAct};
    auto clean = [](QString text) {
        text.remove(QLatin1Char('&'));
        text.remove(QChar(0x2026));
        return text.trimmed();
    };
    std::function<void(QMenu *, const QString &)> walk = [&](QMenu *menu, const QString &where) {
        for (QAction *act : menu->actions()) {
            if (act->isSeparator() || !act->isVisible())
                continue;
            if (QMenu *sub = act->menu()) {
                if (sub == m_newDocumentMenu) {
                    fillNewDocumentMenu(); // its items are made when it opens
                    if (!act->isEnabled())
                        continue;
                }
                walk(sub, where + QStringLiteral(" › ") + clean(sub->title()));
                continue;
            }
            if (!act->isEnabled() || act->text().isEmpty() || seen.contains(act))
                continue;
            seen.insert(act);
            commands.append({clean(act->text()), where, act});
        }
    };
    for (QAction *top : m_menuBar->actions()) {
        if (QMenu *menu = top->menu())
            walk(menu, clean(menu->title()));
    }
    const QString selection = tr("Selection");
    for (QAction *act : {m_starAct, m_removeRecentAct, m_openInNewTabAct, m_openLocationAct, m_openTerminalHereAct}) {
        if (!act || seen.contains(act) || !act->isEnabled() || !act->isVisible())
            continue;
        if (act == m_removeRecentAct && m_pane->model->place() != DirectoryModel::Place::Recent)
            continue;
        if (act == m_openLocationAct && !(m_pane->model->isSearch() || m_pane->model->isVirtual()))
            continue;
        if ((act == m_starAct || act == m_openInNewTabAct || act == m_openLocationAct) && selectedPaths().isEmpty())
            continue;
        seen.insert(act);
        commands.append({clean(act->text()), selection, act});
    }
    m_contextFolder = m_pane->current; // "Open in Terminal" acts on the open folder
    m_palette->open(commands);
}

// Help → User Guide / Keyboard Shortcuts. One window; the shortcuts page is
// made from these actions, so it always matches the menus.
void MainWindow::showGuide(const QString &topic)
{
    static QPointer<HelpDialog> dialog;
    if (!dialog) {
        using Group = HelpDialog::ShortcutGroup;
        auto row = [](QAction *act, const QString &name = QString()) {
            QString text = name.isEmpty() ? act->text() : name;
            text.remove(QLatin1Char('&')).remove(QChar(0x2026)); // "Rename…" -> "Rename"
            return HelpDialog::Shortcut{text, act->shortcuts(), QString()};
        };
        auto note = [](const QString &name, const QString &keys) {
            return HelpDialog::Shortcut{name, {}, keys};
        };
        QList<Group> groups;
        groups.append({tr("Moving Around"),
                       {row(m_backAct, tr("Back")), row(m_forwardAct, tr("Forward")), row(m_upAct, tr("Up one folder")),
                        row(m_homeAct, tr("Home folder")), row(m_locationAct, tr("Type a path")),
                        row(m_searchAct, tr("Search this folder and sub-folders")),
                        note(tr("Open the selected item"), QStringLiteral("[[Enter]]")),
                        note(tr("Jump to a name"), tr("type its first letters"))}});
        groups.append({tr("Views"),
                       {row(m_gridAct, tr("Grid")), row(m_listAct, tr("List")), row(m_analyzeAct, tr("Disk usage")),
                        row(m_hiddenAct, tr("Show hidden files")), row(m_sidebarAct, tr("Show sidebar")),
                        row(m_zoomInAct, tr("Zoom in")), row(m_zoomOutAct, tr("Zoom out")),
                        row(m_zoomResetAct, tr("Normal size")),
                        note(tr("Zoom with the mouse"), tr("[[Ctrl]] + mouse wheel")),
                        note(tr("Quick Look (preview)"), QStringLiteral("[[Space]]")),
                        row(m_splitAct, tr("Split view")), row(m_copyToOtherAct, tr("Copy to the other side")),
                        row(m_moveToOtherAct, tr("Move to the other side"))}});
        QList<HelpDialog::Shortcut> files = {
            row(m_selectAllAct), row(m_selectMatchingAct), row(m_invertSelectionAct),
            row(m_copyAct), row(m_cutAct), row(m_pasteAct), row(m_renameAct, tr("Rename (several items: batch rename)")),
            row(m_newFolderAct), row(m_newFileAct), row(m_trashAct), row(m_deleteAct, tr("Delete permanently")),
            row(m_undoAct, tr("Undo")), row(m_redoAct, tr("Redo")), row(m_propertiesAct),
            row(m_bookmarkAct, tr("Bookmark this folder"))};
        groups.append({tr("Files"), files});
        QList<HelpDialog::Shortcut> windows = {
            row(m_newTabAct), row(m_closeAct), row(m_reopenTabAct),
            note(tr("Next / previous tab"), QStringLiteral("[[Ctrl+Tab]] / [[Ctrl+Shift+Tab]]")),
            note(tr("Go to tab 1…9"), QStringLiteral("[[Alt+1]] … [[Alt+9]]")),
            row(m_closeWindowAct), row(m_quitAct)};
        if (m_terminalAct)
            windows.prepend(row(m_terminalAct, tr("Terminal")));
        groups.append({tr("Tabs and Windows"), windows});
        groups.append({tr("Help"), {row(m_guideAct), row(m_shortcutsAct), row(m_prefsAct), row(m_extensionsAct)}});

        dialog = new HelpDialog(groups, this);
    }
    dialog->showTopic(topic);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void MainWindow::updateGitBadge()
{
    if (inBackground() || !m_pathBar)
        return;
    const QString folder = m_pane->model ? m_pane->model->extensionsFolder() : QString();
    const Extensions::Info info = folder.isEmpty() ? Extensions::Info() : Extensions::Host::instance()->info(folder);
    m_pathBar->setExtensionInfo(info.text, info.tooltip, info.icon);
}

void MainWindow::refreshGitAll()
{
    Extensions::Host::instance()->refreshAll();
}

void MainWindow::applyExpandFolders()
{
    for (int i = 0; i < m_paneCount; ++i) {
        m_panes[i].model->setTreeEnabled(m_expandFolders && !m_panes[i].grid);
    }
    if (m_pane->detailView)
        m_pane->detailView->viewport()->update();
}

void MainWindow::setExpandFoldersEverywhere(bool on)
{
    QSettings().setValue("view/expandFolders", on);
    for (QWidget *widget : QApplication::topLevelWidgets()) {
        if (auto *window = qobject_cast<MainWindow *>(widget)) {
            window->m_expandFolders = on;
            window->applyExpandFolders();
        }
    }
}

// Expandable folders: → opens the current folder, ← closes it, or goes to
// the folder an item is in (like Nautilus).
bool MainWindow::handleTreeKey(QKeyEvent *key)
{
    if (!m_pane->model->isTreeActive() || key->modifiers() != Qt::NoModifier
        || (key->key() != Qt::Key_Right && key->key() != Qt::Key_Left))
        return false;
    const QModelIndex current = m_pane->detailView->currentIndex().siblingAtColumn(0);
    if (!current.isValid())
        return false;
    const QString path = m_pane->model->filePath(toSource(current));
    if (key->key() == Qt::Key_Right) {
        if (current.data(DirectoryModel::ExpandableRole).toBool() && !m_pane->model->isExpanded(path))
            m_pane->model->expand(path);
        return true; // never scroll sideways
    }
    if (m_pane->model->isExpanded(path)) {
        m_pane->model->collapse(path);
        return true;
    }
    if (current.data(DirectoryModel::TreeDepthRole).toInt() > 0) {
        const QModelIndex parent = toView(m_pane->model->indexForPath(QFileInfo(path).absolutePath()));
        if (parent.isValid()) {
            m_pane->detailView->selectionModel()->setCurrentIndex(
                parent, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            m_pane->detailView->scrollTo(parent);
        }
    }
    return true;
}

void MainWindow::applyListColumns()
{
    for (int i = 0; i < m_paneCount; ++i) {
        QTreeView *view = m_panes[i].detailView;
        QHeaderView *header = view->header();
        for (int column : {DirectoryModel::OwnerColumn, DirectoryModel::GroupColumn,
                           DirectoryModel::PermissionsColumn}) {
            const bool shown = m_listColumns.contains(QString::number(column));
            header->setSectionHidden(column, !shown);
            if (shown && header->sectionSize(column) < 40)
                header->resizeSection(column, 110); // a header saved before these columns existed
        }
    }
}

void MainWindow::showListHeaderMenu(QHeaderView *header, const QPoint &pos)
{
    if (!Access::hasOwners())
        return; // no Unix owners or permission bits to show
    QMenu *menu = makeMenu();
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->addSection(tr("Show Columns"));
    for (int column : {DirectoryModel::OwnerColumn, DirectoryModel::GroupColumn,
                       DirectoryModel::PermissionsColumn}) {
        QAction *act = menu->addAction(m_pane->model->headerData(column, Qt::Horizontal).toString());
        act->setCheckable(true);
        const QString key = QString::number(column);
        act->setChecked(m_listColumns.contains(key));
        connect(act, &QAction::toggled, this, [this, key](bool on) {
            m_listColumns.removeAll(key);
            if (on)
                m_listColumns.append(key);
            applyListColumns();
        });
    }
    menu->popup(header->viewport()->mapToGlobal(pos));
}

bool MainWindow::canChangeSelection() const
{
    if (m_pane->model->isTrash())
        return true; // the Trash's own folders
    QHash<QString, Access::FolderAccess> folders;
    for (const QModelIndex &viewIndex : selectedViewIndexes()) {
        const QModelIndex source = toSource(viewIndex);
        const QString path = m_pane->model->filePath(source);
        const int slash = int(path.lastIndexOf(QLatin1Char('/')));
        const QString dir = slash <= 0 ? QStringLiteral("/") : path.left(slash);
        if (!Access::canChangeItemsIn(dir, source.data(DirectoryModel::OwnerIdRole).toUInt(), &folders))
            return false;
    }
    return true;
}

void MainWindow::fitListColumns()
{
    if (!m_pane->detailView)
        return;
    const int width = m_pane->viewStack->width();
    QHeaderView *header = m_pane->detailView->header();
    header->setSectionHidden(DirectoryModel::TypeColumn, width < 640);
    header->setSectionHidden(DirectoryModel::ModifiedColumn, width < 460);
}

// Split view: a slim caption over each pane with its folder; the active
// pane's is highlighted.
void MainWindow::updatePaneCaptions()
{
    for (int i = 0; i < m_paneCount; ++i) {
        QLabel *caption = m_panes[i].caption;
        if (!caption)
            continue;
        caption->setVisible(m_split);
        const QString path = m_panes[i].current;
        caption->setText(titleFor(path));
        caption->setToolTip(QDir::toNativeSeparators(path));
        if (inBackground())
            continue; // only texts change; which pane is active didn't
        const bool active = i == m_activePane;
        if (caption->property("active").toBool() != active) {
            caption->setProperty("active", active); // styled by QLabel#paneCaption[active="true"]
            caption->style()->unpolish(caption);
            caption->style()->polish(caption);
        }
    }
}

void MainWindow::ensureSecondPane()
{
    if (m_paneCount > 1)
        return;
    const Pane &first = m_panes[0];
    {
        PaneScope scope(this, 1, PaneScope::BeingBuilt);
        // It starts like the first pane (view, zoom, order, folder), with a
        // history of its own.
        m_pane->grid = first.grid;
        m_pane->gridZoom = first.gridZoom;
        m_pane->listZoom = first.listZoom;
        QWidget *container = buildPane(m_paneSplitter, 1);
        m_paneSplitter->addWidget(container);
        m_pane->iconView->setGridSize(m_pane->gridDelegate->gridCellSize(m_pane->iconView->fontMetrics()));
        sortBy(first.detailView->header()->sortIndicatorSection(), first.detailView->header()->sortIndicatorOrder());
        navigateTo(first.current, false);
    }
    const int half = qMax(1, m_paneSplitter->width() / 2);
    m_paneSplitter->setSizes({half, half});
}

void MainWindow::setSplitView(bool on)
{
    if (on == m_split)
        return;
    if (on) {
        if (m_analyzing)
            setGridMode(m_pane->grid);
        ensureSecondPane();
        m_split = true;
        m_panes[1].container->show();
        activatePane(1); // the new pane is where you'll go next
    } else {
        if (paneIndex() != 0)
            activatePane(0);
        m_split = false;
        m_panes[1].container->hide();
    }
    const QSignalBlocker block(m_splitAct);
    m_splitAct->setChecked(on);
    if (m_splitButton)
        m_splitButton->setChecked(on);
    updatePaneCaptions();
    updateEditActions();
}

QString MainWindow::otherPaneFolder() const
{
    if (!m_split || m_paneCount < 2)
        return {};
    const QString other = m_panes[1 - paneIndex()].current;
    if (other.isEmpty() || DirectoryModel::isVirtualPath(other) || Trash::isFilesDir(other)
        || !QFileInfo(other).isDir() || !QFileInfo(other).isWritable())
        return {};
    return other;
}

// F5 / F6: the selection to the other pane's folder, through the normal
// copy/move engine (progress, conflicts, undo, USB safety).
void MainWindow::transferToOtherPane(bool move)
{
    const QString target = otherPaneFolder();
    const QStringList paths = selectedPaths();
    if (target.isEmpty() || paths.isEmpty())
        return;
    if (move && QFileInfo(paths.first()).absolutePath() == target) {
        showMessage(tr("They're already in that folder"));
        return;
    }
    move ? m_ops->move(paths, target) : m_ops->copy(paths, target);
}

// Everything one file pane needs: model, views, delegates, floating bar. It
// builds into m_pane, which the caller points at m_panes[index] (the first
// pane by default; ensureSecondPane with a PaneScope).
QWidget *MainWindow::buildPane(QWidget *parent, int index)
{
    m_pane->model = new DirectoryModel(this);
    m_pane->model->setTreeEnabled(m_expandFolders && !m_pane->grid);
    m_pane->model->setSearchContents(QSettings().value("search/contents", false).toBool());
    m_pane->proxy = new FileFilterProxy(this);
    m_pane->proxy->setSourceModel(m_pane->model);
    m_pane->model->setShowHidden(m_showHidden);
    connect(m_pane->model, &DirectoryModel::extensionInfoChanged, this, [this, index] {
        if (index == paneIndex() && !inBackground())
            updateGitBadge();
    });
    for (auto signal : {&QAbstractItemModel::rowsInserted, &QAbstractItemModel::rowsRemoved}) {
        connect(m_pane->model, signal, this, [this, index] {
            if (index == paneIndex() && !inBackground() && m_pane->model->isTrash())
                updateEditActions(); // Restore All follows what's left in the Trash
        });
    }
    connect(m_pane->model, &DirectoryModel::loaded, this, [this, index] {
        PaneScope scope(this, index);
        updateFloatingBar();
        updateEditActions();
        applyPendingSelection();
        if (m_pane->pendingScroll >= 0) { // a tab coming back: where it was
            m_pane->view->verticalScrollBar()->setValue(m_pane->pendingScroll);
            m_pane->pendingScroll = -1;
        }
    });
    // The open folder was deleted or moved: go to the nearest folder left.
    connect(m_pane->model, &DirectoryModel::folderGone, this, [this, index](const QString &gone) {
        PaneScope scope(this, index);
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
    connect(m_pane->proxy, &QAbstractItemModel::rowsInserted, this, [this, index] {
        PaneScope scope(this, index);
        applyPendingSelection();
    });
    // "Searching… 12 found" as results arrive.
    auto searchCount = [this, index] {
        PaneScope scope(this, index);
        if (m_pane->model->isSearch())
            updateFloatingBar();
    };
    connect(m_pane->model, &QAbstractItemModel::rowsInserted, this, searchCount);
    connect(m_pane->model, &QAbstractItemModel::rowsRemoved, this, searchCount);
    connect(m_pane->model, &QAbstractItemModel::modelReset, this, searchCount);

    m_pane->gridDelegate = new FileItemDelegate(m_thumbnailer, this);
    m_pane->listDelegate = new FileItemDelegate(m_thumbnailer, this);
    m_pane->listDelegate->setGridMode(false);
    m_pane->gridDelegate->setTheme(m_theme);
    m_pane->listDelegate->setTheme(m_theme);

    auto *container = new QWidget(parent);
    container->setObjectName("paneContainer");
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto *caption = new QLabel(container);
    caption->setObjectName("paneCaption");
    caption->hide(); // split view only
    caption->installEventFilter(this); // a click makes the pane active
    layout->addWidget(caption);

    m_pane->viewStack = new QStackedWidget(container);
    m_pane->viewStack->setObjectName("viewStack");
    m_pane->viewStack->installEventFilter(this); // resize -> reposition overlays
    layout->addWidget(m_pane->viewStack, 1);

    // Grid.
    m_pane->iconView = new QListView(m_pane->viewStack);
    m_pane->iconView->setObjectName("files");
    m_pane->iconView->setViewMode(QListView::IconMode);
    m_pane->iconView->setMovement(QListView::Static);
    m_pane->iconView->setResizeMode(QListView::Adjust);
    m_pane->iconView->setUniformItemSizes(true);
    m_pane->iconView->setWordWrap(true);
    m_pane->iconView->setSpacing(0);
    setupFileView(m_pane->iconView);
    m_pane->iconView->setItemDelegate(m_pane->gridDelegate);
    m_pane->gridDelegate->setIconSize(FileItemDelegate::gridSizes().at(m_pane->gridZoom));
    m_pane->listDelegate->setIconSize(FileItemDelegate::listSizes().at(m_pane->listZoom));
    m_pane->iconView->setGridSize(m_pane->gridDelegate->gridCellSize(m_pane->iconView->fontMetrics()));

    // List with columns. Both views share the model and the selection.
    m_pane->detailView = new QTreeView(m_pane->viewStack);
    m_pane->detailView->setObjectName("detailView");
    m_pane->detailView->setRootIsDecorated(false);
    m_pane->detailView->setItemsExpandable(false);
    m_pane->detailView->setIndentation(0);
    m_pane->detailView->setUniformRowHeights(true);
    m_pane->detailView->setAllColumnsShowFocus(true);
    m_pane->detailView->setSelectionBehavior(QAbstractItemView::SelectRows);
    setupFileView(m_pane->detailView);
    m_pane->detailView->setItemDelegate(m_pane->listDelegate);
    QItemSelectionModel *unused = m_pane->detailView->selectionModel();
    m_pane->detailView->setSelectionModel(m_pane->iconView->selectionModel());
    delete unused;
    QHeaderView *columns = m_pane->detailView->header();
    columns->setStretchLastSection(false);
    columns->setHighlightSections(false);
    columns->setSectionsMovable(false);
    columns->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    columns->setSectionResizeMode(DirectoryModel::NameColumn, QHeaderView::Stretch);
    columns->resizeSection(DirectoryModel::SizeColumn, 110);
    columns->resizeSection(DirectoryModel::TypeColumn, 170);
    columns->resizeSection(DirectoryModel::ModifiedColumn, 130);
    columns->resizeSection(DirectoryModel::OwnerColumn, 110);
    columns->resizeSection(DirectoryModel::GroupColumn, 110);
    columns->resizeSection(DirectoryModel::PermissionsColumn, 110);
    for (int column : {DirectoryModel::OwnerColumn, DirectoryModel::GroupColumn,
                       DirectoryModel::PermissionsColumn})
        columns->setSectionHidden(column, !m_listColumns.contains(QString::number(column)));
    columns->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(columns, &QWidget::customContextMenuRequested, this,
            [this, columns](const QPoint &pos) { showListHeaderMenu(columns, pos); });
    // Header clicks sort the model (FileFilterProxy forwards sort()).
    columns->setSortIndicator(DirectoryModel::NameColumn, Qt::AscendingOrder);
    m_pane->detailView->setSortingEnabled(true);
    connect(columns, &QHeaderView::sortIndicatorChanged, this, &MainWindow::updateSortActions);
    m_pane->viewStack->addWidget(m_pane->iconView);
    m_pane->viewStack->addWidget(m_pane->detailView);
    m_pane->view = m_pane->grid ? static_cast<QAbstractItemView *>(m_pane->iconView) : m_pane->detailView;
    m_pane->viewStack->setCurrentWidget(m_pane->view);

    QItemSelectionModel *selection = m_pane->iconView->selectionModel();
    connect(selection, &QItemSelectionModel::currentChanged, this, [this, index] {
        if (index == paneIndex() && !inBackground() && quickLookVisible())
            updateQuickLook(); // arrows move through the files, the preview follows
    });
    connect(selection, &QItemSelectionModel::selectionChanged, this, [this, index] {
        PaneScope scope(this, index);
        updateFloatingBar();
        updateEditActions();
    });
    connect(m_pane->proxy, &QAbstractItemModel::layoutChanged, m_rename, &InlineRename::reposition);

    m_pane->fade = new FadeOverlay(m_pane->viewStack); // under the floating bar, over the views
    m_pane->floatingBar = new QLabel(m_pane->viewStack);
    m_pane->floatingBar->setObjectName("floatingBar");
    m_pane->floatingBar->hide();
    m_pane->messageTimer = new QTimer(this);
    m_pane->messageTimer->setSingleShot(true);
    m_pane->messageTimer->setInterval(3000);
    connect(m_pane->messageTimer, &QTimer::timeout, this, [this, index] {
        PaneScope scope(this, index);
        m_pane->message.clear();
        updateFloatingBar();
    });

    m_panes[index].container = container;
    m_panes[index].caption = caption;
    m_paneCount = qMax(m_paneCount, index + 1);
    return container;
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
    m_viewButton->setIcon(glyphIcon(m_pane->grid ? Glyph::List : Glyph::Grid, m_theme.fg));
    m_analyzeButton->setIcon(glyphIcon(Glyph::Usage, m_theme.fg));
    if (m_csd) {
        m_minButton->setIcon(glyphIcon(Glyph::Minimize, m_theme.fg));
        m_closeButton->setIcon(glyphIcon(Glyph::Close, m_theme.fg));
        updateWindowState();
    }

    m_pathBar->setTheme(m_theme);
    for (int i = 0; i < m_paneCount; ++i) {
        m_panes[i].gridDelegate->setTheme(m_theme);
        m_panes[i].listDelegate->setTheme(m_theme);
    }
    if (m_splitButton)
        m_splitButton->setIcon(glyphIcon(Glyph::Split, m_theme.fg));
    m_opsPanel->setTheme(m_theme);
    if (m_tabBar)
        m_tabBar->setTheme(m_theme);
#if defined(ARGONAUT_TERMINAL)
    m_terminalButton->setIcon(glyphIcon(Glyph::Terminal, m_theme.fg));
    m_terminalCloseButton->setIcon(glyphIcon(Glyph::Close, m_theme.fg));
    m_terminal->setTheme(m_theme);
#endif
    m_toast->setTheme(m_theme);
    m_pane->view->viewport()->update();

    m_sidebar->setTheme(m_theme); // rebuilds its rows
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
    else if (event->type() == QEvent::ActivationChange && isActiveWindow())
        refreshGitAll(); // back from an editor or a terminal: files may have changed
}

// -------------------------------------------------------------- sidebar ---

void MainWindow::showSidebarMenu(const QPoint &pos)
{
    QListWidgetItem *item = m_sidebar->itemAt(pos);
    if (!item || Sidebar::isSeparator(item))
        return;
    const QString path = Sidebar::pathOf(item);
    const Sidebar::Kind kind = Sidebar::kindOf(item);

    QMenu *menu = makeMenu();
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->addAction(tr("Open"), this, [this, path] { navigateTo(path); });
    if (m_openTerminalHereAct) {
        menu->addAction(tr("Open in Terminal"), this, [this, path] {
            m_contextFolder = path;
            m_openTerminalHereAct->trigger();
        });
    }
    if (kind == Sidebar::BookmarkItem) {
        menu->addSeparator();
        menu->addAction(tr("Rename…"), this, [this, path, item] {
            bool ok = false;
            const QString label = QInputDialog::getText(this, tr("Rename Bookmark"), tr("Name"),
                                                        QLineEdit::Normal, item->text(), &ok);
            if (ok)
                Bookmarks::rename(path, label == QFileInfo(path).fileName() ? QString() : label);
            m_sidebar->refreshSoon();
        });
        menu->addAction(tr("Remove from Bookmarks"), this, [this, path] {
            Bookmarks::remove(path);
            m_sidebar->refreshSoon();
            updateBookmarkAction();
        });
    } else if (kind == Sidebar::NetworkItem && Sidebar::canEject(item)) {
        menu->addSeparator();
        const QString name = item->text();
        menu->addAction(glyphIcon(Glyph::Eject, m_theme.fg), tr("Disconnect"), this,
                        [this, path, name] { ejectDrive(path, name); });
    } else if (kind == Sidebar::DriveItem && Sidebar::canEject(item)) {
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
    if (m_pane->current.isEmpty() || m_pane->model->isTrash() || m_pane->model->isVirtual())
        return;
    if (Bookmarks::contains(m_pane->current)) {
        Bookmarks::remove(m_pane->current);
        showMessage(tr("Removed from bookmarks"));
    } else if (Bookmarks::add(m_pane->current)) {
        showMessage(tr("“%1” added to bookmarks").arg(QFileInfo(m_pane->current).fileName()));
    }
    m_sidebar->refreshSoon();
    updateBookmarkAction();
}

void MainWindow::updateBookmarkAction()
{
    if (inBackground())
        return;
    const bool marked = Bookmarks::contains(m_pane->current);
    m_bookmarkAct->setText(marked ? tr("Remove Bookmark") : tr("Bookmark This Folder"));
    m_bookmarkAct->setEnabled(!m_pane->current.isEmpty() && !m_pane->model->isTrash() && !m_pane->model->isVirtual());
}

void MainWindow::applySearchFilters()
{
    const auto type = DirectoryModel::TypeFilter(m_typeFilters->checkedAction()->data().toInt());
    const auto date = DirectoryModel::DateFilter(m_dateFilters->checkedAction()->data().toInt());
    QStringList shown;
    if (type != DirectoryModel::TypeFilter::Any)
        shown << m_typeFilters->checkedAction()->text();
    if (date != DirectoryModel::DateFilter::Any)
        shown << m_dateFilters->checkedAction()->text();
    m_filterButton->setText(shown.isEmpty() ? tr("Filters") : shown.join(QStringLiteral(" · ")));
    m_pane->model->setSearchFilters(type, date);
    updateFloatingBar();
}

// ------------------------------------------------------- drag, extras ---

// A drag held over a folder: open it in place (list with expandable
// folders), or go into it, like Nautilus.
void MainWindow::springOver(const QString &folder, int pane)
{
    if (pane != paneIndex() && pane < m_paneCount) {
        PaneScope scope(this, pane); // a drag over the other pane: open it there
        springOver(folder, paneIndex());
        return;
    }
    if (!m_pane->grid && m_pane->model->isTreeActive() && m_pane->model->indexForPath(folder).isValid()) {
        if (!m_pane->model->isExpanded(folder))
            m_pane->model->expand(folder);
        return;
    }
    navigateTo(folder);
}

// The commands extensions offer for these items (none: for the open folder's
// empty space). The extension does the work itself, with the paths; nothing
// goes through our file operations, so Undo doesn't know about it.
void MainWindow::addExtensionCommands(QMenu *menu, const QStringList &selection)
{
    if (m_chooser || m_pane->model->isTrash())
        return;
    QList<Extensions::Item> items;
    for (const QString &path : selection) {
        const QModelIndex source = m_pane->model->indexForPath(path);
        if (!source.isValid())
            return;
        const bool dir = m_pane->model->isDir(source);
        items.append({path, dir ? QStringLiteral("inode/directory") : effectiveMimeAt(source), dir});
    }
    Extensions::Host *host = Extensions::Host::instance();
    const QString folder = m_pane->model->extensionsFolder();
    const QList<Extensions::Command> commands = host->commands(folder, items);
    for (const Extensions::Command &command : commands) {
        menu->addAction(command.label, this, [this, command, folder, selection] {
            Extensions::Host *host = Extensions::Host::instance();
            if (!command.confirm.isEmpty()) {
                QMessageBox box(QMessageBox::Question, host->nameOf(command.extension), command.confirm,
                                QMessageBox::NoButton, this);
                box.setTextFormat(Qt::PlainText);
                QPushButton *go = box.addButton(QString(command.label).remove(QStringLiteral("…")), QMessageBox::AcceptRole);
                box.addButton(QMessageBox::Cancel);
                box.setDefaultButton(QMessageBox::Cancel);
                box.exec();
                if (box.clickedButton() != go)
                    return;
            }
            host->invoke(command, folder, selection);
        });
    }
}

// Scripts ▸: the user's Nautilus scripts, run on `selection`. Sub-folders
// become sub-menus. Null when there are none (or here isn't a real folder).
QMenu *MainWindow::scriptsMenu(const QStringList &selection)
{
    const QString root = DesktopActions::scriptsFolder();
    if (m_pane->model->isVirtual() || m_pane->model->isTrash())
        return nullptr;
    const QList<DesktopActions::Script> scripts = DesktopActions::scripts(root);
    if (scripts.isEmpty())
        return nullptr;
    std::function<void(QMenu *, const QList<DesktopActions::Script> &)> fill =
        [&](QMenu *menu, const QList<DesktopActions::Script> &entries) {
            for (const DesktopActions::Script &entry : entries) {
                if (entry.isFolder()) {
                    QMenu *sub = makeMenu();
                    sub->setTitle(entry.name);
                    sub->setParent(menu, sub->windowFlags());
                    fill(sub, entry.children);
                    menu->addMenu(sub);
                    continue;
                }
                menu->addAction(entry.name, this, [this, script = entry.path, selection] {
                    if (!DesktopActions::runScript(script, m_pane->current, selection, geometry()))
                        showMessage(tr("Couldn't run “%1”").arg(QFileInfo(script).fileName()));
                });
            }
        };
    QMenu *menu = makeMenu();
    menu->setTitle(tr("Scripts"));
    fill(menu, scripts);
    menu->addSeparator();
    menu->addAction(tr("Open Scripts Folder"), this, [this, root] { navigateTo(root); });
    menu->setAttribute(Qt::WA_DeleteOnClose);
    return menu;
}

// ------------------------------------------------------------ organize ---

void MainWindow::organizeWithClaude()
{
    if (m_pane->current.isEmpty() || !QFileInfo(m_pane->current).isWritable() || m_pane->model->isVirtual())
        return;
    const QString folder = m_pane->current;
    OrganizerDialog dialog(folder, this);
    connect(&dialog, &OrganizerDialog::openPreferences, this, [this] { m_prefsAct->trigger(); });
    if (dialog.exec() != QDialog::Accepted)
        return;
    QList<QPair<QString, QPair<QString, QString>>> moves;
    for (const Organizer::Move &m : dialog.keptMoves())
        moves.append({m.item, {m.folder, m.newName}});
    applyOrganizePlan(folder, moves);
}

// The kept moves: new folders first (undoable on their own), then one move
// job with renames ("Undo Organize" puts everything back).
void MainWindow::applyOrganizePlan(const QString &folder, const QList<QPair<QString, QPair<QString, QString>>> &moves)
{
    QList<TransferResult> created; // the top-most folders made for the plan
    QList<Transfer> transfers;
    for (const auto &m : moves) {
        const QString &item = m.first;
        const QString &sub = m.second.first;
        const QString &newName = m.second.second;
        QString dir = folder;
        for (const QString &part : sub.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
            dir += QLatin1Char('/') + part;
            if (!QFileInfo::exists(dir)) {
                if (!QDir().mkdir(dir)) {
                    showMessage(tr("Couldn't create the folder “%1”").arg(part));
                    return;
                }
                bool inside = false;
                for (const TransferResult &c : std::as_const(created))
                    inside = inside || dir.startsWith(c.target + QLatin1Char('/'));
                if (!inside)
                    created.append({QString(), dir});
            }
        }
        transfers.append({folder + QLatin1Char('/') + item, dir, newName.isEmpty() ? item : newName});
    }
    if (!created.isEmpty())
        m_ops->recordUndo(FileJob::Kind::Copy, created, tr("New Folders"));
    if (!transfers.isEmpty())
        m_ops->moveItems(transfers, tr("Organize"));
    showMessage(transfers.size() == 1 ? tr("Organizing 1 item…") : tr("Organizing %1 items…").arg(transfers.size()));
}

// ------------------------------------------------------------- servers ---

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
    if (m_pane->current == root || m_pane->current.startsWith(root + QLatin1Char('/')))
        navigateTo(QDir::homePath()); // don't keep the drive busy ourselves

    m_mounts->eject(root, name);
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
    m_previewWidth = settings.value("window/previewWidth", 0).toInt();
    if (settings.contains("view/listHeader"))
        m_pane->detailView->header()->restoreState(settings.value("view/listHeader").toByteArray());
    m_listColumns = settings.value("view/listColumns").toStringList();
    m_expandFolders = settings.value("view/expandFolders", false).toBool();
    applyExpandFolders();
    applyListColumns(); // after restoreState: an older saved header knows only 4 columns
        fitListColumns(); // the saved layout may hide columns this width has room for
    sortBy(settings.value("view/sortColumn", DirectoryModel::NameColumn).toInt(),
           Qt::SortOrder(settings.value("view/sortOrder", int(Qt::AscendingOrder)).toInt()));
    m_hiddenAct->setChecked(settings.value("view/showHidden", false).toBool());
    m_sidebarAct->setChecked(settings.value("view/sidebar", true).toBool());
    setGridMode(settings.value("view/grid", true).toBool());
    // The split view comes back (once the first pane has its folder).
    if (settings.value("view/split", false).toBool()) {
        const QString otherPath = settings.value("view/splitPath").toString();
        QTimer::singleShot(0, this, [this, otherPath] {
            if (m_pane->current.isEmpty() || m_split)
                return;
            setSplitView(true);
            if (!otherPath.isEmpty() && (QFileInfo(otherPath).isDir() || DirectoryModel::isVirtualPath(otherPath)))
                navigateTo(otherPath, false);
            activatePane(0);
        });
    }
    m_analyzeFilesAct->setChecked(settings.value("view/analyzeFiles", false).toBool());
    // Saved as pixels (not levels), so changing the levels later keeps the choice.
    auto nearest = [](const QList<int> &sizes, int px) {
        int best = 0;
        for (int i = 0; i < sizes.size(); ++i) {
            if (qAbs(sizes.at(i) - px) < qAbs(sizes.at(best) - px))
                best = i;
        }
        return best;
    };
    m_pane->gridZoom = nearest(FileItemDelegate::gridSizes(),
                         settings.value("view/gridIconSize", FileItemDelegate::DefaultGridIconSize).toInt());
    m_pane->listZoom = nearest(FileItemDelegate::listSizes(),
                         settings.value("view/listIconSize", FileItemDelegate::DefaultListIconSize).toInt());
    applyZoom();
}

void MainWindow::saveSettings()
{
    QSettings settings;
    settings.setValue("window/geometry", saveGeometry());
    settings.setValue("window/sidebarSplitter", m_splitter->saveState());
    if (m_terminalPanel && m_terminalPanel->isVisible())
        m_terminalHeight = m_terminalPanel->height();
    settings.setValue("window/terminalHeight", m_terminalHeight);
    if (quickLookVisible())
        m_previewWidth = m_quickLook->width();
    settings.setValue("window/previewWidth", m_previewWidth);
    settings.setValue("view/listHeader", m_pane->detailView->header()->saveState());
    settings.setValue("view/listColumns", m_listColumns);
    settings.setValue("view/sortColumn", m_pane->recentSort ? m_pane->sortColumnBefore : m_pane->model->sortColumn());
    settings.setValue("view/sortOrder", int(m_pane->recentSort ? m_pane->sortOrderBefore : m_pane->model->sortOrder()));
    settings.setValue("view/showHidden", m_showHidden);
    settings.setValue("view/sidebar", m_sidebarAct->isChecked());
    settings.setValue("view/grid", m_pane->grid);
    settings.setValue("view/split", m_split);
    if (m_split)
        settings.setValue("view/splitPath", m_panes[1].current);
    settings.setValue("view/gridIconSize", FileItemDelegate::gridSizes().at(m_pane->gridZoom));
    settings.setValue("view/listIconSize", FileItemDelegate::listSizes().at(m_pane->listZoom));
    settings.setValue("view/analyzeFiles", m_analyzeFilesAct->isChecked());
}

// Highlight the sidebar entry matching the current folder, like Nautilus.
void MainWindow::syncSidebarSelection()
{
    if (inBackground())
        return;
    m_sidebar->selectPath(m_pane->current);
}

// ----------------------------------------------------------- navigation ---

void MainWindow::openFolders(const QStringList &folders)
{
    if (folders.isEmpty())
        return;
    navigateTo(folders.first());
    for (int i = 1; i < folders.size(); ++i)
        openTab(folders.at(i), false);
}

void MainWindow::showItems(const QStringList &items)
{
    QStringList dirs;
    QHash<QString, QStringList> byDir;
    for (const QString &item : items) {
        const QString dir = QFileInfo(item).absolutePath();
        if (!byDir.contains(dir))
            dirs << dir;
        byDir[dir] << QDir::cleanPath(item);
    }
    if (dirs.isEmpty())
        return;
    if (navigateTo(dirs.first()))
        selectPaths(byDir.value(dirs.first())); // applied once the folder is listed
    for (int i = 1; i < dirs.size(); ++i)
        openTab(dirs.at(i), false);
}

bool MainWindow::navigateTo(const QString &path, bool addToHistory)
{
    // A server address (typed, a bookmark, the command line): connect first.
    if (path == Sidebar::connectPlace()) {
        m_connectAct->trigger();
        return false;
    }
    if (Remote::isRemoteAddress(path)) {
        m_mounts->connectToServer(path);
        return false; // opens when connected
    }
    // Recent and Starred aren't folders: lists of items from anywhere.
    const bool place = DirectoryModel::isVirtualPath(path);
    const QFileInfo info(path);
    if (!place && (!info.exists() || !info.isDir())) {
        showMessage(tr("Folder not found: %1").arg(path));
        return false;
    }
    if (!place && (!info.isReadable() || !info.isExecutable())) {
        showMessage(tr("You don't have permission to open “%1”").arg(info.fileName()));
        return false;
    }

    const QString clean = place ? path : QDir::cleanPath(info.absoluteFilePath());
    if (clean == m_pane->current)
        return true;

    if (!inBackground()) {
        // Leaving search here must not re-list the old folder: setPath()
        // below ends the search anyway.
        const QSignalBlocker blocker(m_searchEdit);
        if (m_searchTimer)
            m_searchTimer->stop();
        setSearchActive(false);
        m_rename->cancel();
    }
    m_pane->pendingSelection.clear();
    m_pane->pendingRename.clear();
    if (addToHistory && !m_pane->current.isEmpty()) {
        m_pane->back.push(m_pane->current);
        m_pane->forward.clear();
    }
    // Another folder: the old view fades out over the new one (not while
    // Analyze has the page, and not before the window is up).
    if (clean != m_pane->current && !m_pane->current.isEmpty() && m_pane->fade && isVisible()
        && !(m_analyzing && paneIndex() == 0))
        m_pane->fade->start(m_pane->view);
    m_pane->current = clean;

    m_thumbnailer->cancelPending(); // the old folder's queued thumbnails
    if (m_quickLook && !inBackground())
        m_quickLook->clear(); // stays open; the next selected file fills it
    m_pane->model->setPath(clean);
    // Recent: newest first. The folder sort comes back when leaving it.
    const bool recent = DirectoryModel::placeOf(clean) == DirectoryModel::Place::Recent;
    if (recent && !m_pane->recentSort) {
        m_pane->sortColumnBefore = m_pane->model->sortColumn();
        m_pane->sortOrderBefore = m_pane->model->sortOrder();
        m_pane->recentSort = true;
        sortBy(DirectoryModel::ModifiedColumn, Qt::DescendingOrder);
    } else if (!recent && m_pane->recentSort) {
        m_pane->recentSort = false;
        sortBy(m_pane->sortColumnBefore, m_pane->sortOrderBefore);
    }
    if (m_analyzing && !inBackground()) { // Analyze is the first pane's, and the active one's
        if (place)
            setGridMode(m_pane->grid); // Analyze measures folders
        else
            m_analyzeView->setFolder(clean); // instant if it's inside what was measured
    }
    m_pane->view->scrollToTop();

    // Tabs belong to the first pane.
    const QString title = titleFor(clean);
    if (paneIndex() == 0 && m_tabBar)
        m_tabBar->setActivePath(clean);
    updatePaneCaptions();
    if (inBackground())
        updateFloatingBar(); // this pane's own; the header shows the other one
    else
        refreshChrome();
    return true;
}

void MainWindow::goBack()
{
    if (m_pane->back.isEmpty())
        return;
    const QString previous = m_pane->current;
    if (navigateTo(m_pane->back.pop(), false))
        m_pane->forward.push(previous);
    updateActions();
}

void MainWindow::goForward()
{
    if (m_pane->forward.isEmpty())
        return;
    const QString previous = m_pane->current;
    if (navigateTo(m_pane->forward.pop(), false))
        m_pane->back.push(previous);
    updateActions();
}

void MainWindow::goUp()
{
    QDir dir(m_pane->current);
    if (dir.cdUp())
        navigateTo(dir.absolutePath());
}

void MainWindow::openIndex(const QModelIndex &viewIndex)
{
    const QModelIndex index = toSource(viewIndex);
    const QString path = m_pane->model->filePath(index);
#if defined(ARGONAUT_DBUS)
    if (m_chooser && !m_pane->model->isDir(index)) { // a file: that's the choice, nothing is opened
        m_chooser->fileActivated(path);
        return;
    }
#endif
    if (m_pane->model->isDir(index))
        navigateTo(path);
    else
        openFile(path);
}

void MainWindow::updateActions()
{
    if (inBackground())
        return; // the header shows the active pane
    m_backAct->setEnabled(!m_pane->back.isEmpty());
    m_forwardAct->setEnabled(!m_pane->forward.isEmpty());
    const bool place = DirectoryModel::isVirtualPath(m_pane->current);
    m_upAct->setEnabled(!place && !m_pane->current.isEmpty() && !QDir(m_pane->current).isRoot());
    m_analyzeAct->setEnabled(!place);
    if (m_analyzeButton)
        m_analyzeButton->setEnabled(!place);
    m_backButton->setEnabled(m_backAct->isEnabled());
    m_forwardButton->setEnabled(m_forwardAct->isEnabled());
#if defined(ARGONAUT_DBUS)
    if (m_chooser) // an Open/Save window: its commands stay off
        m_chooser->keepActionsOff();
#endif
}

// ----------------------------------------------------------- view modes ---

void MainWindow::setGridMode(bool grid)
{
    m_rename->cancel();
    const bool wasAnalyzing = m_analyzing;
    m_analyzing = false;
    if (m_analyzeButton)
        m_analyzeButton->setChecked(false);
    m_pane->grid = grid;
    m_pane->model->setTreeEnabled(m_expandFolders && !grid); // the grid shows only the open folder
    QAbstractItemView *next = grid ? static_cast<QAbstractItemView *>(m_pane->iconView) : m_pane->detailView;
    const bool hadFocus = m_pane->view && m_pane->view->hasFocus();
    const QModelIndex current = m_pane->view ? m_pane->view->currentIndex() : QModelIndex();
    if (!grid) {
        // The grid selects only the first column; the table selects rows.
        QItemSelection rows;
        for (const QModelIndex &index : selectedViewIndexes())
            rows.select(index, index.siblingAtColumn(DirectoryModel::ColumnCount - 1));
        m_pane->view->selectionModel()->select(rows, QItemSelectionModel::ClearAndSelect);
    }
    m_pane->viewStack->setCurrentWidget(next);
    m_pane->view = next;
    if (current.isValid())
        m_pane->view->scrollTo(current);
    if (hadFocus || wasAnalyzing)
        m_pane->view->setFocus();
    if (wasAnalyzing) {
        updateEditActions();
        updateFloatingBar();
    }
    m_rename->reposition();
    updateViewChrome();
}

void MainWindow::updateViewChrome()
{
    if (inBackground() || !m_viewButton)
        return;
    if (m_zoomInAct) { // grid and list have their own zoom limits
        const int level = m_pane->grid ? m_pane->gridZoom : m_pane->listZoom;
        const int last = int((m_pane->grid ? FileItemDelegate::gridSizes() : FileItemDelegate::listSizes()).size()) - 1;
        m_zoomInAct->setEnabled(level < last);
        m_zoomOutAct->setEnabled(level > 0);
    }
    m_viewButton->setIcon(glyphIcon(m_pane->grid ? Glyph::List : Glyph::Grid, m_theme.fg));
    m_viewButton->setToolTip(m_pane->grid ? tr("List view (Ctrl+2)") : tr("Grid view (Ctrl+1)"));
    if (m_gridAct && !m_analyzing)
        (m_pane->grid ? m_gridAct : m_listAct)->setChecked(true);
}

void MainWindow::setAnalyzeMode(bool on)
{
    if (!on) {
        setGridMode(m_pane->grid);
        return;
    }
    if (paneIndex() != 0)
        activatePane(0); // Analyze lives in the first pane
    m_rename->cancel();
    setSearchActive(false); // search is about files; Analyze shows folders
    hideQuickLook();
    m_analyzing = true;
    m_analyzeAct->setChecked(true);
    m_analyzeButton->setChecked(true);
    m_pane->viewStack->setCurrentWidget(m_analyzeView);
    m_analyzeView->setFolder(m_pane->current);
    m_analyzeView->setFocus();
    updateEditActions();
    updateFloatingBar();
}

void MainWindow::showAnalyzeMenu(const QPoint &pos)
{
    const AnalyzeView::Row row = m_analyzeView->rowAt(pos);
    const QString path = row.path;
    QMenu *menu = makeMenu();
    menu->setAttribute(Qt::WA_DeleteOnClose);
    if (row.isFile) {
        menu->addAction(tr("Open"), this, [this, path] { openFile(path); });
        menu->addAction(tr("Show in Folder"), this, [this, path] {
            navigateTo(QFileInfo(path).absolutePath());
            setGridMode(m_pane->grid);
            selectPaths({path});
        });
    } else if (!path.isEmpty()) {
        menu->addAction(tr("Open Folder"), this, [this, path] {
            navigateTo(path);
            setGridMode(m_pane->grid);
        });
        menu->addAction(tr("Analyze This Folder"), this, [this, path] { navigateTo(path); })
            ->setEnabled(path != m_pane->current);
        menu->addAction(tr("Open in New Tab"), this, [this, path] { openTab(path, false); });
    }
    if (!path.isEmpty()) {
        menu->addAction(tr("Copy Location"), this, [path] {
            QApplication::clipboard()->setText(QDir::toNativeSeparators(path));
        });
        if (path != m_pane->current) {
            menu->addSeparator();
            menu->addAction(tr("Move to Trash"), this, [this, path] { m_ops->trash({path}); });
        }
        menu->addSeparator();
    }
    menu->addAction(m_analyzeFilesAct);
    menu->addAction(tr("Scan Again"), m_analyzeView, &AnalyzeView::rescan);
    menu->popup(m_analyzeView->viewport()->mapToGlobal(pos));
}

void MainWindow::zoom(int step)
{
    if (m_analyzing)
        return;
    const QList<int> &sizes = m_pane->grid ? FileItemDelegate::gridSizes() : FileItemDelegate::listSizes();
    int &level = m_pane->grid ? m_pane->gridZoom : m_pane->listZoom;
    const int defaultLevel = int(sizes.indexOf(m_pane->grid ? FileItemDelegate::DefaultGridIconSize
                                                     : FileItemDelegate::DefaultListIconSize));
    const int next = step == 0 ? defaultLevel : qBound(0, level + step, int(sizes.size()) - 1);
    if (next == level)
        return;
    level = next;
    applyZoom();
}

void MainWindow::applyZoom()
{
    m_pane->gridDelegate->setIconSize(FileItemDelegate::gridSizes().at(m_pane->gridZoom));
    m_pane->listDelegate->setIconSize(FileItemDelegate::listSizes().at(m_pane->listZoom));
    m_pane->iconView->setGridSize(m_pane->gridDelegate->gridCellSize(m_pane->iconView->fontMetrics()));
    // Keep what you were looking at in view.
    const QModelIndex current = m_pane->view->currentIndex();
    if (current.isValid())
        m_pane->view->scrollTo(current);
    m_rename->reposition();
    updateViewChrome();
}

void MainWindow::setShowHidden(bool show)
{
    m_showHidden = show;
    // One switch for the window: the other pane follows too.
    for (int i = 0; i < m_paneCount; ++i)
        m_panes[i].model->setShowHidden(show);
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
        if (m_analyzing)
            setGridMode(m_pane->grid); // search finds files: show the file view
        m_locationStack->setCurrentWidget(m_searchEdit->parentWidget());
        m_searchEdit->setFocus(Qt::ShortcutFocusReason);
        m_searchEdit->selectAll();
    } else if (m_locationStack->currentWidget() != m_pathBar) {
        // Filters end with the search.
        m_typeFilters->actions().first()->setChecked(true);
        m_dateFilters->actions().first()->setChecked(true);
        m_filterButton->setText(tr("Filters"));
        m_pane->model->setSearchFilters(DirectoryModel::TypeFilter::Any, DirectoryModel::DateFilter::Any);
        m_searchEdit->clear();
        m_locationStack->setCurrentWidget(m_pathBar);
        m_pane->view->setFocus();
    }
}

// Search = every item in this folder and below whose name has all the
// typed words (DirectoryModel::setSearch). Typing is debounced a little so
// each key doesn't start a new walk; clearing is immediate.
void MainWindow::updateFilter()
{
    m_pane->model->setShowHidden(m_showHidden);
    const QString query = m_searchEdit ? m_searchEdit->text().simplified() : QString();
    if (query.isEmpty() || !m_searchTimer) {
        if (m_searchTimer)
            m_searchTimer->stop();
        m_pane->model->setSearch(query);
    } else {
        m_searchTimer->start();
    }
    if (m_pane->view) // not during setupUi
        updateFloatingBar();
}

QModelIndex MainWindow::toSource(const QModelIndex &viewIndex) const
{
    return viewIndex.model() == m_pane->proxy ? m_pane->proxy->mapToSource(viewIndex) : viewIndex;
}

QModelIndex MainWindow::toView(const QModelIndex &sourceIndex) const
{
    return m_pane->proxy->mapFromSource(sourceIndex);
}

// -------------------------------------------------------- floating bar ---

void MainWindow::showMessage(const QString &text)
{
    m_pane->message = text;
    m_pane->messageTimer->start();
    updateFloatingBar();
}

void MainWindow::updateFloatingBar()
{
    QString text = m_pane->message;
    if (text.isEmpty() && m_analyzing)
        text = m_analyzeView->statusText();
    if (text.isEmpty() && !m_analyzing) {
        const QModelIndexList selected = selectedViewIndexes();
        if (selected.size() == 1) {
            const QModelIndex idx = toSource(selected.first());
            text = m_pane->model->isDir(idx)
                       ? tr("“%1” selected").arg(m_pane->model->fileName(idx))
                       : tr("“%1” selected (%2)")
                             .arg(m_pane->model->fileName(idx),
                                  formatSize(m_pane->model->size(idx)));
        } else if (selected.size() > 1) {
            qint64 bytes = 0;
            for (const QModelIndex &viewIdx : selected) {
                const QModelIndex idx = toSource(viewIdx);
                if (!m_pane->model->isDir(idx))
                    bytes += m_pane->model->size(idx);
            }
            text = tr("%n items selected", nullptr, int(selected.size()));
            if (bytes > 0)
                text += QStringLiteral(" (%1)").arg(formatSize(bytes));
        }
    }
    if (text.isEmpty() && !m_analyzing && m_pane->model->isSearch()) {
        const int found = m_pane->model->rowCount();
        if (m_pane->model->isLoading())
            text = found == 0 ? tr("Searching…") : tr("Searching… %1 found").arg(QLocale().toString(found));
        else if (m_pane->model->searchTruncated())
            text = tr("Showing the first %1 results").arg(QLocale().toString(found));
        else
            text = found == 0   ? tr("No results")
                   : found == 1 ? tr("1 result")
                                : tr("%1 results").arg(QLocale().toString(found));
    }

    if (text.isEmpty()) {
        m_pane->floatingBar->hide();
        return;
    }
    m_pane->floatingBar->setText(text);
    m_pane->floatingBar->adjustSize();
    positionFloatingBar();
    m_pane->floatingBar->show();
    m_pane->floatingBar->raise();
}

void MainWindow::positionFloatingBar()
{
    const int margin = 10;
    const int maxWidth = qMax(100, m_pane->viewStack->width() - 2 * margin);
    if (m_pane->floatingBar->width() > maxWidth)
        m_pane->floatingBar->resize(maxWidth, m_pane->floatingBar->height());
    m_pane->floatingBar->move(m_pane->viewStack->width() - m_pane->floatingBar->width() - margin,
                        m_pane->viewStack->height() - m_pane->floatingBar->height() - margin);
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
    // Split view: input in the other pane makes it the active one; drags
    // and resizes there are handled as that pane, without switching to it.
    if (m_paneCount > 1) {
        const int pane = paneOf(watched);
        if (pane >= 0 && pane != paneIndex()) {
            switch (event->type()) {
            case QEvent::MouseButtonPress:
            case QEvent::MouseButtonDblClick:
            case QEvent::ContextMenu:
            case QEvent::KeyPress:
            case QEvent::Wheel:
            case QEvent::FocusIn:
                if (!inBackground())
                    activatePane(pane);
                break;
            case QEvent::DragEnter:
            case QEvent::DragMove:
            case QEvent::DragLeave:
            case QEvent::Drop:
            case QEvent::Resize: {
                PaneScope scope(this, pane);
                return eventFilter(watched, event);
            }
            default:
                break;
            }
        }
    }

    // Ctrl+wheel zooms. Touchpads send many small steps: act on whole notches.
    if (event->type() == QEvent::Wheel && isFileViewport(watched)) {
        auto *wheel = static_cast<QWheelEvent *>(event);
        if (wheel->modifiers() & Qt::ControlModifier) {
            m_wheelZoom += wheel->angleDelta().y();
            while (qAbs(m_wheelZoom) >= 120) {
                zoom(m_wheelZoom > 0 ? +1 : -1);
                m_wheelZoom -= m_wheelZoom > 0 ? 120 : -120;
            }
            return true;
        }
        m_wheelZoom = 0;
    }

    switch (event->type()) {
    case QEvent::DragEnter:
    case QEvent::DragMove:
    case QEvent::DragLeave:
    case QEvent::Drop:
        if (isFileViewport(watched))
            return handleViewDrag(watched == m_pane->iconView->viewport()
                                      ? static_cast<QAbstractItemView *>(m_pane->iconView)
                                      : m_pane->detailView,
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
            const QString target = m_tabBar->pathAt(index);
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
    if (m_csd && m_menuBar && (watched == m_menuBar || watched == m_menuRow)) {
        const QEvent::Type type = event->type();
        const bool isMouse = type == QEvent::MouseButtonPress || type == QEvent::MouseMove
                             || type == QEvent::MouseButtonRelease
                             || type == QEvent::MouseButtonDblClick;
        // Only a mouse event may be cast to one (the cases below are all mouse events).
        auto *me = isMouse ? static_cast<QMouseEvent *>(event) : nullptr;
        // On the bar, a menu title is for its menu; the row has none.
        const bool onMenu = watched == m_menuBar
                            && (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick)
                            && m_menuBar->actionAt(me->position().toPoint());
        switch (event->type()) {
        case QEvent::MouseButtonPress:
            m_menuPressed = me->button() == Qt::LeftButton && !onMenu;
            m_menuPressPos = me->globalPosition().toPoint();
            break;
        case QEvent::MouseMove:
            if (m_menuPressed && (me->buttons() & Qt::LeftButton)
                && (me->globalPosition().toPoint() - m_menuPressPos).manhattanLength()
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
            if (!onMenu) {
                isMaximized() ? showNormal() : showMaximized();
                return true;
            }
            break;
        default:
            break;
        }
    }

    if (watched == windowHandle() && event->type() == QEvent::MouseButtonPress) {
        const Qt::MouseButton button = static_cast<QMouseEvent *>(event)->button();
        if (button == Qt::BackButton || button == Qt::ForwardButton) {
            button == Qt::BackButton ? goBack() : goForward();
            return true;
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
        if (item && me->button() == Qt::MiddleButton && !Sidebar::pathOf(item).isEmpty()) {
            openTab(Sidebar::pathOf(item), false);
            return true;
        }
        if (me->button() == Qt::LeftButton && m_sidebar->ejectButtonHit(item, me->position().toPoint())) {
            ejectDrive(Sidebar::pathOf(item), item->text());
            return true; // don't also open it
        }
    } else if (m_pane->detailView && watched == m_pane->detailView->viewport()
               && (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick)
               && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton
               && [&] { // on a folder's ▸ arrow: open or close it, nothing else
                      const QPoint pos = static_cast<QMouseEvent *>(event)->position().toPoint();
                      const QModelIndex index = m_pane->detailView->indexAt(pos).siblingAtColumn(0);
                      const QRect arrow = m_pane->listDelegate->expanderRect(m_pane->detailView->visualRect(index), index);
                      if (!index.isValid() || !arrow.adjusted(-2, -4, 2, 4).contains(pos))
                          return false;
                      if (event->type() == QEvent::MouseButtonPress) {
                          const QString path = m_pane->model->filePath(toSource(index));
                          if (m_pane->model->isExpanded(path))
                              m_pane->model->collapse(path);
                          else
                              m_pane->model->expand(path);
                      }
                      return true;
                  }()) {
        return true;
    } else if (isFileViewport(watched) && event->type() == QEvent::MouseButtonPress
               && static_cast<QMouseEvent *>(event)->button() == Qt::MiddleButton) {
        auto *view = static_cast<QAbstractItemView *>(static_cast<QWidget *>(watched)->parentWidget());
        const QModelIndex source =
            toSource(view->indexAt(static_cast<QMouseEvent *>(event)->position().toPoint()));
        if (source.isValid() && m_pane->model->isDir(source))
            openTab(m_pane->model->filePath(source), false);
        return true;
    } else if (m_tabBar && watched == m_tabBar && event->type() == QEvent::MouseButtonRelease
               && static_cast<QMouseEvent *>(event)->button() == Qt::MiddleButton) {
        const int index = m_tabBar->tabAt(static_cast<QMouseEvent *>(event)->position().toPoint());
        if (index >= 0)
            m_tabBar->closeTab(index);
        return true;
    } else if (m_pane->view
               && ((isFileViewport(watched) && event->type() == QEvent::MouseButtonPress)
                   || ((watched == m_pane->iconView || watched == m_pane->detailView)
                       && event->type() == QEvent::KeyPress))) {
        m_pane->pendingSelection.clear();
        m_pane->pendingRename.clear();
        if (event->type() == QEvent::KeyPress && typeAheadKey(static_cast<QKeyEvent *>(event)))
            return true;
        if (event->type() == QEvent::KeyPress) {
            auto *key = static_cast<QKeyEvent *>(event);
            if (key->key() == Qt::Key_Space && key->modifiers() == Qt::NoModifier) {
                toggleQuickLook();
                return true;
            }
            if (key->key() == Qt::Key_Escape && quickLookVisible()) {
                hideQuickLook();
                return true;
            }
            if (watched == m_pane->detailView && handleTreeKey(key))
                return true;
        }
    }
    if (m_rename->isActive() && isFileViewport(watched) && event->type() == QEvent::Paint)
        m_rename->reposition(); // item positions are final when the view paints
    if (m_pane->viewStack && watched == m_pane->viewStack && event->type() == QEvent::Resize) {
        fitListColumns();
        if (m_pane->floatingBar->isVisible())
            positionFloatingBar();
        if (m_opsPanel->isVisible()) {
            m_opsPanel->reposition();
            m_toast->setAvoidRect(m_opsPanel->geometry());
        }
        if (m_toast->isVisible())
            m_toast->reposition();
        m_rename->reposition();
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
        paths.append(m_pane->model->filePath(toSource(index)));
    return paths;
}

void MainWindow::openSelection()
{
#if defined(ARGONAUT_DBUS)
    if (m_chooser) {
        m_chooser->openSelection();
        return;
    }
#endif
    const QModelIndexList selected = selectedViewIndexes();
    if (selected.size() == 1) {
        openIndex(selected.first());
        return;
    }
    for (const QModelIndex &index : selected) {
        const QModelIndex source = toSource(index);
        if (!m_pane->model->isDir(source))
            openFile(m_pane->model->filePath(source));
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
        m_ops->move(paths, m_pane->current);
        QApplication::clipboard()->clear(); // a cut is pasted only once
    } else {
        m_ops->copy(paths, m_pane->current);
    }
}

void MainWindow::trashSelection()
{
    if (m_pane->model->isTrash()) {
        deleteSelection(); // already in the Trash: the only step left is forever
        return;
    }
    const QStringList paths = selectedPaths();
    if (paths.isEmpty())
        return;
    // Servers have no Trash (GVfs folders): offer to delete instead.
    for (const QString &p : paths) {
        if (!Remote::mountOf(p).localPath.isEmpty()) {
            QMessageBox box(QMessageBox::Warning, tr("No Trash on This Server"),
                            paths.size() == 1 ? tr("“%1” can't be moved to the Trash.").arg(QFileInfo(paths.first()).fileName())
                                              : tr("These %1 items can't be moved to the Trash.").arg(paths.size()),
                            QMessageBox::NoButton, this);
            box.setInformativeText(tr("Files on a server can only be deleted forever."));
            QPushButton *cancel = box.addButton(QMessageBox::Cancel);
            QPushButton *remove = box.addButton(tr("Delete Permanently"), QMessageBox::DestructiveRole);
            box.setDefaultButton(cancel);
            box.exec();
            if (box.clickedButton() == remove)
                m_ops->deletePermanently(paths);
            return;
        }
    }
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
    if (!m_pane->model->isTrash())
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
    if (inBackground())
        return; // actions act on the active pane
    const bool hasSelection = !m_analyzing && m_pane->view && m_pane->view->selectionModel()->hasSelection();
    const bool inTrash = m_pane->model && m_pane->model->isTrash();
    // Items in a folder you can't change can be copied, not moved or trashed.
    const bool changeable = hasSelection && (inTrash || canChangeSelection());
    for (QAction *action : {m_openAct, m_copyAct})
        action->setEnabled(hasSelection);
    for (QAction *action : {m_cutAct, m_trashAct})
        action->setEnabled(changeable);
    const bool writable = !m_analyzing && !inTrash && !m_pane->current.isEmpty() && QFileInfo(m_pane->current).isWritable();
    m_deleteAct->setEnabled(changeable);
    m_restoreAct->setEnabled(inTrash && hasSelection);
    if (m_trashRestoreButton)
        m_trashRestoreButton->setText(tr("Restore")); // setDefaultAction resets it
    m_emptyTrashAct->setEnabled(Trash::isSupported());
    m_restoreAllAct->setEnabled(inTrash && m_pane->model->rowCount() > 0);
    m_pasteAct->setEnabled(writable && FileOperations::clipboardHasFiles());
    m_newFolderAct->setEnabled(writable);
    m_newFileAct->setEnabled(writable);
    m_renameAct->setEnabled(!inTrash && changeable);
    m_newDocumentMenu->menuAction()->setEnabled(writable);
    m_organizeAct->setEnabled(writable && !m_pane->model->isSearch() && !m_pane->model->isVirtual());
    const bool fileView = !m_analyzing && m_pane->view && m_pane->model->rowCount() > 0;
    m_selectMatchingAct->setEnabled(fileView);
    m_invertSelectionAct->setEnabled(fileView);
    const bool archives = FileJob::archivesSupported();
    m_compressAct->setVisible(archives);
    m_compressAct->setEnabled(archives && writable && hasSelection);
    const bool extractable = archives && !inTrash && hasSelection && !selectedArchives().isEmpty();
    m_extractHereAct->setVisible(archives);
    m_extractToAct->setVisible(archives);
    m_extractHereAct->setEnabled(extractable && writable);
    m_extractToAct->setEnabled(extractable);
    m_undoAct->setEnabled(m_ops->canUndo());
    m_undoAct->setText(m_ops->undoText());
    m_redoAct->setEnabled(m_ops->canRedo());
    m_redoAct->setText(m_ops->redoText());
    const bool toOther = hasSelection && !inTrash && !otherPaneFolder().isEmpty();
    m_copyToOtherAct->setEnabled(toOther);
    m_moveToOtherAct->setEnabled(toOther && changeable);
#if defined(ARGONAUT_DBUS)
    if (m_chooser) // an Open/Save window: its commands stay off
        m_chooser->keepActionsOff();
#endif
}

void MainWindow::showContextMenu(const QPoint &pos)
{
    const QModelIndex index = m_pane->view->indexAt(pos);
    QItemSelectionModel *selection = m_pane->view->selectionModel();
    if (index.isValid() && !selection->isSelected(index))
        selection->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect);
    else if (!index.isValid())
        selection->clearSelection();
    updateEditActions();

    QMenu *menu = makeMenu();
    menu->setAttribute(Qt::WA_DeleteOnClose);
    m_contextFolder = m_pane->current;
    if (index.isValid() && m_pane->model->isDir(toSource(index)))
        m_contextFolder = m_pane->model->filePath(toSource(index));
    if (index.isValid() && m_pane->model->isTrash()) {
        menu->addAction(m_restoreAct);
        menu->addAction(m_deleteAct);
        menu->addSeparator();
        menu->addAction(m_openAct);
        menu->addAction(m_copyAct);
    } else if (!index.isValid() && m_pane->model->isTrash()) {
        menu->addAction(m_restoreAllAct);
        menu->addAction(m_emptyTrashAct);
        menu->addAction(m_hiddenAct);
    } else if (index.isValid()) {
        menu->addAction(m_openAct);
        if (selectedViewIndexes().size() == 1)
            menu->addAction(m_previewAct);
        const auto verdict = ContentType::Verdict(index.data(DirectoryModel::VerdictRole).toInt());
        if ((verdict == ContentType::Verdict::Mismatch || verdict == ContentType::Verdict::Dangerous)
            && selectedViewIndexes().size() == 1) {
            const QString path = m_pane->model->filePath(toSource(index));
            const QString fixed = ContentType::fixedName(QFileInfo(path).fileName(),
                                                         index.data(DirectoryModel::ContentMimeRole).toString());
            if (!fixed.isEmpty())
                menu->addAction(tr("Rename to “%1”").arg(fixed), this, [this, path] { fixExtension(path); });
        }
        const QStringList files = selectedFiles();
        if (!m_pane->model->isDir(toSource(index)) && OpenWith::isSupported() && !files.isEmpty())
            menu->addMenu(openWithMenu(files));
        addExtensionCommands(menu, selectedPaths());
        if ((m_pane->model->isSearch() || m_pane->model->isVirtual()) && selectedViewIndexes().size() == 1)
            menu->addAction(m_openLocationAct);
        if (m_pane->model->isDir(toSource(index)))
            menu->addAction(m_openInNewTabAct);
        if (m_openTerminalHereAct && m_pane->model->isDir(toSource(index)))
            menu->addAction(m_openTerminalHereAct);
        if (FileJob::archivesSupported()) {
            menu->addSeparator();
            if (m_extractHereAct->isEnabled() || m_extractToAct->isEnabled()) {
                menu->addAction(m_extractHereAct);
                menu->addAction(m_extractToAct);
            }
            menu->addAction(m_compressAct);
        }
        menu->addSeparator();
        m_starAct->setText(selectionIsStarred() ? tr("Unstar") : tr("Star"));
        menu->addAction(m_starAct);
        if (m_pane->model->place() == DirectoryModel::Place::Recent)
            menu->addAction(m_removeRecentAct);
        menu->addSeparator();
        menu->addAction(m_renameAct);
        menu->addSeparator();
        menu->addAction(m_cutAct);
        menu->addAction(m_copyAct);
        if (m_split) {
            menu->addAction(m_copyToOtherAct);
            menu->addAction(m_moveToOtherAct);
        }
        if (QMenu *scripts = scriptsMenu(selectedPaths())) {
            menu->addSeparator();
            menu->addMenu(scripts);
        }
        menu->addSeparator();
        menu->addAction(m_trashAct);
        menu->addAction(m_deleteAct);
        menu->addSeparator();
        menu->addAction(m_propertiesAct);
    } else {
        menu->addAction(m_newFolderAct);
        menu->addAction(m_newFileAct);
        menu->addMenu(m_newDocumentMenu);
        menu->addSeparator();
        if (m_openTerminalHereAct) {
            menu->addAction(m_openTerminalHereAct);
            menu->addSeparator();
        }
        menu->addAction(m_pasteAct);
        menu->addAction(m_undoAct);
        menu->addAction(m_redoAct);
        menu->addSeparator();
        menu->addAction(m_copyLocationAct);
        menu->addAction(m_hiddenAct);
        menu->addSeparator();
        menu->addAction(m_organizeAct);
        addExtensionCommands(menu, {});
        if (QMenu *scripts = scriptsMenu({})) {
            menu->addSeparator();
            menu->addMenu(scripts);
        }
        menu->addSeparator();
        menu->addAction(m_propertiesAct);
    }
    menu->popup(m_pane->view->viewport()->mapToGlobal(pos));
}

// The selected items that aren't folders.
QStringList MainWindow::selectedFiles() const
{
    QStringList files;
    for (const QString &path : selectedPaths()) {
        if (!QFileInfo(path).isDir())
            files << path;
    }
    return files;
}

// True when something is selected and all of it is starred.
bool MainWindow::selectionIsStarred() const
{
    const QStringList paths = selectedPaths();
    return !paths.isEmpty() && std::all_of(paths.cbegin(), paths.cend(), [](const QString &path) {
        return Starred::instance().isStarred(path);
    });
}

QStringList MainWindow::selectedArchives() const
{
    QStringList archives;
    for (const QModelIndex &index : selectedViewIndexes()) {
        const ContentType::Result r = DirectoryModel::contentResult(index);
        // The real type: a ".zip" that is really 7z still extracts; a
        // program named ".zip" doesn't.
        if (r.verdict == ContentType::Verdict::Dangerous || m_pane->model->isDir(toSource(index))
            || !FileOperations::isArchive(ContentType::effectiveMime(r)))
            return {};
        archives << m_pane->model->filePath(toSource(index));
    }
    return archives;
}

void MainWindow::compressSelection()
{
    const QStringList paths = selectedPaths();
    if (paths.isEmpty() || !FileJob::archivesSupported())
        return;
    CompressDialog dialog(paths, m_pane->current, this);
    if (dialog.exec() == QDialog::Accepted)
        m_ops->compress(paths, m_pane->current, dialog.archiveName(), dialog.format());
}

void MainWindow::onJobFinished(FileJob *job)
{
    refreshGitAll();
    const QList<TransferResult> results = job->results();
    const QStringList errors = job->errors();
    const int done = int(results.size());
    const bool userAction = job->property(FileOperations::UserActionProperty).toBool();
    // Results can be in sub-folders, which aren't watched: search again.
    for (int i = 0; i < m_paneCount; ++i) {
        PaneScope scope(this, i);
        if (m_pane->model->isSearch())
            m_pane->model->refresh();
    }
    // A moved item takes its star (and those of what's inside) with it.
    if (job->kind() == FileJob::Kind::Move) {
        for (const TransferResult &r : results)
            Starred::instance().renamed(r.source, r.target);
    }
    m_analyzeView->applyJob(job->kind(), results);
    if (m_analyzing && m_analyzeView->isEmpty())
        m_analyzeView->setFolder(m_pane->current); // the shown folder itself went: measure again

    if (job->isCanceled()) {
        showMessage(tr("Canceled"));
    } else if (!userAction) {
        if (done > 0)
            showMessage(job->property(FileOperations::HistoryProperty).toString() == QLatin1String("redo")
                            ? tr("Redone")
                            : tr("Undone"));
    } else if (job->kind() == FileJob::Kind::Trash && done > 0) {
        const QString text =
            done == 1 ? tr("“%1” moved to the Trash").arg(QFileInfo(results.first().source).fileName())
                      : tr("%n items moved to the Trash", nullptr, done);
        m_toast->showMessage(text, tr("Undo"), [this] { m_ops->undo(); });
    } else if (job->kind() == FileJob::Kind::Delete && done > 0) {
        showMessage(done == 1 ? tr("1 item deleted") : tr("%1 items deleted").arg(done));
    } else if (job->kind() == FileJob::Kind::Extract && done > 0) {
        showMessage(done == 1 ? tr("Extracted “%1”").arg(QFileInfo(results.first().target).fileName())
                              : tr("%1 archives extracted").arg(done));
    } else if (job->kind() == FileJob::Kind::Compress && done > 0) {
        showMessage(tr("Created “%1”").arg(QFileInfo(results.first().target).fileName()));
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

    // Highlight what was pasted (or put back by an undo) in each pane
    // showing that folder: with F5/F6 that's the other pane.
    if (job->kind() != FileJob::Kind::Trash) {
        for (int i = 0; i < m_paneCount; ++i) {
            PaneScope scope(this, i);
            QStringList here;
            for (const TransferResult &r : results) {
                if (QFileInfo(r.target).absolutePath() == m_pane->current)
                    here.append(r.target);
            }
            if (!here.isEmpty())
                selectPaths(here);
        }
    }

    if (!errors.isEmpty()) {
        QString text = errors.mid(0, 10).join('\n');
        if (errors.size() > 10)
            text += '\n' + tr("…and %n more", nullptr, int(errors.size() - 10));
        QMessageBox::warning(this, tr("Some items couldn't be processed"), text);
    }
}

// Defined for every build (the header declares it always); only the Open/Save
// part needs D-Bus.
void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
#if defined(ARGONAUT_DBUS)
    if (m_chooser) // after Qt's own show(), which clears the property
        QTimer::singleShot(0, m_chooser, &ChooserMode::attachToParent);
#endif
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
#if defined(ARGONAUT_DBUS)
    if (m_chooser) { // an Open/Save window: no answer yet means Cancel
        m_chooser->cancel();
        QMainWindow::closeEvent(event);
        return; // the file manager's own settings stay as they were
    }
#endif
    if (paneIndex() != 0)
        activatePane(0); // settings describe the first pane
    saveSettings();
    QMainWindow::closeEvent(event);
}

void MainWindow::selectPaths(const QStringList &paths)
{
    m_pane->pendingSelection = paths;
    m_pane->pendingSince.start();
    applyPendingSelection();
}

void MainWindow::applyPendingSelection()
{
    if (m_pane->pendingSelection.isEmpty())
        return;
    if (m_pane->pendingSince.elapsed() > 5000) { // give up quietly
        m_pane->pendingSelection.clear();
        return;
    }

    QItemSelection selection;
    QModelIndex first;
    int found = 0;
    for (const QString &path : std::as_const(m_pane->pendingSelection)) {
        const QModelIndex index = toView(m_pane->model->indexForPath(path));
        if (!index.isValid())
            continue;
        selection.select(index, index.siblingAtColumn(DirectoryModel::ColumnCount - 1));
        if (!first.isValid())
            first = index;
        ++found;
    }
    if (!first.isValid())
        return;

    QItemSelectionModel *model = m_pane->view->selectionModel();
    model->select(selection, QItemSelectionModel::ClearAndSelect);
    model->setCurrentIndex(first, QItemSelectionModel::NoUpdate);
    m_pane->view->scrollTo(first);

    if (found == m_pane->pendingSelection.size())
        m_pane->pendingSelection.clear();

    if (!m_pane->pendingRename.isEmpty()) {
        const QModelIndex index = toView(m_pane->model->indexForPath(m_pane->pendingRename));
        if (index.isValid()) {
            m_pane->pendingRename.clear();
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
    file->addMenu(m_newDocumentMenu);
    file->addSeparator();
    file->addAction(m_organizeAct);
    file->addSeparator();
    file->addAction(m_openAct);
    file->addAction(m_propertiesAct);
    file->addSeparator();
    if (FileJob::archivesSupported()) {
        file->addAction(m_compressAct);
        file->addAction(m_extractHereAct);
        file->addAction(m_extractToAct);
        file->addSeparator();
    }
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
    edit->addAction(m_redoAct);
    edit->addSeparator();
    edit->addAction(m_copyToOtherAct);
    edit->addAction(m_moveToOtherAct);
    edit->addSeparator();
    edit->addAction(m_cutAct);
    edit->addAction(m_copyAct);
    edit->addAction(m_pasteAct);
    edit->addSeparator();
    edit->addAction(m_selectAllAct);
    edit->addAction(m_selectMatchingAct);
    edit->addAction(m_invertSelectionAct);
    edit->addSeparator();
    edit->addAction(m_renameAct);
    edit->addAction(m_trashAct);
    edit->addAction(m_deleteAct);
    if (Trash::isSupported())
        edit->addAction(m_restoreAct);
    edit->addSeparator();
    edit->addAction(m_copyLocationAct);
    edit->addSeparator();
    edit->addAction(m_extensionsAct);
    edit->addAction(m_prefsAct);

    QMenu *view = addMenu(tr("&View"));
    view->addAction(m_gridAct);
    view->addAction(m_listAct);
    view->addAction(m_analyzeAct);
    view->addAction(m_analyzeFilesAct);
    view->addSeparator();
    view->addAction(m_splitAct);
    view->addSeparator();
    view->addAction(m_searchContentsAct);
    view->addAction(m_paletteAct);
    view->addSeparator();
    view->addAction(m_zoomInAct);
    view->addAction(m_zoomOutAct);
    view->addAction(m_zoomResetAct);
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
    go->addAction(m_connectAct);
    go->addSeparator();
    go->addAction(m_locationAct);
    go->addAction(m_searchAct);

    QMenu *help = addMenu(tr("&Help"));
    help->addAction(m_guideAct);
    help->addAction(m_shortcutsAct);
    help->addSeparator();
    help->addAction(m_aboutAct);

    m_menuBar->installEventFilter(this);
    return m_menuBar;
}

// ----------------------------------------------------- new items, rename ---

void MainWindow::createNewItem(bool folder)
{
    if (m_pane->current.isEmpty() || !QFileInfo(m_pane->current).isWritable()) {
        showMessage(tr("You can't create items in this folder"));
        return;
    }
    const QDir dir(m_pane->current);
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
    m_ops->recordUndo(FileJob::Kind::Copy, {{QString(), path}}, folder ? tr("New Folder") : tr("New File"));
    m_pane->pendingRename = path;
    selectPaths({path});
}

void MainWindow::renameSelection()
{
    const QModelIndexList selected = selectedViewIndexes();
    if (selected.size() == 1)
        startRename(selected.first());
    else if (selected.size() > 1)
        batchRename(selectedPaths());
}

void MainWindow::batchRename(const QStringList &paths)
{
    m_rename->cancel();
    BatchRenameDialog dialog(paths, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QList<Naming::RenameItem> items = dialog.items();
    QString error;
    const QList<TransferResult> steps = Naming::applyBatch(items, &error);
    if (steps.isEmpty()) {
        if (!error.isEmpty())
            showMessage(error + tr(" — nothing was renamed"));
        return;
    }
    for (const TransferResult &step : steps)
        Starred::instance().renamed(step.source, step.target);
    // Undo runs the steps backwards, so chains and swaps come back too.
    m_ops->recordUndo(FileJob::Kind::Move, steps, tr("Rename"));
    QStringList renamed;
    for (const Naming::RenameItem &item : items)
        renamed << QFileInfo(item.path).absolutePath() + '/' + item.newName;
    selectPaths(renamed);
    showMessage(renamed.size() == 1 ? tr("Renamed 1 item") : tr("Renamed %1 items").arg(renamed.size()));
}

// ------------------------------------------------------- selection tools ---

void MainWindow::selectMatching()
{
    if (m_analyzing || !m_pane->view)
        return;
    bool ok = false;
    const QString pattern = QInputDialog::getText(
        this, tr("Select Items Matching"),
        tr("Pattern, e.g. *.jpg or IMG_20??* (text without * or ? matches names containing it):"),
        QLineEdit::Normal, m_lastPattern, &ok);
    if (!ok || pattern.trimmed().isEmpty())
        return;
    m_lastPattern = pattern.trimmed();

    QItemSelection selection;
    QModelIndex first;
    const int rows = m_pane->proxy->rowCount();
    for (int row = 0; row < rows; ++row) {
        const QModelIndex index = m_pane->proxy->index(row, 0);
        if (!Naming::matchesPattern(m_pane->model->fileName(toSource(index)), m_lastPattern))
            continue;
        selection.select(index, index.siblingAtColumn(DirectoryModel::ColumnCount - 1));
        if (!first.isValid())
            first = index;
    }
    QItemSelectionModel *model = m_pane->view->selectionModel();
    model->select(selection, QItemSelectionModel::ClearAndSelect);
    if (!first.isValid()) {
        showMessage(tr("No items match “%1”").arg(m_lastPattern));
        return;
    }
    model->setCurrentIndex(first, QItemSelectionModel::NoUpdate);
    m_pane->view->scrollTo(first);
    m_pane->view->setFocus();
}

void MainWindow::invertSelection()
{
    if (m_analyzing || !m_pane->view)
        return;
    const int rows = m_pane->proxy->rowCount();
    if (rows == 0)
        return;
    QItemSelection all;
    all.select(m_pane->proxy->index(0, 0), m_pane->proxy->index(rows - 1, DirectoryModel::ColumnCount - 1));
    m_pane->view->selectionModel()->select(all, QItemSelectionModel::Toggle);
    m_pane->view->setFocus();
}

// ------------------------------------------------------- new from template ---

void MainWindow::fillNewDocumentMenu()
{
    m_newDocumentMenu->clear();
    for (QMenu *old : m_newDocumentMenu->findChildren<QMenu *>(Qt::FindDirectChildrenOnly))
        old->deleteLater(); // sub-menus from last time
    const QString folder = Templates::folder();
    const QList<Templates::Entry> entries = Templates::list(folder);
    std::function<void(QMenu *, const QList<Templates::Entry> &)> fill =
        [this, &fill](QMenu *menu, const QList<Templates::Entry> &list) {
            QMimeDatabase mimes;
            QFileIconProvider icons;
            for (const Templates::Entry &entry : list) {
                if (entry.isFolder) {
                    // Owned by the top menu, so the next fill deletes it.
                    auto *sub = new QMenu(m_newDocumentMenu);
                    sub->setWindowFlags(sub->windowFlags() | Qt::FramelessWindowHint
                                        | Qt::NoDropShadowWindowHint);
                    sub->setAttribute(Qt::WA_TranslucentBackground);
                    sub->setTitle(entry.name);
                    fill(sub, entry.children);
                    menu->addMenu(sub);
                    continue;
                }
                QAction *action = menu->addAction(entry.name, this, [this, path = entry.path] {
                    createFromTemplate(path);
                });
                action->setIcon(icons.icon(QFileInfo(entry.path)));
            }
        };
    fill(m_newDocumentMenu, entries);
    if (entries.isEmpty()) {
        QAction *none = m_newDocumentMenu->addAction(tr("No Templates"));
        none->setEnabled(false);
    }
    m_newDocumentMenu->addSeparator();
    if (!folder.isEmpty()) {
        m_newDocumentMenu->addAction(tr("Open Templates Folder"), this, [this, folder] { navigateTo(folder); });
    } else {
        // Nothing to open yet: say where documents to offer here belong.
        QAction *hint = m_newDocumentMenu->addAction(
            tr("Put files in %1 to see them here").arg(QDir::toNativeSeparators(QDir::homePath() + QStringLiteral("/Templates"))));
        hint->setEnabled(false);
    }
}

void MainWindow::createFromTemplate(const QString &templatePath)
{
    if (m_pane->current.isEmpty() || !QFileInfo(m_pane->current).isWritable()) {
        showMessage(tr("You can't create items in this folder"));
        return;
    }
    QString error;
    const QString path = Templates::create(templatePath, m_pane->current, &error);
    if (path.isEmpty()) {
        showMessage(error);
        return;
    }
    // Undo trashes it (same as undoing a copy).
    m_ops->recordUndo(FileJob::Kind::Copy, {{templatePath, path}}, tr("New Document"));
    m_pane->pendingRename = path;
    selectPaths({path});
}

// F2: the text field over the item's name is InlineRename's; the window
// says where the name is and what follows a rename.
void MainWindow::startRename(const QModelIndex &viewIndex)
{
    const QModelIndex source = toSource(viewIndex);
    if (!source.isValid()) {
        m_rename->cancel();
        return;
    }
    m_pane->view->scrollTo(viewIndex);
    m_rename->start(m_pane->view->viewport(), m_pane->model->filePath(source), m_pane->model->isDir(source),
                    m_pane->grid ? Qt::AlignCenter : Qt::AlignLeft | Qt::AlignVCenter);
}

// Where `path`'s name is drawn in the visible view; null if it isn't listed.
QRect MainWindow::nameRectOf(const QString &path) const
{
    const QModelIndex index = toView(m_pane->model->indexForPath(path));
    if (!index.isValid())
        return {};
    const FileItemDelegate *delegate = m_pane->grid ? m_pane->gridDelegate : m_pane->listDelegate;
    return delegate->nameEditorRect(m_pane->view->visualRect(index), m_pane->view->fontMetrics(), index);
}

// ---------------------------------------------------------- file views ---

void MainWindow::setupFileView(QAbstractItemView *view)
{
    view->setModel(m_pane->proxy);
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
    connect(view->verticalScrollBar(), &QScrollBar::valueChanged, m_rename,
            &InlineRename::reposition);
}

QModelIndexList MainWindow::selectedViewIndexes() const
{
    QModelIndexList items;
    if (m_analyzing)
        return items; // the file view is hidden: its selection mustn't be acted on
    for (const QModelIndex &index : m_pane->iconView->selectionModel()->selectedIndexes()) {
        if (index.column() == 0)
            items.append(index);
    }
    return items;
}

bool MainWindow::isFileViewport(const QObject *object) const
{
    return (m_pane->iconView && object == m_pane->iconView->viewport())
           || (m_pane->detailView && object == m_pane->detailView->viewport());
}

void MainWindow::sortBy(int column, Qt::SortOrder order)
{
    m_pane->detailView->sortByColumn(column, order); // updates the header and sorts the model
    updateSortActions();
}

void MainWindow::updateSortActions()
{
    if (inBackground())
        return;
    if (m_sortColumnActs.isEmpty())
        return;
    const int column = m_pane->model->sortColumn();
    if (column >= 0 && column < m_sortColumnActs.size())
        m_sortColumnActs.at(column)->setChecked(true);
    (m_pane->model->sortOrder() == Qt::AscendingOrder ? m_sortAscAct : m_sortDescAct)->setChecked(true);
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
    m_pane->gridDelegate->setDropTarget(viewIndex);
    m_pane->listDelegate->setDropTarget(viewIndex);
    m_pane->view->viewport()->update();
}

bool MainWindow::handleViewDrag(QAbstractItemView *view, QEvent *event)
{
    if (event->type() == QEvent::DragLeave) {
        setDropTarget({});
        m_dragScrollTimer->stop();
        m_springTimer->stop();
        m_springPath.clear();
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
    QString targetDir = m_pane->current;
    if (target.isValid()) {
        const QModelIndex source = toSource(target);
        const QString path = m_pane->model->filePath(source);
        if (m_pane->model->isDir(source) && !sources.contains(path))
            targetDir = path;
        else
            target = QModelIndex();
    }
    const Qt::DropAction action =
        dropActionFor(sources, targetDir, drop->modifiers(), drop->possibleActions());

    if (event->type() == QEvent::Drop) {
        setDropTarget({});
        m_dragScrollTimer->stop();
        m_springTimer->stop();
        m_springPath.clear();
        if (action == Qt::IgnoreAction) {
            drop->ignore();
            return true;
        }
        drop->setDropAction(action);
        drop->accept();
        performDrop(sources, targetDir, action);
        return true;
    }

    // DragEnter / DragMove: feedback only. Holding still over a folder opens it.
    const QString spring = target.isValid() ? targetDir : QString();
    if (spring != m_springPath) {
        m_springPath = spring;
        m_springPane = qMax(0, paneOf(view->viewport()));
        if (spring.isEmpty())
            m_springTimer->stop();
        else
            m_springTimer->start();
    }
    setDropTarget(action == Qt::IgnoreAction ? QModelIndex() : target.siblingAtColumn(0));
    const int edge = 32;
    const int height = view->viewport()->height();
    m_dragScrollStep = pos.y() < edge ? -20 : (pos.y() > height - edge ? 20 : 0);
    m_dragScrollView = view;
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
    if (event->type() == QEvent::DragLeave || event->type() == QEvent::Drop) {
        m_springTimer->stop();
        m_springPath.clear();
    }
    if (event->type() == QEvent::DragLeave) {
        syncSidebarSelection();
        return true;
    }
    auto *drop = static_cast<QDropEvent *>(event);
    const QStringList sources = localPaths(drop->mimeData());
    QListWidgetItem *item = m_sidebar->itemAt(drop->position().toPoint());

    // Folders dropped on empty space (or a separator) become bookmarks.
    if (!item || Sidebar::isSeparator(item)) {
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
            m_sidebar->refreshSoon();
            updateBookmarkAction();
        }
        return true;
    }
    const QString targetDir = QDir::cleanPath(Sidebar::pathOf(item));
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
    if (targetDir != m_springPath && !Trash::isFilesDir(targetDir)) {
        m_springPath = targetDir; // held still: open it, like Nautilus
        m_springPane = paneIndex();
        m_springTimer->start();
    }
    drop->setDropAction(action);
    drop->accept();
    return true;
}

// ------------------------------------------------------------ terminal ---

#if defined(ARGONAUT_TERMINAL)

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
void MainWindow::showTerminal(const QString &folder)
{
    // Recent / Starred aren't folders: the shell starts at home.
    const QString directory = DirectoryModel::isVirtualPath(folder) ? QDir::homePath() : folder;
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
        m_pane->view->setFocus();
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

// The tabs themselves are TabStrip's. These two say what a tab is made of in
// this window: its folder, history, selection and scroll position.
TabStrip::Tab MainWindow::captureTab() const
{
    TabStrip::Tab tab;
    tab.path = m_pane->current;
    tab.back = m_pane->back;
    tab.forward = m_pane->forward;
    tab.selection = selectedPaths();
    tab.scroll = m_pane->view->verticalScrollBar()->value();
    return tab;
}

void MainWindow::restoreTab(const TabStrip::Tab &tab)
{
    if (tab.path != m_pane->current) {
        if (!navigateTo(tab.path, false)) // its folder may be gone by now
            navigateTo(QDir::homePath(), false);
    } else {
        setWindowTitle(tr("%1 — Argonaut").arg(titleFor(m_pane->current)));
    }
    m_pane->back = tab.back;
    m_pane->forward = tab.forward;
    updateActions();
    if (!tab.selection.isEmpty())
        selectPaths(tab.selection);
    else
        m_pane->view->selectionModel()->clearSelection();
    if (m_pane->model->isLoading())
        m_pane->pendingScroll = tab.scroll; // applied when the folder is listed
    else
        m_pane->view->verticalScrollBar()->setValue(tab.scroll);
}

void MainWindow::openTab(const QString &path, bool activate)
{
    // Recent and Starred are names, not paths: cleaning "recent:///" breaks it.
    const QString target = DirectoryModel::isVirtualPath(path) ? path : QDir::cleanPath(path);
    m_tabBar->open(target, activate);
    if (!activate)
        showMessage(tr("Opened “%1” in a new tab").arg(titleFor(target)));
}

// ------------------------------------------------------------ type-ahead ---

// Letters typed in the file view build a search; a pause of a second
// starts a new one. Returns true if the key was used.
bool MainWindow::typeAheadKey(QKeyEvent *event)
{
    if (event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))
        return false;
    const bool active = !m_typeAhead.isEmpty() && m_typeAheadTimer.isValid()
                        && m_typeAheadTimer.elapsed() < 1000;
    if (event->key() == Qt::Key_Backspace && active) {
        m_typeAhead.chop(1);
        m_typeAheadTimer.restart();
        if (!m_typeAhead.isEmpty())
            typeAheadFind();
        return true;
    }
    if (event->key() == Qt::Key_Escape && active) {
        m_typeAhead.clear();
        showMessage(QString());
        return true;
    }
    const QString text = event->text();
    if (text.isEmpty() || !text.at(0).isPrint())
        return false;
    if (text == QLatin1String(" ") && !active)
        return false; // a lone space keeps its usual meaning
    if (!active)
        m_typeAhead.clear();
    m_typeAhead += text;
    m_typeAheadTimer.restart();
    typeAheadFind();
    return true;
}

// Select the first item whose name starts with the typed text, or failing
// that contains it (case-insensitive).
void MainWindow::typeAheadFind()
{
    QModelIndex hit;
    const int rows = m_pane->proxy->rowCount();
    for (const bool prefix : {true, false}) {
        for (int row = 0; row < rows && !hit.isValid(); ++row) {
            const QModelIndex index = m_pane->proxy->index(row, 0);
            const QString name = index.data().toString();
            if (prefix ? name.startsWith(m_typeAhead, Qt::CaseInsensitive)
                       : name.contains(m_typeAhead, Qt::CaseInsensitive))
                hit = index;
        }
        if (hit.isValid())
            break;
    }
    if (hit.isValid()) {
        QItemSelectionModel *selection = m_pane->view->selectionModel();
        selection->select(QItemSelection(hit, hit.siblingAtColumn(DirectoryModel::ColumnCount - 1)),
                          QItemSelectionModel::ClearAndSelect);
        selection->setCurrentIndex(hit, QItemSelectionModel::NoUpdate);
        m_pane->view->scrollTo(hit);
        showMessage(tr("Find: %1").arg(m_typeAhead));
    } else {
        showMessage(tr("Find: %1 — no match").arg(m_typeAhead));
    }
}

// ----------------------------------------------------------- open with ---

QString MainWindow::effectiveMimeAt(const QModelIndex &sourceIndex) const
{
    return ContentType::effectiveMime(DirectoryModel::contentResult(sourceIndex));
}

bool MainWindow::quickLookVisible() const
{
    return m_quickLook && m_quickLook->isVisible();
}

void MainWindow::hideQuickLook()
{
    if (!quickLookVisible())
        return;
    m_previewWidth = m_quickLook->width();
    m_quickLook->hide();
}

void MainWindow::toggleQuickLook()
{
    if (quickLookVisible()) {
        hideQuickLook();
        return;
    }
    if (m_analyzing)
        return;
    if (!m_quickLook) {
        m_quickLook = new QuickLook(m_thumbnailer, m_previewSplitter);
        m_previewSplitter->addWidget(m_quickLook);
        m_previewSplitter->setStretchFactor(1, 0);
        connect(m_quickLook, &QuickLook::closeRequested, this, &MainWindow::hideQuickLook);
        connect(m_quickLook, &QuickLook::openRequested, this, [this](const QString &path) {
            if (QFileInfo(path).isDir())
                navigateTo(path);
            else
                openFile(path);
        });
    }
    // The width it had last time, else two fifths of the room; the files
    // always keep a usable part.
    const int total = m_previewSplitter->width();
    int width = m_previewWidth > 0 ? m_previewWidth : total * 2 / 5;
    width = qBound(m_quickLook->minimumWidth(), width, qMax(m_quickLook->minimumWidth(), total - 320));
    m_quickLook->show();
    m_previewSplitter->setSizes({total - width, width});
    updateQuickLook();
    m_pane->view->setFocus(); // keys keep going to the file view
}

void MainWindow::updateQuickLook()
{
    if (!m_quickLook)
        return;
    const QModelIndex view = m_pane->view->currentIndex();
    if (!view.isValid()) {
        m_quickLook->clear();
        return;
    }
    const QModelIndex source = toSource(view.siblingAtColumn(0));
    QuickLook::Item item;
    item.path = m_pane->model->filePath(source);
    if (item.path == m_quickLook->path() && m_quickLook->isVisible())
        return;
    item.name = m_pane->model->fileName(source);
    item.verdict = ContentType::Verdict(source.data(DirectoryModel::VerdictRole).toInt());
    item.mimeType = m_pane->model->isDir(source) ? QStringLiteral("inode/directory") : effectiveMimeAt(source);
    item.icon = source.data(Qt::DecorationRole).value<QIcon>();
    static const QMimeDatabase db;
    QStringList details;
    details << (m_pane->model->isDir(source) ? tr("Folder") : db.mimeTypeForName(item.mimeType).comment());
    if (!m_pane->model->isDir(source))
        details << formatSize(m_pane->model->size(source));
    details << QLocale().toString(source.data(DirectoryModel::ModifiedRole).toDateTime(), QLocale::ShortFormat);
    item.details = details.join(QStringLiteral(" · "));
    m_quickLook->showItem(item);
}

void MainWindow::rememberOpened(const QStringList &files)
{
    static const QMimeDatabase db;
    for (const QString &file : files)
        RecentFiles::add(file, db.mimeTypeForFile(file, QMimeDatabase::MatchExtension).name());
}

QMenu *MainWindow::openWithMenu(const QStringList &files)
{
    QMenu *menu = makeMenu();
    menu->setTitle(tr("Open With"));
    static const QMimeDatabase db;
    QString mime = db.mimeTypeForFile(files.first()).name();
    if (mime.isEmpty())
        mime = QStringLiteral("application/octet-stream"); // never ask gio about ""
    QString defaultId;
    const QList<OpenWith::App> apps = OpenWith::appsFor(mime, &defaultId);
    int shown = 0;
    for (const OpenWith::App &app : apps) {
        if (shown++ == 8)
            break; // the rest are in "Other Application…"
        QAction *act = menu->addAction(appIcon(app), app.id == defaultId
                                                         ? tr("%1 (default)").arg(app.name)
                                                         : app.name);
        connect(act, &QAction::triggered, this, [this, app, files] { launchWith(app, files); });
    }
    if (!apps.isEmpty())
        menu->addSeparator();
    menu->addAction(tr("Other Application…"), this, [this, files] { openWithOther(files); });
    return menu;
}

void MainWindow::openWithOther(const QStringList &files)
{
    static const QMimeDatabase db;
    const QMimeType type = db.mimeTypeForFile(files.first());
    const QString mime = type.isValid() ? type.name() : QStringLiteral("application/octet-stream");
    OpenWithDialog dialog(files, mime, type.isValid() ? type.comment() : mime, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const OpenWith::App app = dialog.chosen();
    if (app.desktopFile.isEmpty())
        return;
    if (dialog.makeDefault())
        OpenWith::setDefault(mime, app);
    launchWith(app, files);
}

// Starts `app` with the files, says so, and adds them to Recent.
void MainWindow::launchWith(const OpenWith::App &app, const QStringList &files)
{
    if (!OpenWith::launch(app, files)) {
        showMessage(tr("Couldn't start %1").arg(app.name));
        return;
    }
    rememberOpened(files);
    showMessage(tr("Opening with %1").arg(app.name));
}

// ------------------------------------------------------ real file types ---

void MainWindow::openFile(const QString &path)
{
    const QFileInfo info(path);
    const qint64 mtime = info.lastModified().toMSecsSinceEpoch();
    ContentType::Result result;
    if (!ContentType::Cache::instance().lookup(path, info.size(), mtime, &result)) {
        result = ContentType::inspect(path); // one file: well under a millisecond
        ContentType::Cache::instance().store(path, info.size(), mtime, result);
    }

    if (result.verdict != ContentType::Verdict::Dangerous)
        rememberOpened({path});
    if (result.verdict == ContentType::Verdict::Dangerous) {
        QMessageBox box(QMessageBox::Warning, tr("This file is a program in disguise"),
                        tr("“%1” is named like a document, but it is really a program.")
                            .arg(info.fileName()),
                        QMessageBox::NoButton, this);
        box.setInformativeText(ContentType::describe(result) + QStringLiteral(".\n\n")
                               + tr("Files like this are a common way to spread malware. "
                                    "Argonaut won't run it."));
        QPushButton *keep = box.addButton(tr("Don't Open"), QMessageBox::RejectRole);
        QPushButton *trash = box.addButton(tr("Move to Trash"), QMessageBox::DestructiveRole);
        QPushButton *asNamed = box.addButton(tr("Open as Named"), QMessageBox::AcceptRole);
        asNamed->setToolTip(tr("Open with the app for its name, without running it"));
        box.setDefaultButton(keep);
        box.exec();
        if (box.clickedButton() == trash)
            m_ops->trash({path});
        else if (box.clickedButton() == asNamed)
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        return;
    }

    // Named wrongly or not at all: open with the app for what it really is.
    if ((result.verdict == ContentType::Verdict::Mismatch
         || result.verdict == ContentType::Verdict::Identified)
        && OpenWith::isSupported()) {
        QString defaultId;
        const QList<OpenWith::App> apps = OpenWith::appsFor(result.contentMime, &defaultId);
        for (const OpenWith::App &app : apps) {
            if (app.id == defaultId || defaultId.isEmpty()) {
                if (OpenWith::launch(app, {path})) {
                    showMessage(tr("Opening as %1 with %2")
                                    .arg(QMimeDatabase().mimeTypeForName(result.contentMime).comment(),
                                         app.name));
                    return;
                }
            }
        }
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

// Give a mismatched file the extension its content calls for (undoable).
void MainWindow::fixExtension(const QString &path)
{
    const QFileInfo info(path);
    const ContentType::Result result = ContentType::inspect(path);
    const QString fixed = ContentType::fixedName(info.fileName(), result.contentMime);
    if (fixed.isEmpty())
        return;
    QString target = info.absolutePath() + QLatin1Char('/') + fixed;
    if (QFileInfo::exists(target))
        target = uniqueTargetPath(info.absolutePath(), fixed, false);
    if (!QDir().rename(path, target)) {
        showMessage(tr("Couldn't rename “%1”").arg(info.fileName()));
        return;
    }
    Starred::instance().renamed(path, target);
    m_ops->recordUndo(FileJob::Kind::Move, {{path, target}}, tr("Rename"));
    selectPaths({target});
    showMessage(tr("Renamed to “%1”").arg(QFileInfo(target).fileName()));
}

void MainWindow::keyPressEvent(QKeyEvent *event)
{
#if defined(ARGONAUT_DBUS)
    // Esc that nothing else wanted (rename, search, type-ahead) cancels.
    if (m_chooser && event->key() == Qt::Key_Escape) {
        m_chooser->cancel();
        return;
    }
#endif
    QMainWindow::keyPressEvent(event);
}

// ------------------------------------------------------ Open/Save window ---
#if defined(ARGONAUT_DBUS)

// Turns this window into an Open/Save window for another app. What that
// means (accepting, the one answer, where the window goes) is ChooserMode's;
// here the window only takes off what a dialog doesn't need and tells the
// mode what the user does.
void MainWindow::startChooser(const ChooserRequest &request)
{
    ChooserMode::Host host;
    host.window = this;
    host.selectedPaths = [this] { return selectedPaths(); };
    host.currentFolder = [this] { return m_pane->current; };
    host.inTrash = [this] { return m_pane->model->isTrash(); };
    host.navigateTo = [this](const QString &folder) { return navigateTo(folder); };
    host.showMessage = [this](const QString &text) { showMessage(text); };
    m_chooser = new ChooserMode(request, host, this);

    // What a dialog doesn't need: the menus, terminal, Analyze, split view,
    // tabs, and commands that would open other windows or quit the service.
    // The menu bar stays (empty): its row holds the window buttons at the
    // right and drags or maximises the window like in the file manager.
    if (m_menuBar)
        for (QAction *menu : m_menuBar->actions())
            menu->setVisible(false);
    for (QToolButton *button : {m_terminalButton, m_analyzeButton, m_splitButton, m_menuButton})
        if (button)
            button->hide();
    m_chooser->setUnavailableActions({m_splitAct, m_terminalAct, m_openTerminalHereAct, m_analyzeAct, m_newTabAct,
                                      m_reopenTabAct, m_openInNewTabAct, m_quitAct, m_prefsAct, m_extensionsAct, m_paletteAct,
                                      m_organizeAct, m_copyToOtherAct, m_moveToOtherAct, m_aboutAct, m_guideAct,
                                      m_shortcutsAct});

    m_contentLayout->addWidget(m_chooser->bar());
    auto applyFilter = [this] { m_pane->proxy->setChooserFilter(m_chooser->currentFilter(), m_chooser->request().directory); };
    applyFilter();
    connect(m_chooser, &ChooserMode::filterChanged, this, applyFilter);

    const auto selection = request.multiple ? QAbstractItemView::ExtendedSelection
                                            : QAbstractItemView::SingleSelection;
    m_pane->iconView->setSelectionMode(selection);
    m_pane->detailView->setSelectionMode(selection);
    connect(m_pane->view->selectionModel(), &QItemSelectionModel::selectionChanged, m_chooser, &ChooserMode::selectionChanged);

    connect(m_chooser, &ChooserMode::finished, this, [this](uint response, const QVariantMap &results) {
        emit chooserFinished(response, results);
        QTimer::singleShot(0, this, &QWidget::close); // not from inside closeEvent
    });

    m_chooser->placeWindow();
    if (const QString file = m_chooser->fileToShow(); !file.isEmpty())
        showItems({file});
    else
        navigateTo(m_chooser->startFolder());
    refreshChrome(); // the title
    m_chooser->selectionChanged();
}

void MainWindow::cancelChooser()
{
    if (m_chooser)
        m_chooser->cancel();
}
#endif
