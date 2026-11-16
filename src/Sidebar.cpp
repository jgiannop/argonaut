#include "Sidebar.h"

#include "Extensions.h"
#include "Places.h"

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

    // What the places are is Places' business (shared with the terminal
    // interface); here they become rows, with a line between the groups.
    m_lastMounts.clear();
    const QList<Places::Entry> entries = Places::list(&m_lastMounts);
    bool first = true;
    Places::Group group = Places::Group::Standard;
    for (const Places::Entry &entry : entries) {
        // Always a line before the drives, as there always was (the
        // bookmarks may be none).
        if (!first && entry.group != group)
            addSeparator();
        first = false;
        group = entry.group;
        Glyph glyph = Glyph::Folder;
        switch (entry.icon) {
        case Places::Icon::Recent: glyph = Glyph::Recent; break;
        case Places::Icon::Star: glyph = Glyph::Star; break;
        case Places::Icon::Home: glyph = Glyph::Home; break;
        case Places::Icon::Desktop: glyph = Glyph::Desktop; break;
        case Places::Icon::Documents: glyph = Glyph::Documents; break;
        case Places::Icon::Downloads: glyph = Glyph::Downloads; break;
        case Places::Icon::Music: glyph = Glyph::Music; break;
        case Places::Icon::Pictures: glyph = Glyph::Pictures; break;
        case Places::Icon::Videos: glyph = Glyph::Videos; break;
        case Places::Icon::Trash: glyph = Glyph::Trash; break;
        case Places::Icon::Folder: glyph = Glyph::Folder; break;
        case Places::Icon::Drive: glyph = Glyph::Drive; break;
        case Places::Icon::Removable: glyph = Glyph::Removable; break;
        case Places::Icon::Network: glyph = Glyph::Network; break;
        case Places::Icon::Cloud: glyph = Glyph::Cloud; break;
        }
        const Kind kind = entry.group == Places::Group::Bookmarks ? BookmarkItem
                          : entry.group == Places::Group::Drives ? DriveItem
                          : entry.group == Places::Group::Extensions ? ExtensionItem : PlaceItem;
        QListWidgetItem *item = addRow(entry.name, entry.path, glyph, kind);
        if (!entry.tooltip.isEmpty())
            item->setToolTip(entry.tooltip);
        if (entry.eject)
            item->setData(EjectRole, true);
        if (!entry.extensionPlace.isEmpty())
            item->setData(ExtensionRole, entry.extensionPlace);
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

