#pragma once

#include "Theme.h"

#include <QElapsedTimer>
#include <QMainWindow>
#include <QStack>
#include <QString>

class FileItemDelegate;
class FileJob;
class FileOperations;
class OperationsPanel;
class Toast;
class PathBar;
class Thumbnailer;
class QAction;
class QFileSystemModel;
class QLabel;
class QLineEdit;
class QListView;
class QListWidget;
class QMenu;
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

    // File operations
    QStringList selectedPaths() const;
    void openSelection();
    void copySelection(bool cut);
    void paste();
    void trashSelection();
    void updateEditActions();
    void showContextMenu(const QPoint &pos);
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
    int m_themeDark = -1;
    bool m_csd = false;
    bool m_grid = true;
    bool m_showHidden = false;
    bool m_resizeCursor = false;

    FileOperations *m_ops = nullptr;
    OperationsPanel *m_opsPanel = nullptr;
    Toast *m_toast = nullptr;
    QFileSystemModel *m_model = nullptr;
    Thumbnailer *m_thumbnailer = nullptr;
    FileItemDelegate *m_delegate = nullptr;
    QListView *m_view = nullptr;
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

    QStack<QString> m_backHistory;
    QStack<QString> m_forwardHistory;
    QString m_current;

    QStringList m_pendingSelection;
    QElapsedTimer m_pendingSince;
    bool m_applyingSelection = false;
};
