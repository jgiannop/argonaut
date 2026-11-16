#pragma once

#include <QList>
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

// Everything, in the order it is shown. `mounts`, if given, gets the root
// of every mounted volume (to notice later that they changed).
QList<Entry> list(QStringList *mounts = nullptr);

} // namespace Places
