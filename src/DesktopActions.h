#pragma once

#include <QList>
#include <QRect>
#include <QString>
#include <QStringList>

// Things the desktop does with files, started from the file manager (core
// code, no widgets): email them, use one as the wallpaper, run the user's
// Nautilus scripts on them. Each runs an outside program; see Tools for the
// ARGONAUT_*_PROGRAM variables that replace them in tests.
namespace DesktopActions {

// Opens the email app with the files attached (xdg-email). It sends nothing
// itself. False if the app couldn't be started.
bool sendByEmail(const QStringList &paths);

// GNOME: makes `image` the desktop background. The image is first copied
// into ~/.local/share/backgrounds, so moving or deleting the original later
// doesn't break the background. On failure `error` says why.
bool setAsBackground(const QString &image, QString *error = nullptr);

// The user's scripts: ~/.local/share/nautilus/scripts, the folder Nautilus
// uses, so existing scripts work unchanged.
QString scriptsFolder();

struct Script
{
    QString name;
    QString path;           // empty for a folder
    QList<Script> children; // a folder's scripts (never empty for a folder)
    bool isFolder() const { return path.isEmpty(); }
};
// Executable files in `folder`, and its sub-folders that hold any (at most
// three levels down), folders first, by name.
QList<Script> scripts(const QString &folder);

// Runs a script the way Nautilus does: in `currentFolder`, with the selected
// items as arguments (names for items of that folder, full paths otherwise)
// and the NAUTILUS_SCRIPT_* variables set. False if it couldn't be started.
bool runScript(const QString &script, const QString &currentFolder, const QStringList &selection,
               const QRect &windowGeometry);

} // namespace DesktopActions
