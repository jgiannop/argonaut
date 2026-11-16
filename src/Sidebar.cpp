#include "Sidebar.h"

#include "Extensions.h"

#include "Bookmarks.h"
#include "DirectoryModel.h"
#include "Remote.h"
#include "Trash.h"

#include <QDir>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QPainter>
#include <QScrollBar>
#include <QSet>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QUrl>

#if defined(Q_OS_LINUX)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

constexpr int PathRole = Qt::UserRole;
constexpr int SeparatorRole = Qt::UserRole + 1;
constexpr int KindRole = Qt::UserRole + 2;  // Sidebar::Kind
constexpr int EjectRole = Qt::UserRole + 3; // bool: show an eject button
constexpr int ExtensionRole = Qt::UserRole + 4; // an extension's row: its place address

constexpr int RefreshDelayMs = 250; // one rebuild for a burst of changes
constexpr int MountPollMs = 3000;   // where mount changes can't be watched

// The eject button's area inside a row.
QRect ejectRect(const QRect &row)
{
    return QRect(row.right() - 30, row.center().y() - 12, 24, 24);
}

// Separator rows are drawn as a thin line; other rows are drawn normally so
// the style sheet's rounded hover/selection applies, with the ⏏ button on
// the rows that have one.
class SidebarDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QColor lineColor;
    QIcon ejectIcon;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
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

} // namespace

Sidebar::Sidebar(QWidget *parent) : QListWidget(parent), m_refreshTimer(new QTimer(this))
{
    setObjectName("sidebar");
    setItemDelegate(new SidebarDelegate(this));
    setIconSize(QSize(16, 16));
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::NoFocus);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setTextElideMode(Qt::ElideRight);

    m_refreshTimer->setSingleShot(true);
    m_refreshTimer->setInterval(RefreshDelayMs);
    connect(m_refreshTimer, &QTimer::timeout, this, &Sidebar::refresh);
    watchForChanges();
    // Extensions add and take away rows of their own.
    connect(Extensions::Host::instance(), &Extensions::Host::placesChanged, this, &Sidebar::refreshSoon);
}

Sidebar::~Sidebar()
{
#if defined(Q_OS_LINUX)
    if (m_mountsFd >= 0)
        ::close(m_mountsFd);
#endif
}

// Bookmark edits (also Nautilus's), drives plugged in or removed, servers
// connected or disconnected.
void Sidebar::watchForChanges()
{
    auto *bookmarksWatcher = new QFileSystemWatcher(this);
    const QString bookmarksFile = Bookmarks::filePath();
    QDir().mkpath(QFileInfo(bookmarksFile).absolutePath());
    bookmarksWatcher->addPath(QFileInfo(bookmarksFile).absolutePath()); // atomic saves replace the file
    connect(bookmarksWatcher, &QFileSystemWatcher::directoryChanged, this, &Sidebar::refreshSoon);
    // Servers connect and disconnect inside one FUSE mount, so /proc/mounts
    // doesn't change: watch GVfs's folder of mounts instead.
    if (const QString gvfs = Remote::gvfsRoot(); !gvfs.isEmpty()) {
        auto *gvfsWatcher = new QFileSystemWatcher({gvfs}, this);
        connect(gvfsWatcher, &QFileSystemWatcher::directoryChanged, this, &Sidebar::refreshSoon);
    }
#if defined(Q_OS_LINUX)
    // The kernel flags this file as "exceptional" whenever a mount changes.
    m_mountsFd = ::open("/proc/self/mounts", O_RDONLY | O_CLOEXEC);
    if (m_mountsFd >= 0) {
        auto *mounts = new QSocketNotifier(m_mountsFd, QSocketNotifier::Exception, this);
        connect(mounts, &QSocketNotifier::activated, this, &Sidebar::refreshSoon);
    }
#endif
    if (m_mountsFd < 0) {
        auto *poll = new QTimer(this);
        connect(poll, &QTimer::timeout, this, [this] {
            QStringList now;
            for (const QStorageInfo &vol : QStorageInfo::mountedVolumes())
                now << vol.rootPath();
            if (now != m_lastMounts)
                refreshSoon();
        });
        poll->start(MountPollMs);
    }
}

void Sidebar::setTheme(const Theme &theme)
{
    m_theme = theme;
    auto *delegate = static_cast<SidebarDelegate *>(itemDelegate());
    delegate->lineColor = theme.border;
    delegate->ejectIcon = glyphIcon(Glyph::Eject, theme.fg);
    refresh();
}

void Sidebar::refreshSoon()
{
    m_refreshTimer->start();
}

QListWidgetItem *Sidebar::addRow(const QString &name, const QString &path, Glyph glyph, Kind kind)
{
    auto *item = new QListWidgetItem(glyphIcon(glyph, m_theme.fg), name, this);
    item->setData(PathRole, path);
    item->setData(KindRole, int(kind));
    item->setToolTip(QDir::toNativeSeparators(path));
    return item;
}

void Sidebar::addSeparator()
{
    auto *separator = new QListWidgetItem(this);
    separator->setFlags(Qt::NoItemFlags);
    separator->setData(SeparatorRole, true);
}

void Sidebar::refresh()
{
    const int scroll = verticalScrollBar()->value();
    clear();

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
    addRow(tr("Recent"), DirectoryModel::recentPath(), Glyph::Recent, PlaceItem)->setToolTip(tr("Recently used files"));
    addRow(tr("Starred"), DirectoryModel::starredPath(), Glyph::Star, PlaceItem)->setToolTip(tr("Starred files and folders"));
    QStringList shown;
    for (const auto &p : places) {
        const QString path = QDir::cleanPath(QStandardPaths::writableLocation(p.loc));
        if (path.isEmpty() || shown.contains(path) || !QFileInfo(path).isDir())
            continue;
        shown << path;
        addRow(tr(p.name), path, p.glyph, PlaceItem);
    }

    if (Trash::isSupported()) {
        // Shows every trash folder merged (home and removable drives).
        const QString trash = Trash::homeFilesDir();
        QDir().mkpath(trash);
        QDir().mkpath(QFileInfo(trash).absolutePath() + QStringLiteral("/info"));
        addRow(tr("Trash"), trash, Glyph::Trash, PlaceItem);
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
        addRow(b.displayName(), b.path, Glyph::Folder, BookmarkItem);
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
        QListWidgetItem *item = addRow(system ? tr("Computer") : vol.displayName(), root,
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

#if defined(Q_OS_LINUX)
    // Network: connected servers (and phones), network bookmarks not
    // connected now, and Connect to Server….
    addSeparator();
    QSet<QString> connected;
    for (const Remote::Mount &m : Remote::mounts()) {
        QListWidgetItem *item = addRow(m.name, m.localPath, Glyph::Network, NetworkItem);
        item->setData(EjectRole, true);
        item->setToolTip(m.address + QLatin1Char('\n') + tr("Disconnect: click ⏏"));
        connected.insert(QUrl(m.address).host());
    }
    for (const Bookmarks::Bookmark &b : Bookmarks::loadRemote()) {
        const QUrl url(b.path);
        if (connected.contains(url.host()))
            continue;
        QListWidgetItem *item = addRow(b.label.isEmpty() ? url.host() : b.label, b.path, Glyph::Network, NetworkItem);
        item->setToolTip(b.path);
    }
    addRow(tr("Connect to Server…"), connectPlace(), Glyph::Network, PlaceItem)->setToolTip(tr("sftp, smb, ftp, WebDAV…"));
#endif
    // Rows from extensions (a cloud drive, a kind of server we don't know):
    // a group of their own at the end.
    const QList<Extensions::Place> added = Extensions::Host::instance()->places();
    if (!added.isEmpty())
        addSeparator();
    for (const Extensions::Place &place : added) {
        const Glyph glyph = place.icon == QLatin1String("network") ? Glyph::Network
                            : place.icon == QLatin1String("cloud") ? Glyph::Cloud
                            : place.icon == QLatin1String("drive") ? Glyph::Drive
                            : place.icon == QLatin1String("removable") ? Glyph::Removable : Glyph::Folder;
        const QString address = Extensions::Host::placePath(place.extension, place.id);
        QListWidgetItem *item = addRow(place.label, place.folder.isEmpty() ? address : place.folder, glyph, ExtensionItem);
        item->setData(ExtensionRole, address);
        item->setData(EjectRole, place.eject);
        const QString from = Extensions::Host::instance()->nameOf(place.extension);
        QString tip = place.tooltip.isEmpty() ? (place.folder.isEmpty() ? place.label : QDir::toNativeSeparators(place.folder)) : place.tooltip;
        if (!from.isEmpty())
            tip += QLatin1Char('\n') + tr("From the extension “%1”").arg(from);
        if (place.eject)
            tip += QLatin1Char('\n') + tr("Disconnect: click ⏏");
        item->setToolTip(tip);
    }
    verticalScrollBar()->setValue(scroll);
    emit refreshed();
}

QString Sidebar::extensionPlace(const QListWidgetItem *item)
{
    return item ? item->data(ExtensionRole).toString() : QString();
}

void Sidebar::selectPath(const QString &path)
{
    for (int i = 0; i < count(); ++i) {
        QListWidgetItem *row = item(i);
        const QString rowPath = pathOf(row);
        if (!rowPath.isEmpty() && (rowPath == path || QDir::cleanPath(rowPath) == path)) {
            setCurrentItem(row);
            return;
        }
    }
    clearSelection();
    setCurrentRow(-1);
}

QString Sidebar::pathOf(const QListWidgetItem *item)
{
    return item ? item->data(PathRole).toString() : QString();
}

Sidebar::Kind Sidebar::kindOf(const QListWidgetItem *item)
{
    return item ? Kind(item->data(KindRole).toInt()) : PlaceItem;
}

bool Sidebar::isSeparator(const QListWidgetItem *item)
{
    return item && item->data(SeparatorRole).toBool();
}

bool Sidebar::canEject(const QListWidgetItem *item)
{
    return item && item->data(EjectRole).toBool();
}

bool Sidebar::ejectButtonHit(const QListWidgetItem *item, const QPoint &viewportPos) const
{
    return canEject(item) && ejectRect(visualItemRect(item)).contains(viewportPos);
}

QString Sidebar::connectPlace()
{
    return QStringLiteral("connect:///");
}
