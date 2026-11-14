#pragma once

#include <QFile>
#include <QHash>
#include <QString>

// Who may do what with a file, worked out cheaply (core code, no widgets).
//
// Listing keeps the owner, group and permission bits from the one stat it
// already does, so these answers cost no extra system calls per file:
//  - likelyWritable() is the in-memory guess behind the 🔒 emblem;
//  - userName()/groupName() look each id up once and cache it;
//  - canChangeItemsIn() is the exact check, run only for the folders of
//    the selected items (rename, move, trash).
namespace Access {

// Owner/group/other bits of QFileDevice::Permissions (never the "User"
// bits: Qt computes those with one access() call per file).
constexpr QFile::Permissions PermissionMask =
    QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadGroup | QFile::WriteGroup
    | QFile::ExeGroup | QFile::ReadOther | QFile::WriteOther | QFile::ExeOther;

// Unknown owner or group (Windows, or a place with no ids).
constexpr uint NoId = uint(-2);

// True where files have Unix owners and groups (not Windows).
bool hasOwners();

// Can the current user probably write it, from its bits alone? Root can;
// ACLs and read-only drives aren't seen, so it is only for showing.
bool likelyWritable(QFile::Permissions permissions, uint owner, uint group);

// "jgiannop", "root"; the number when the id has no name. Cached.
QString userName(uint id);
QString groupName(uint id);

// "drwxr-xr-x" / "-rw-r--r--" / "lrwxrwxrwx", like ls -l.
QString permissionString(QFile::Permissions permissions, bool isDir, bool isSymLink);

// Exact: may the current user rename, move or trash an item owned by
// `itemOwner` inside `dir`? The folder must be writable, and in a sticky
// folder like /tmp the item (or the folder) must be yours. `cache` keeps
// one answer per folder, so a big selection costs one check per folder.
struct FolderAccess
{
    bool writable = false;
    bool sticky = false;
    uint owner = NoId;
};
bool canChangeItemsIn(const QString &dir, uint itemOwner, QHash<QString, FolderAccess> *cache);

} // namespace Access
