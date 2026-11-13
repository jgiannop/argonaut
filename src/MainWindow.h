#pragma once

#include "Theme.h"

#include <QAbstractItemModel>
#include <QElapsedTimer>
#include <QHash>
#include <QMainWindow>
#include <QStack>
#include <QString>

class DirectoryModel;
class FileFilterProxy;
class FileItemDelegate;
class FileJob;
class FileOperations;
class OperationsPanel;
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
class QTimer;
class QToolButton;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

    // Show the folder at `path`. Returns false if it doesn't exist.
    bool navigateTo(const QString &path, bool addToHistory = true);

protected:
    void changeEvent(QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void setupActions();
    void setupUi();
    QWidget *buildSidebarPane();
    QWidget *buildContentPane();
    QToolButton *makeButton(const QString &toolTip, const char *objectName = "flat");
    QMenu *makeMenu();
    QWidget *buildMenuBar();

    void applyTheme(bool force = false);
    void populateSidebar();
    void syncSidebarSelection();

    void goBack();
    void goForward();
    void goUp();
    void openIndex(const QModelIndex &index);
    void setGridMode(bool grid);
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
    void openSelection();
    void copySelection(bool cut);
    void paste();
    void trashSelection();
    void updateEditActions();
    void showContextMenu(const QPoint &pos);
    void createNewItem(bool folder);

    // Inline rename (F2, and right after New Folder / New File).
    void renameSelection();
    void startRename(const QModelIndex &viewIndex);
    bool commitRename();
    void cancelRename();
    void updateRenameGeometry();
    void onJobFinished(FileJob *job);

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
    bool m_grid = true;
    bool m_showHidden = false;
    bool m_resizeCursor = false;

    FileOperations *m_ops = nullptr;
    OperationsPanel *m_opsPanel = nullptr;
    Toast *m_toast = nullptr;
    DirectoryModel *m_model = nullptr;
    FileFilterProxy *m_proxy = nullptr; // search filter between model and view
    Thumbnailer *m_thumbnailer = nullptr;
    FileItemDelegate *m_gridDelegate = nullptr; // one per view: each caches its
    FileItemDelegate *m_listDelegate = nullptr; // own sizes and layout
    QAbstractItemView *m_view = nullptr; // the visible one of the two below
    QListView *m_iconView = nullptr;      // grid
    QTreeView *m_detailView = nullptr;    // list with columns
    QStackedWidget *m_viewStack = nullptr;
    QListWidget *m_sidebar = nullptr;
    QWidget *m_root = nullptr;
    QSplitter *m_splitter = nullptr;
    QStackedWidget *m_locationStack = nullptr;
    PathBar *m_pathBar = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    QLabel *m_titleLabel = nullptr;
    QLabel *m_floatingBar = nullptr;
    QTimer *m_messageTimer = nullptr;
    QString m_message;

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
    QAction *m_aboutAct = nullptr;
    QAction *m_openAct = nullptr;
    QAction *m_cutAct = nullptr;
    QAction *m_copyAct = nullptr;
    QAction *m_pasteAct = nullptr;
    QAction *m_trashAct = nullptr;
    QAction *m_undoAct = nullptr;
    QAction *m_homeAct = nullptr;
    QAction *m_locationAct = nullptr;
    QAction *m_gridAct = nullptr;
    QAction *m_listAct = nullptr;
    QAction *m_newFolderAct = nullptr;
    QAction *m_newFileAct = nullptr;
    QAction *m_renameAct = nullptr;
    QAction *m_selectAllAct = nullptr;
    QAction *m_closeAct = nullptr;
    QAction *m_quitAct = nullptr;
    QAction *m_sidebarAct = nullptr;
    QList<QAction *> m_sortColumnActs; // by DirectoryModel::Column
    QAction *m_sortAscAct = nullptr;
    QAction *m_sortDescAct = nullptr;

    QMenuBar *m_menuBar = nullptr;
    QPoint m_menuPressPos; // dragging the window by empty menu bar space
    bool m_menuPressed = false;
    QWidget *m_sidebarPane = nullptr;

    QTimer *m_dragScrollTimer = nullptr; // auto-scroll near the view's edges
    int m_dragScrollStep = 0;
    mutable QHash<QString, QByteArray> m_deviceCache; // folder -> storage device

    QLineEdit *m_renameEdit = nullptr;
    QString m_renamePath;
    QString m_pendingRename; // start renaming this once it appears
    bool m_committingRename = false;

    QStack<QString> m_backHistory;
    QStack<QString> m_forwardHistory;
    QString m_current;

    QStringList m_pendingSelection;
    QElapsedTimer m_pendingSince;
    bool m_applyingSelection = false;
};
