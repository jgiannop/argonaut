#include "Places.h"

#include "Bookmarks.h"
#include "DirectoryModel.h"
#include "Extensions.h"
#include "Trash.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStorageInfo>

namespace Places {

namespace {

// The window's sidebar had these texts first; they are still its to translate.
QString tr(const char *text)
{
    return QCoreApplication::translate("Sidebar", text);
}

} // namespace

QList<Entry> list(QStringList *mounts)
{
    QList<Entry> out;
    const struct {
        QStandardPaths::StandardLocation loc;
        const char *name;
        Icon icon;
    } places[] = {
        {QStandardPaths::HomeLocation, QT_TRANSLATE_NOOP("Sidebar", "Home"), Icon::Home},
        {QStandardPaths::DesktopLocation, QT_TRANSLATE_NOOP("Sidebar", "Desktop"), Icon::Desktop},
        {QStandardPaths::DocumentsLocation, QT_TRANSLATE_NOOP("Sidebar", "Documents"), Icon::Documents},
        {QStandardPaths::DownloadLocation, QT_TRANSLATE_NOOP("Sidebar", "Downloads"), Icon::Downloads},
        {QStandardPaths::MusicLocation, QT_TRANSLATE_NOOP("Sidebar", "Music"), Icon::Music},
        {QStandardPaths::PicturesLocation, QT_TRANSLATE_NOOP("Sidebar", "Pictures"), Icon::Pictures},
        {QStandardPaths::MoviesLocation, QT_TRANSLATE_NOOP("Sidebar", "Videos"), Icon::Videos},
    };
    out.append({Group::Standard, Icon::Recent, tr("Recent"), DirectoryModel::recentPath(), tr("Recently used files"), false, {}});
    out.append({Group::Standard, Icon::Star, tr("Starred"), DirectoryModel::starredPath(), tr("Starred files and folders"), false, {}});
    QStringList shown;
    for (const auto &p : places) {
        const QString path = QDir::cleanPath(QStandardPaths::writableLocation(p.loc));
        if (path.isEmpty() || shown.contains(path) || !QFileInfo(path).isDir())
            continue;
        shown << path;
        out.append({Group::Standard, p.icon, tr(p.name), path, {}, false, {}});
    }
    if (Trash::isSupported()) {
        // Shows every trash folder merged (home and removable drives).
        const QString trash = Trash::homeFilesDir();
        QDir().mkpath(trash);
        QDir().mkpath(QFileInfo(trash).absolutePath() + QStringLiteral("/info"));
        out.append({Group::Standard, Icon::Trash, tr("Trash"), trash, {}, false, {}});
    }

    // Bookmarks (shared with Nautilus). Ones that duplicate a place above,
    // or whose folder is gone, aren't shown (but stay in the file).
    const QList<Bookmarks::Bookmark> bookmarks = Bookmarks::load();
    for (const Bookmarks::Bookmark &b : bookmarks) {
        if (shown.contains(b.path) || !QFileInfo(b.path).isDir())
            continue;
        shown << b.path;
        out.append({Group::Bookmarks, Icon::Folder, b.displayName(), b.path, {}, false, {}});
    }

    const QList<QStorageInfo> volumes = QStorageInfo::mountedVolumes();
    for (const QStorageInfo &vol : volumes) {
        if (!vol.isValid() || !vol.isReady())
            continue;
        const QString root = vol.rootPath();
        if (mounts)
            *mounts << root;
#if defined(Q_OS_LINUX)
        // Skip system/snap mounts; keep "/" and user-visible drives.
        if (root != QLatin1String("/") && !root.startsWith(QLatin1String("/media/")) && !root.startsWith(QLatin1String("/mnt/"))
            && !root.startsWith(QLatin1String("/run/media/")))
            continue;
#elif defined(Q_OS_MACOS)
        if (root.startsWith(QLatin1String("/System/Volumes")))
            continue;
#endif
        const bool system = root == QDir::rootPath();
        Entry drive{Group::Drives, system ? Icon::Drive : Icon::Removable, system ? tr("Computer") : vol.displayName(), root, {}, false, {}};
#if defined(Q_OS_LINUX)
        // Drives mounted by the desktop (udisks) can be ejected.
        if (root.startsWith(QLatin1String("/media/")) || root.startsWith(QLatin1String("/run/media/"))) {
            drive.eject = true;
            drive.tooltip = QDir::toNativeSeparators(root) + QLatin1Char('\n') + tr("Eject: click ⏏");
        }
#endif
        out.append(drive);
    }

    // Rows from extensions: connected servers, network bookmarks and
    // "Connect to Server…" are the Connect to Server extension's; a cloud
    // drive would be another's.
    const QList<Extensions::Place> added = Extensions::Host::instance()->places();
    for (const Extensions::Place &place : added) {
        Entry entry;
        entry.group = Group::Extensions;
        entry.icon = place.icon == QLatin1String("network") ? Icon::Network
                     : place.icon == QLatin1String("cloud") ? Icon::Cloud
                     : place.icon == QLatin1String("drive") ? Icon::Drive
                     : place.icon == QLatin1String("removable") ? Icon::Removable : Icon::Folder;
        entry.name = place.label;
        entry.extensionPlace = Extensions::Host::placePath(place.extension, place.id);
        entry.path = place.folder.isEmpty() ? entry.extensionPlace : place.folder;
        entry.eject = place.eject;
        const QString from = Extensions::Host::instance()->nameOf(place.extension);
        entry.tooltip = place.tooltip.isEmpty() ? (place.folder.isEmpty() ? place.label : QDir::toNativeSeparators(place.folder)) : place.tooltip;
        if (!from.isEmpty())
            entry.tooltip += QLatin1Char('\n') + tr("From the extension “%1”").arg(from);
        if (place.eject)
            entry.tooltip += QLatin1Char('\n') + tr("Disconnect: click ⏏");
        out.append(entry);
    }
    return out;
}

} // namespace Places
