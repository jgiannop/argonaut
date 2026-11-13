#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>

// The freedesktop.org Trash (what Nautilus and QFile::moveToTrash use):
//   ~/.local/share/Trash/{files,info}            for the home drive
//   <drive>/.Trash-<uid>/{files,info}            for other drives
//   <drive>/.Trash/<uid>/{files,info}            (admin-created variant)
// Each trashed item "files/NAME" has "info/NAME.trashinfo" with its original
// path and deletion date. Linux/BSD only; elsewhere everything is empty.
namespace Trash {

struct Info
{
    QString originalPath; // absolute
    QDateTime deletionDate;
    bool valid = false;
};

bool isSupported();
// The home trash's "files" folder (what the sidebar's Trash opens).
QString homeFilesDir();
// Every existing "files" folder: the home one first, then mounted drives'.
QStringList filesDirs();
// Is `dir` one of the trash "files" folders?
bool isFilesDir(const QString &dir);
// The trash "files" folder that `path` is inside of, or empty.
QString filesDirOf(const QString &path);
// For a top-level item in a trash "files" folder: its .trashinfo file.
QString infoPathFor(const QString &pathInTrash);
Info info(const QString &pathInTrash);

} // namespace Trash
