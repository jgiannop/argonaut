#pragma once

#include "Theme.h"

#include <QAbstractItemModel>
#include <QElapsedTimer>
#include <QHash>
#include "TabStrip.h"

#include <QMainWindow>
#include <QPointer>
#include <QStack>
#include <QString>

#if defined(ARGONAUT_DBUS)
#include "FileChooserRequest.h"
#include <optional>
class ChooserMode;
class QVBoxLayout;
class QWindow;
#endif

namespace OpenWith {
struct App;
}
class DirectoryModel;
class InlineRename;
class MountActions;
class Sidebar;
class AnalyzeView;
class QuickLook;
namespace Extensions {
struct PreviewReply;
}
namespace FileRules {
struct Question;
}
class FileFilterProxy;
class FileItemDelegate;
class FileJob;
class FileOperations;
class OperationsPanel;
class TerminalWidget;
class Toast;
class PathBar;
class Thumbnailer;
class QAction;
class QLabel;
class QLineEdit;
class QAbstractItemView;
class QListView;
class QTreeView;
class QListWidget;
class QMenu;
class QMenuBar;
class QModelIndex;
class QSplitter;
class QStackedWidget;
class QTabBar;
class QTimer;
class QToolButton;
class QKeyEvent;
class QHBoxLayout;
class QVBoxLayout;

class QHeaderView;

class MainWindow : public QMainWindow
{
    Q_OBJECT
    friend class TestMainWindow; // tests/tst_mainwindow.cpp

public:
    explicit MainWindow(QWidget *parent = nullptr);

    // Show the folder at `path`. Returns false if it doesn't exist.
    bool navigateTo(const QString &path, bool addToHistory = true);
    // The first folder here, the others in tabs ("Open folder" from other apps).
    void openFolders(const QStringList &folders);
    // Files or folders shown selected in the folder that holds them ("Show
    // in folder"); items in other folders open in tabs.
    void showItems(const QStringList &items);

#if defined(ARGONAUT_DBUS)
    // Open/Save for another app (`argonaut --portal`): call right after
    // construction. The window then answers once, through chooserFinished
    // (FileChooserPortal's response and results), and closes.
    void startChooser(const ChooserRequest &request);
    void cancelChooser(); // the app gave up (Request.Close)

signals:
    void chooserFinished(uint response, const QVariantMap &results);
#endif

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void changeEvent(QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void setupActions();
    void setupUi();
    QWidget *buildSidebarPane();
    QWidget *buildContentPane();
    QToolButton *makeButton(const QString &toolTip, const char *objectName = "flat");
    // Minimize / maximize / close (client-side decorations), at the end of `row`.
    void addWindowButtons(QHBoxLayout *row);
    QMenu *makeMenu();
    QWidget *buildMenuBar();

    void applyTheme(bool force = false);
    void showSidebarMenu(const QPoint &pos);
    void ejectDrive(const QString &root, const QString &name);
    void toggleBookmark();
    void updateBookmarkAction();
    void loadSettings();
    void saveSettings();
    // Typing in the file view jumps to a matching name.
    bool typeAheadKey(QKeyEvent *event);
    // Open With: a submenu of the recommended apps, and the full chooser.
    QMenu *openWithMenu(const QStringList &files);
    // Open a file by what it really is (ContentType), warning about programs
    // in disguise instead of opening them.
    void openFile(const QString &path);
    void fixExtension(const QString &path);
    void openWithOther(const QStringList &files);
    void launchWith(const OpenWith::App &app, const QStringList &files);
    void typeAheadFind();

    // Tabs (TabStrip): what a tab is made of here, and a new tab.
    TabStrip::Tab captureTab() const;
    void restoreTab(const TabStrip::Tab &tab);
    void openTab(const QString &path, bool activate);
    void syncSidebarSelection();

    void goBack();
    void goForward();
    void goUp();
    void openIndex(const QModelIndex &index);
    void setGridMode(bool grid);
    // The Analyze view: disk usage of the open folder's sub-folders, as a
    // tree (DiskUsageModel). File actions are off while it shows.
    void setAnalyzeMode(bool on);
    void showAnalyzeMenu(const QPoint &pos);
    void setShowHidden(bool show);
    void setSearchActive(bool active);
    void updateFilter();
    void updateActions();

    void setupFileView(QAbstractItemView *view);

    // Drag and drop (drops are handled here, not by the views/model).
    bool handleViewDrag(QAbstractItemView *view, QEvent *event);
    bool handleSidebarDrag(QEvent *event);
    Qt::DropAction dropActionFor(const QStringList &sources, const QString &targetDir,
                                 Qt::KeyboardModifiers modifiers,
                                 Qt::DropActions possible) const;
    void setDropTarget(const QModelIndex &viewIndex);
    QByteArray deviceOf(const QString &path) const;
    QModelIndexList selectedViewIndexes() const; // one index (column 0) per selected item
    bool isFileViewport(const QObject *object) const;
    void sortBy(int column, Qt::SortOrder order);
    void updateSortActions();

    // View (proxy) <-> DirectoryModel indexes.
    QModelIndex toSource(const QModelIndex &viewIndex) const;
    QModelIndex toView(const QModelIndex &sourceIndex) const;

    // File operations
    QStringList selectedPaths() const;
    QStringList selectedFiles() const; // without folders
    bool selectionIsStarred() const;   // something is selected, all of it starred
    void rememberOpened(const QStringList &files); // add to the shared Recent list
    // Quick Look: Space shows the current item; it follows the current item
    // while open.
    void toggleQuickLook();
    void updateQuickLook();
    bool quickLookVisible() const;
    QString effectiveMimeAt(const QModelIndex &sourceIndex) const;
    // Zoom the visible view (grid and list have their own size): +1, -1, or 0 = default.
    void zoom(int step);
    void applyZoom();
    QStringList selectedArchives() const; // empty unless every selected item is an archive
    void compressSelection();
    void openSelection();
    void copySelection(bool cut);
    void paste();
    void trashSelection();
    void deleteSelection(); // permanently, after confirming
    void restoreSelection();
    void emptyTrash();
    bool ask(const FileRules::Question &question); // one of FileRules' questions, as a dialog
    bool confirmPermanentDelete(const QStringList &paths, bool wholeTrash);
    void performDrop(const QStringList &sources, const QString &targetDir, Qt::DropAction action);
    void updateEditActions();
    void showContextMenu(const QPoint &pos);
    void createNewItem(bool folder);
    // New Document ▸ (files in the Templates folder), rebuilt when opened.
    void fillNewDocumentMenu();
    void createFromTemplate(const QString &templatePath);
    // Edit → Select Items Matching (Ctrl+S) and Invert Selection.
    void selectMatching();
    void invertSelection();
    // F2 with several items selected.
    void batchRename(const QStringList &paths);

    // Embedded terminal (bottom panel). Built only with libvterm.
    QWidget *buildTerminalPanel();
    void showTerminal(const QString &directory);
    void hideTerminal();

    // Inline rename (F2, and right after New Folder / New File).
    void renameSelection();
    void startRename(const QModelIndex &viewIndex);
    QRect nameRectOf(const QString &path) const;
    void onJobFinished(FileJob *job);
    // Name clashes: one "Replace?" question at a time (jobs wait meanwhile).
    void askConflict(FileJob *job, const QString &source, const QString &existing);
    void showNextConflict();

    // Highlight `paths` (in the current folder) once the model has them.
    void selectPaths(const QStringList &paths);
    void applyPendingSelection();

    void updateFloatingBar();
    void showMessage(const QString &text);
    void positionFloatingBar();

    // Client-side decorations (frameless window with our own header bars).
    void updateWindowState();
    Qt::Edges edgesAt(const QPoint &pos) const;
    void updateResizeCursor(Qt::Edges edges);

    Theme m_theme;
    QString m_themeId; // empty until the first applyTheme()
    bool m_csd = false;
    bool m_showHidden = false;
    bool m_resizeCursor = false;

    FileOperations *m_ops = nullptr;
    OperationsPanel *m_opsPanel = nullptr;
    Toast *m_toast = nullptr;
    Thumbnailer *m_thumbnailer = nullptr;
    AnalyzeView *m_analyzeView = nullptr;   // Analyze: folder sizes as a tree
    QAction *m_analyzeAct = nullptr;      // Ctrl+3
    QAction *m_analyzeFilesAct = nullptr; // Analyze: also list the biggest files
    QToolButton *m_analyzeButton = nullptr;
    bool m_analyzing = false;
    Sidebar *m_sidebar = nullptr;
    QWidget *m_root = nullptr;
    QSplitter *m_splitter = nullptr;
    QStackedWidget *m_locationStack = nullptr;
    PathBar *m_pathBar = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    QTimer *m_searchTimer = nullptr;     // debounces typing before searching sub-folders
    QAction *m_openLocationAct = nullptr; // search results: open the folder that holds it
    QLabel *m_titleLabel = nullptr;

    QToolButton *m_backButton = nullptr;
    QToolButton *m_forwardButton = nullptr;
    QToolButton *m_searchButton = nullptr;
    QToolButton *m_menuButton = nullptr;
    QToolButton *m_viewButton = nullptr;
    QToolButton *m_minButton = nullptr;
    QToolButton *m_maxButton = nullptr;
    QToolButton *m_closeButton = nullptr;

    QAction *m_backAct = nullptr;
    QAction *m_forwardAct = nullptr;
    QAction *m_upAct = nullptr;
    QAction *m_hiddenAct = nullptr;
    QAction *m_searchAct = nullptr;
    QAction *m_copyLocationAct = nullptr;
    QAction *m_prefsAct = nullptr;
    QAction *m_extensionsAct = nullptr; // Ctrl+Shift+X: the Extensions window
    QAction *m_aboutAct = nullptr;
    QAction *m_paletteAct = nullptr;   // Ctrl+Shift+P: run any command by name
    QAction *m_connectAct = nullptr;   // Connect to Server…
    QAction *m_organizeAct = nullptr;  // Organize with Claude…
    QAction *m_restoreAllAct = nullptr;  // Trash: Restore All
    QToolButton *m_trashRestoreAllButton = nullptr;
    // Dragging: hold still over a folder and it opens (or expands in place).
    QTimer *m_springTimer = nullptr;
    QString m_springPath;
    int m_springPane = 0;
    void springOver(const QString &folder, int pane);
    // Right-click extras: extensions' commands, Scripts ▸.
    void addExtensionCommands(QMenu *menu, const QStringList &selection);
    void showExtensionInfoMenu(const QPoint &globalPos);
    void showExtensionResult(const QString &extension, const QString &title, const Extensions::PreviewReply &content);
    QMenu *scriptsMenu(const QStringList &selection);
    void organizeWithClaude();
    void applyOrganizePlan(const QString &folder, const QList<QPair<QString, QPair<QString, QString>>> &moves);
    QAction *m_searchContentsAct = nullptr; // search also inside files (search/contents)
    QToolButton *m_filterButton = nullptr;  // search filters: type and date
    QActionGroup *m_typeFilters = nullptr;
    QActionGroup *m_dateFilters = nullptr;
    void applySearchFilters();
    class CommandPalette *m_palette = nullptr;
    void showCommandPalette();
    QAction *m_guideAct = nullptr;     // F1: the built-in user guide
    QAction *m_shortcutsAct = nullptr; // Ctrl+?: the guide's shortcuts page
    void showGuide(const QString &topic);
    QAction *m_openAct = nullptr;
    QAction *m_cutAct = nullptr;
    QAction *m_copyAct = nullptr;
    QAction *m_pasteAct = nullptr;
    QAction *m_trashAct = nullptr;
    QAction *m_undoAct = nullptr;
    QAction *m_redoAct = nullptr;         // Ctrl+Shift+Z / Ctrl+Y
    QAction *m_starAct = nullptr;         // Star / Unstar the selection
    QAction *m_previewAct = nullptr;      // Quick Look (Space, handled in eventFilter)
    QuickLook *m_quickLook = nullptr;     // the preview pane at the right; made on first use
    QSplitter *m_previewSplitter = nullptr; // files (and terminal) | preview
    int m_previewWidth = 0;               // the pane's last width (view/previewWidth)
    void hideQuickLook();
    QAction *m_removeRecentAct = nullptr; // Recent: take items off the list
    QAction *m_zoomInAct = nullptr;       // Ctrl++ / Ctrl+= / Ctrl+wheel
    QAction *m_zoomOutAct = nullptr;      // Ctrl+-
    QAction *m_zoomResetAct = nullptr;    // Ctrl+0
    int m_wheelZoom = 0;                  // Ctrl+wheel delta not used yet (touchpads)
    QAction *m_homeAct = nullptr;
    QAction *m_locationAct = nullptr;
    QAction *m_gridAct = nullptr;
    QAction *m_listAct = nullptr;
    QAction *m_newFolderAct = nullptr;
    QAction *m_newFileAct = nullptr;
    QAction *m_renameAct = nullptr;
    QAction *m_selectAllAct = nullptr;
    QAction *m_selectMatchingAct = nullptr;
    QAction *m_invertSelectionAct = nullptr;
    QMenu *m_newDocumentMenu = nullptr;
    QString m_lastPattern; // Select Items Matching remembers it
    QAction *m_closeAct = nullptr;
    QAction *m_quitAct = nullptr;
    QAction *m_sidebarAct = nullptr;
    QList<QAction *> m_sortColumnActs; // by DirectoryModel::Column
    QAction *m_sortAscAct = nullptr;
    QAction *m_sortDescAct = nullptr;

    struct PendingConflict
    {
        QPointer<FileJob> job;
        QString source;
        QString existing;
    };
    QList<PendingConflict> m_conflicts;
    bool m_conflictOpen = false;

    QString m_typeAhead;
    QElapsedTimer m_typeAheadTimer;
    TabStrip *m_tabBar = nullptr;
    QAction *m_newTabAct = nullptr;
    QAction *m_closeWindowAct = nullptr;
    QAction *m_reopenTabAct = nullptr;
    QAction *m_openInNewTabAct = nullptr;

    QAction *m_propertiesAct = nullptr; // Ctrl+I / Alt+Enter
    QAction *m_compressAct = nullptr;     // archives (when built with libarchive)
    QAction *m_extractHereAct = nullptr;
    QAction *m_extractToAct = nullptr;
    QAction *m_bookmarkAct = nullptr;   // Ctrl+D: bookmark / unbookmark this folder
    int m_terminalHeight = 0;           // remembered between sessions

    QAction *m_deleteAct = nullptr;     // Delete Permanently…
    QAction *m_restoreAct = nullptr;    // Restore From Trash
    QAction *m_emptyTrashAct = nullptr; // Empty Trash…
    QWidget *m_trashBar = nullptr;      // shown above the files in the Trash
    QToolButton *m_trashRestoreButton = nullptr;
    QToolButton *m_trashEmptyButton = nullptr;
    QAction *m_terminalAct = nullptr;        // null if built without the terminal
    QAction *m_openTerminalHereAct = nullptr;
    QString m_contextFolder; // folder the context menu's "Open in Terminal" uses
    QSplitter *m_contentSplitter = nullptr;  // file views above, terminal below
    QWidget *m_terminalPanel = nullptr;
    TerminalWidget *m_terminal = nullptr;
    QLabel *m_terminalTitle = nullptr;
    QToolButton *m_terminalButton = nullptr;
    QToolButton *m_terminalCloseButton = nullptr;
    bool m_terminalSized = false;

    QMenuBar *m_menuBar = nullptr;
    QPoint m_menuPressPos; // dragging the window by empty menu bar space
    bool m_menuPressed = false;
    QWidget *m_sidebarPane = nullptr;

    QTimer *m_dragScrollTimer = nullptr; // auto-scroll near the view's edges
    int m_dragScrollStep = 0;
    mutable QHash<QString, QByteArray> m_deviceCache; // folder -> storage device

    MountActions *m_mounts = nullptr; // servers and drives: connect, disconnect, eject
    InlineRename *m_rename = nullptr; // F2: the field over an item's name



    // ----------------------------------------------------- split view ---
    // Each file pane owns its folder, history, model, views and state (Pane).
    // The window's code reaches all of it through m_pane, the pane it is
    // working on: the user's pane (m_activePane), or for a moment the other
    // one, while handling something that happened there (PaneScope).
    struct Pane
    {
        // Widgets (made once by buildPane; never change).
        // model -> proxy -> the two views; `view` is the visible one.
        DirectoryModel *model = nullptr;
        FileFilterProxy *proxy = nullptr;
        FileItemDelegate *gridDelegate = nullptr;
        FileItemDelegate *listDelegate = nullptr;
        QListView *iconView = nullptr;
        QTreeView *detailView = nullptr;
        QStackedWidget *viewStack = nullptr;
        QLabel *floatingBar = nullptr;
        class FadeOverlay *fade = nullptr; // the cross-fade when its folder changes
        QTimer *messageTimer = nullptr;
        QWidget *container = nullptr; // caption + views, in m_paneSplitter
        QLabel *caption = nullptr;    // split view: the pane's folder
        // State.
        QAbstractItemView *view = nullptr; // iconView or detailView, whichever shows
        QString current;                   // the open folder (or a place: "recent:///")
        QStack<QString> back, forward;     // history
        bool grid = true;                  // grid or list
        int gridZoom = 1, listZoom = 1;    // indexes into FileItemDelegate::gridSizes() / listSizes()
        QString message;                   // a temporary text in the floating bar
        // Asked for before the folder was listed, applied when it is:
        QStringList pendingSelection;
        QElapsedTimer pendingSince;
        int pendingScroll = -1;            // a tab's scroll position
        QString pendingRename;             // start renaming this once it appears
        // Recent sorts by "last used"; the folder order to go back to:
        bool recentSort = false;
        int sortColumnBefore = 0;
        Qt::SortOrder sortOrderBefore = Qt::AscendingOrder;
    };
    class PaneScope; // switches to a pane temporarily (see MainWindow.cpp)
    friend class PaneScope;
    Pane m_panes[2];
    int m_paneCount = 0;  // 1, or 2 once the split view was opened
    int m_activePane = 0;         // the pane the user is in
    Pane *m_pane = &m_panes[0];   // the pane being worked on: that one, or the other inside a PaneScope
    int paneIndex() const;        // m_pane's number
    bool inBackground() const;    // working on the pane the user isn't in: leave the header and actions alone
    bool m_split = false;
    QSplitter *m_paneSplitter = nullptr;
    QAction *m_splitAct = nullptr;        // F3
    QAction *m_copyToOtherAct = nullptr;  // F5
    QAction *m_moveToOtherAct = nullptr;  // F6
    QToolButton *m_splitButton = nullptr;
    QPointer<QAbstractItemView> m_dragScrollView;

    QWidget *buildPane(QWidget *parent, int index);
    void activatePane(int index);
    int paneOf(const QObject *object) const;
    void refreshChrome();     // header, title, sidebar, actions: for the active pane
    void updateViewChrome();  // grid/list button and zoom limits
    void updatePaneCaptions();
    void fitListColumns(); // narrow panes drop Type, then Modified, so names have room
    // Owner / Group / Permissions: hidden unless chosen in the list
    // header's right-click menu (saved as view/listColumns, for both panes).
    void applyListColumns();
    void showListHeaderMenu(QHeaderView *header, const QPoint &pos);
    QStringList m_listColumns;
    // Expandable folders in the list view (Preferences, view/expandFolders).
    // Only the list shows them: switching to the grid closes every folder.
    bool m_expandFolders = false;
    void applyExpandFolders(); // to every pane's model
    void updateGitBadge();     // the extension text in the path bar (Git: the branch)
    void refreshGitAll();      // extensions look again: files may have changed elsewhere
    bool handleTreeKey(QKeyEvent *key);
public:
    // Preferences: turn expandable folders on or off in every window.
    static void setExpandFoldersEverywhere(bool on);
    // Preferences: git badges turned on or off.
private:
    // Rename, move and trash need a folder you can change: checked exactly,
    // once per folder of the selected items.
    bool canChangeSelection() const;
    QString titleFor(const QString &path) const;
    void setSplitView(bool on);
    void ensureSecondPane();
    QString otherPaneFolder() const; // where F5/F6 copy and move to, or empty
    void transferToOtherPane(bool move);

#if defined(ARGONAUT_DBUS)
    // Open/Save window: set by startChooser(), which hands that behaviour
    // to ChooserMode. Null in a file manager window.
    ChooserMode *m_chooser = nullptr;
#endif
    QVBoxLayout *m_contentLayout = nullptr;
    QWidget *m_menuRow = nullptr; // the menu bar's row; drags the window like the bar
};
