#pragma once

#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

class QFileSystemWatcher;
class QProcess;
class QTimer;

// Drives that are there but not mounted (a second disk, a stick the desktop
// didn't open), so the side list can show them as Nautilus does, and mount
// one when it is chosen. Core code, Linux.
//
// The list is the desktop's own (GVfs's volume monitor, asked with
// `gio mount -li`): what Nautilus shows, with the same names, and without
// the partitions the system says to hide. Mounting is `gio mount -d
// <device>`, so the desktop mounts it where it always does
// (/media/<user>/<label>) and asks for a password itself where one is
// needed. Nothing here needs more rights than the user has.
//
// Nothing blocks: the list is asked for in the background and kept;
// changed() says when it is another one.
class Volumes : public QObject
{
    Q_OBJECT

public:
    struct Volume
    {
        QString name;   // as the desktop names it ("HITACHI", "2.0 TB Volume")
        QString device; // "/dev/sdb1"
        QString uuid;
        bool removable = false; // on a drive that can be taken out
        bool operator==(const Volume &o) const { return name == o.name && device == o.device && uuid == o.uuid && removable == o.removable; }
    };

    static Volumes *instance();
    // False where there is nothing to ask (not Linux, no `gio`), and in
    // tests: ARGONAUT_VOLUMES=0 switches it off and =1 on; left unset, it is
    // off wherever ARGONAUT_TRASH_HOME_ONLY keeps the tests away from real
    // drives.
    static bool isActive();

    // The volumes that can be mounted and aren't, as last heard. Asks again
    // in the background when that was a while ago, or when `mounted` (the
    // devices mounted now) isn't what it was the last time.
    QList<Volume> unmounted(const QStringList &mounted = {});
    void refresh();

    // A side-list row for a volume has this address instead of a folder.
    static QString pathFor(const QString &device); // "volume:///dev/sdb1"
    static bool isVolumePath(const QString &path);
    static QString deviceOf(const QString &path);  // "" unless it is a volume's address with a plain device name
    QString nameOf(const QString &device) const;

    // Mounts it; mounted() or failed() follows. Asked twice for one device,
    // it mounts once.
    void mount(const QString &device);

    // What `gio mount -li` printed, as the volumes without a mount.
    static QList<Volume> parse(const QByteArray &output);

signals:
    void changed();
    void mounted(const QString &device, const QString &folder);
    void failed(const QString &device, const QString &message);

private:
    Volumes();

    QList<Volume> m_volumes;
    QStringList m_mountedBefore;
    QElapsedTimer m_asked;      // when the list was last asked for
    QProcess *m_listing = nullptr;
    bool m_again = false;       // asked for while a listing was running
    QSet<QString> m_mounting;
    QFileSystemWatcher *m_watcher = nullptr; // /dev/disk/by-uuid: a partition came or went
    QTimer *m_soon = nullptr;
};
