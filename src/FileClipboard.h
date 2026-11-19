#pragma once

#include <QByteArray>
#include <QStringList>

// Files on the clipboard, for the terminal interface, which has no window
// system and so no QClipboard of its own (the window uses FileOperations'
// clipboard functions; the format is the same, so the two understand each
// other and so does Nautilus).
//
// Where there is a desktop, its clipboard is reached through a helper
// program: wl-copy / wl-paste (Wayland) or xclip (X11). Without a desktop
// or a helper, the interface keeps what was copied to itself.
namespace FileClipboard {

// "x-special/gnome-copied-files": "copy" or "cut", then a file URL a line.
extern const char *const GnomeFormat;
QByteArray encode(const QStringList &paths, bool cut);
// Only local files with absolute paths come out of it; false: none.
bool decode(const QByteArray &data, QStringList *paths, bool *cut);
// "text/uri-list" (what a program that knows nothing of cutting offers).
bool decodeUriList(const QByteArray &data, QStringList *paths);

// The desktop's clipboard can be reached: a display and a helper program.
// Off in tests (ARGONAUT_CLIPBOARD=0/1; unset it is off wherever
// ARGONAUT_TRASH_HOME_ONLY is set), so none touches the real clipboard;
// ARGONAUT_CLIPBOARD_PROGRAM is a pretend xclip for the tests about it.
bool desktopAvailable();
// There is a desktop but no helper program: what to install to share the
// clipboard with other programs ("xclip"); empty when nothing is missing
// (or there is no desktop to share with).
QString missingHelper();

enum class Read { Unavailable, Empty, Files };
bool copyToDesktop(const QStringList &paths, bool cut);
Read readDesktop(QStringList *paths, bool *cut);
void clearDesktop(); // a cut is pasted once
// Text (lines copied from the preview) on the desktop's clipboard.
bool copyTextToDesktop(const QString &text);

} // namespace FileClipboard
