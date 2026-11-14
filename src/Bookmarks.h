#pragma once

#include <QList>
#include <QString>

// Sidebar bookmarks, stored in the GTK bookmarks file that Nautilus uses
// (~/.config/gtk-3.0/bookmarks on Linux; a file of the same format in the
// app's config folder elsewhere). One line per bookmark:
//     file:///home/me/Projects%20Old Optional label
// Lines Kosmos doesn't use (network locations, etc.) are kept untouched.
namespace Bookmarks {

struct Bookmark
{
    QString path;  // local folder
    QString label; // empty: show the folder name
    QString displayName() const;
};

QString filePath();
QList<Bookmark> load(); // local ones, in file order
// Network bookmarks (sftp://, smb://… lines, as Nautilus saves them):
// `path` is the address. Shown under Network; opening one connects.
QList<Bookmark> loadRemote();
bool contains(const QString &path);
bool add(const QString &path);
bool remove(const QString &path);
bool rename(const QString &path, const QString &label);

} // namespace Bookmarks
