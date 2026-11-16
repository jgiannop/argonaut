#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

// The places a file manager's side list offers: Recent, Starred and the
// standard folders, the Trash, bookmarks (shared with Nautilus), drives, and
// the rows extensions add. What they are is decided here, once, for the
// window's sidebar and for the terminal interface's. Core code, no widgets.
namespace Places {

enum class Group { Standard, Bookmarks, Drives, Extensions };
enum class Icon { Recent, Star, Home, Desktop, Documents, Downloads, Music, Pictures, Videos, Trash, Folder, Drive, Removable, Network, Cloud };

struct Entry
{
    Group group = Group::Standard;
    Icon icon = Icon::Folder;
    QString name;
    QString path;           // a folder, a place ("recent:///"), or an extension's row's address
    QString tooltip;        // empty: the path
    bool eject = false;     // can be ejected or disconnected
    QString extensionPlace; // Extensions: the row's address (Extensions::Host::placePath)
};

// A row that isn't a folder to open as it is: an extension's (its
// extension is asked) or a drive that isn't mounted (it is mounted first).
bool isAddress(const QString &path);

// Everything, in the order it is shown.
QList<Entry> list();

// Says when the list may be another one: a bookmark was added or removed
// (here or in Nautilus), a drive was mounted or unmounted, a drive that
// isn't mounted came or went, an extension changed its rows. Whoever shows
// the places asks for the list again then (and needn't keep asking
// meanwhile). changed() may come several times for one change: the listener
// waits a moment before it reads.
class Watcher : public QObject
{
    Q_OBJECT

public:
    explicit Watcher(QObject *parent = nullptr);
    ~Watcher() override;

signals:
    void changed();

private:
    int m_mountsFd = -1;      // /proc/self/mounts (Linux): the kernel flags it when a mount changes
    QStringList m_lastMounts; // elsewhere: compared every few seconds
};

} // namespace Places
