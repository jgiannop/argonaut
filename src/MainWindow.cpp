#include "MainWindow.h"

#include <QAction>
#include <QDesktopServices>
#include <QDir>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QKeySequence>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStorageInfo>
#include <QStyle>
#include <QToolBar>
#include <QUrl>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    resize(1100, 700);
    setupToolbar();
    setupViews();
    populateSidebar();
}

void MainWindow::setupToolbar()
{
    QToolBar *tb = addToolBar(tr("Navigation"));
    tb->setMovable(false);

    m_backAct = tb->addAction(
        QIcon::fromTheme("go-previous", style()->standardIcon(QStyle::SP_ArrowBack)),
        tr("Back"), this, &MainWindow::goBack);
    m_backAct->setShortcut(QKeySequence::Back);

    m_forwardAct = tb->addAction(
        QIcon::fromTheme("go-next", style()->standardIcon(QStyle::SP_ArrowForward)),
        tr("Forward"), this, &MainWindow::goForward);
    m_forwardAct->setShortcut(QKeySequence::Forward);

    m_upAct = tb->addAction(
        QIcon::fromTheme("go-up", style()->standardIcon(QStyle::SP_ArrowUp)),
        tr("Up"), this, &MainWindow::goUp);
    m_upAct->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Up));

    QAction *homeAct = tb->addAction(
        QIcon::fromTheme("go-home", style()->standardIcon(QStyle::SP_DirHomeIcon)),
        tr("Home"), this, [this] { navigateTo(QDir::homePath()); });
    homeAct->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Home));

    m_pathEdit = new QLineEdit(this);
    m_pathEdit->setPlaceholderText(tr("Type a path and press Enter"));
    tb->addWidget(m_pathEdit);
    connect(m_pathEdit, &QLineEdit::returnPressed, this,
            [this] { navigateTo(m_pathEdit->text().trimmed()); });

    QAction *focusPath = new QAction(this);
    focusPath->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_L));
    connect(focusPath, &QAction::triggered, this, [this] {
        m_pathEdit->setFocus();
        m_pathEdit->selectAll();
    });
    addAction(focusPath);

    QAction *gridAct = tb->addAction(
        QIcon::fromTheme("view-grid", style()->standardIcon(QStyle::SP_FileDialogContentsView)),
        tr("Grid view"));
    gridAct->setCheckable(true);
    gridAct->setChecked(true);
    connect(gridAct, &QAction::toggled, this, &MainWindow::setGridMode);

    QAction *hiddenAct = tb->addAction(
        QIcon::fromTheme("view-hidden", style()->standardIcon(QStyle::SP_FileDialogDetailedView)),
        tr("Show hidden files"));
    hiddenAct->setCheckable(true);
    hiddenAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_H));
    connect(hiddenAct, &QAction::toggled, this, &MainWindow::setShowHidden);
}

void MainWindow::setupViews()
{
    m_model = new QFileSystemModel(this);
    m_model->setRootPath(QString()); // watch the whole filesystem lazily
    m_model->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::AllDirs);
    connect(m_model, &QFileSystemModel::directoryLoaded, this, [this](const QString &path) {
        if (QDir::cleanPath(path) == m_current)
            updateStatus();
    });

    m_sidebar = new QListWidget(this);
    m_sidebar->setIconSize(QSize(20, 20));
    m_sidebar->setFrameShape(QFrame::NoFrame);
    connect(m_sidebar, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        const QString path = item->data(Qt::UserRole).toString();
        if (!path.isEmpty())
            navigateTo(path);
    });

    m_view = new QListView(this);
    m_view->setModel(m_model);
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_view->setUniformItemSizes(true);
    m_view->setResizeMode(QListView::Adjust);
    m_view->setWordWrap(true);
    m_view->setFrameShape(QFrame::NoFrame);
    connect(m_view, &QListView::activated, this, &MainWindow::openIndex);
    setGridMode(true);

    auto *splitter = new QSplitter(this);
    splitter->addWidget(m_sidebar);
    splitter->addWidget(m_view);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({200, 900});
    setCentralWidget(splitter);
}

void MainWindow::populateSidebar()
{
    QFileIconProvider icons;

    auto addPlace = [&](const QString &name, const QString &path, const QIcon &icon) {
        auto *item = new QListWidgetItem(icon, name, m_sidebar);
        item->setData(Qt::UserRole, path);
        item->setToolTip(QDir::toNativeSeparators(path));
    };

    const struct { QStandardPaths::StandardLocation loc; const char *name; } places[] = {
        {QStandardPaths::HomeLocation, QT_TR_NOOP("Home")},
        {QStandardPaths::DesktopLocation, QT_TR_NOOP("Desktop")},
        {QStandardPaths::DocumentsLocation, QT_TR_NOOP("Documents")},
        {QStandardPaths::DownloadLocation, QT_TR_NOOP("Downloads")},
        {QStandardPaths::MusicLocation, QT_TR_NOOP("Music")},
        {QStandardPaths::PicturesLocation, QT_TR_NOOP("Pictures")},
        {QStandardPaths::MoviesLocation, QT_TR_NOOP("Videos")},
    };
    QStringList added;
    for (const auto &p : places) {
        const QString path = QStandardPaths::writableLocation(p.loc);
        if (path.isEmpty() || added.contains(path) || !QFileInfo(path).isDir())
            continue;
        added << path;
        addPlace(tr(p.name), path, icons.icon(QFileInfo(path)));
    }

    // Separator
    auto *sep = new QListWidgetItem(m_sidebar);
    sep->setFlags(Qt::NoItemFlags);
    sep->setSizeHint(QSize(0, 10));

    // Drives / mounted volumes
    const QIcon driveIcon = icons.icon(QFileIconProvider::Drive);
    for (const QStorageInfo &vol : QStorageInfo::mountedVolumes()) {
        if (!vol.isValid() || !vol.isReady())
            continue;
        const QString root = vol.rootPath();
#if defined(Q_OS_LINUX)
        // Skip system/snap mounts; keep "/" and user-visible drives.
        if (root != "/" && !root.startsWith("/media/") && !root.startsWith("/mnt/")
            && !root.startsWith("/run/media/"))
            continue;
#elif defined(Q_OS_MACOS)
        if (root.startsWith("/System/Volumes"))
            continue;
#endif
        QString name = vol.displayName();
        if (root == "/")
            name = tr("Computer");
        addPlace(name, root, driveIcon);
    }
}

bool MainWindow::navigateTo(const QString &path, bool addToHistory)
{
    const QFileInfo info(path);
    if (!info.exists() || !info.isDir()) {
        statusBar()->showMessage(tr("Folder not found: %1").arg(path), 4000);
        m_pathEdit->setText(QDir::toNativeSeparators(m_current));
        return false;
    }

    const QString clean = QDir::cleanPath(info.absoluteFilePath());
    if (clean == m_current)
        return true;

    if (addToHistory && !m_current.isEmpty()) {
        m_backHistory.push(m_current);
        m_forwardHistory.clear();
    }
    m_current = clean;

    m_view->setRootIndex(m_model->index(clean));
    m_view->scrollToTop();
    m_pathEdit->setText(QDir::toNativeSeparators(clean));

    const QString title = info.fileName().isEmpty() ? QDir::toNativeSeparators(clean)
                                                    : info.fileName();
    setWindowTitle(tr("%1 — Kaftilous").arg(title));

    updateActions();
    updateStatus();
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

void MainWindow::openIndex(const QModelIndex &index)
{
    const QString path = m_model->filePath(index);
    if (m_model->isDir(index))
        navigateTo(path);
    else
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void MainWindow::setGridMode(bool grid)
{
    if (grid) {
        m_view->setViewMode(QListView::IconMode);
        m_view->setMovement(QListView::Static);
        m_view->setIconSize(QSize(48, 48));
        m_view->setGridSize(QSize(110, 96));
        m_view->setSpacing(4);
    } else {
        m_view->setViewMode(QListView::ListMode);
        m_view->setIconSize(QSize(22, 22));
        m_view->setGridSize(QSize());
        m_view->setSpacing(1);
    }
}

void MainWindow::setShowHidden(bool show)
{
    QDir::Filters f = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::AllDirs;
    if (show)
        f |= QDir::Hidden;
    m_model->setFilter(f);
    updateStatus();
}

void MainWindow::updateActions()
{
    m_backAct->setEnabled(!m_backHistory.isEmpty());
    m_forwardAct->setEnabled(!m_forwardHistory.isEmpty());
    m_upAct->setEnabled(!m_current.isEmpty() && !QDir(m_current).isRoot());
}

void MainWindow::updateStatus()
{
    const int count = m_model->rowCount(m_view->rootIndex());
    statusBar()->showMessage(tr("%n item(s)", nullptr, count));
}
