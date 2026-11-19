#pragma once

#include <QFileDevice>
#include <QString>

// Editing a text file in the preview (the window's editor, and $EDITOR from
// the terminal interface): which files may be edited, reading one so that
// it is written back the way it was (UTF-8 with or without a byte order
// mark, Windows or Unix line ends), and saving it without ever losing what
// was there: the version from before the editing goes to the Trash first.
namespace TextFile {

constexpr qint64 MaxBytes = 5'000'000; // bigger isn't something to edit in a preview

// Why `path` can't be edited here ("isn't text"…); empty: it can. Only
// plain files you may write, in a folder you may write (saving replaces
// the file), UTF-8 text without NUL bytes, never a program in disguise or
// anything in the Trash.
QString problem(const QString &path);

struct Loaded
{
    QString path;          // the real file (a link's target): that is what is edited
    QString text;          // with "\n" line ends
    QString problem;       // not empty: nothing was loaded (why)
    bool crlf = false;     // the file had "\r\n" line ends: written back with them
    bool bom = false;      // …and a UTF-8 byte order mark
    qint64 size = -1;      // as it was on disk when read (or last saved)
    qint64 mtimeMs = -1;
    QFileDevice::Permissions permissions;
};
Loaded load(const QString &path);

// Another program changed (or removed) the file since it was read or saved.
bool changedOnDisk(const Loaded &file);

// Puts a copy of `path` in the Trash, recorded under its own place, so it
// can be restored from there; the file stays as it is. Returns where the
// copy is in the Trash; empty with `error` set when there is no Trash here.
QString keepOldVersion(const QString &path, QString *error);
// Takes back a copy keepOldVersion() just made (nothing was changed after all).
void forgetOldVersion(const QString &pathInTrash);

struct Saved
{
    bool ok = false;
    QString error;
    QString oldVersion; // where the version from before is in the Trash (when it was put there now)
};
// Writes `text` over the file, atomically (a crash leaves the old file or
// the new one, never half of one), with the file's line ends, mark and
// permissions. With `keepOld`, the file as it is goes to the Trash first;
// that failing stops the save (the caller may ask and try without).
Saved save(Loaded *file, const QString &text, bool keepOld);

} // namespace TextFile
