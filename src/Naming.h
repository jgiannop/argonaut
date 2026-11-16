#pragma once

#include "FileJob.h"

#include <QList>
#include <QCollator>
#include <QString>
#include <QStringList>

// Name helpers shared by Select Items Matching, Batch Rename and New
// Document (Templates). Core code: no widgets, so tests can use it.
namespace Naming {

// Natural, case-insensitive order: "file2" before "file10". In the "C"/POSIX
// locale Qt can't sort numerically, so English rules are used there.
QCollator naturalCollator();

// Hidden by its name: a dot-file or an editor backup ("notes.txt~").
inline bool isHiddenName(const QString &name)
{
    return name.startsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char('~'));
}
// True if any part of a relative path ("a/.git/config") is hidden.
bool hasHiddenPart(const QString &relativePath);

// Splits "Photo.tar.gz" into "Photo" + ".tar.gz" (folders and dotfiles
// like ".bashrc" have no extension), the same way rename selects the base.
QString baseName(const QString &name, bool isDir, QString *extension = nullptr);

// Shell-style pattern for Select Items Matching: "*.jpg", "IMG_20??*",
// "[ab]*". Case-insensitive. A pattern without wildcards matches names
// that contain it, so typing "holiday" works too.
bool matchesPattern(const QString &name, const QString &pattern);

// How well a command's name fits what was typed in a command palette (0: not
// at all). Both already folded (DirectoryModel::foldForSearch). The start of
// the name is best, then the start of a word, then anywhere, then all the
// words, then the letters in order ("nf" → "new folder").
int commandScore(const QString &text, const QString &query);

// ------------------------------------------------------------ batch rename

struct RenameOptions
{
    enum class Mode { Template, Replace };
    Mode mode = Mode::Template;
    // Template: text with [Original name], [1], [01], [001] (numbering
    // starts at `start`). Replace: find `text`, replace with `replacement`.
    QString text;
    QString replacement;
    bool caseSensitive = false;
    int start = 1;
};

// Placeholders understood by RenameOptions::Mode::Template.
QString originalNameTag();          // "[Original name]"
QStringList numberTags();           // "[1]", "[01]", "[001]"

// The new name for `name` (the n-th item, from 0). The extension is kept:
// only the base name is changed.
QString renamed(const QString &name, bool isDir, int n, const RenameOptions &options);

// Why `name` can't be used as a file name, or an empty string.
QString nameProblem(const QString &name);

struct RenameItem
{
    QString path;    // current full path
    QString newName; // wanted name in the same folder
};

// Checks a batch. Fills `problems` (one per item, empty when fine): bad
// names, two items getting the same name, or a name taken by a file that
// isn't part of the batch. Returns true if every item is fine.
bool checkBatch(const QList<RenameItem> &items, QStringList *problems);

// Renames a checked batch. Items whose names are taken by other items of
// the batch go after them; swaps and cycles go through a temporary hidden
// name. Returns each step done, in order, so FileOperations::recordUndo
// can reverse it (an undo of a Move runs the steps backwards). On failure,
// every step done is rolled back, `error` says why and the result is empty.
QList<TransferResult> applyBatch(const QList<RenameItem> &items, QString *error);

} // namespace Naming

// The user's Templates folder (New Document ▸): on Linux XDG_TEMPLATES_DIR
// from user-dirs.dirs, else ~/Templates. Empty when there is none, or when
// it is set to the home folder (the XDG way to turn it off).
namespace Templates {

QString folder();

struct Entry
{
    QString name;          // shown name (no extension for files)
    QString path;          // full path of the template file, or the folder
    QList<Entry> children; // for sub-folders (they become sub-menus)
    bool isFolder = false;
};

// The files to offer, sorted naturally; hidden files and empty
// sub-folders are left out. At most `depth` levels of sub-folders.
QList<Entry> list(const QString &folder, int depth = 3);

// Copies `templatePath` into `dir` under its own name ("Letter.odt",
// then "Letter (copy).odt"…), never overwriting. Returns the new path, or
// an empty string (with `error`) on failure.
QString create(const QString &templatePath, const QString &dir, QString *error = nullptr);

} // namespace Templates
