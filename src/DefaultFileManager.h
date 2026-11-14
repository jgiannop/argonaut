#pragma once

#include <QString>

// Making Argonaut the desktop's file manager, and giving it back (Linux).
// Two hooks, both per user, both reversible; nothing of Nautilus is changed:
//  1. The default app for folders (inode/directory, via `gio mime`, i.e.
//     ~/.config/mimeapps.list): folders opened from other apps, Super+E,
//     desktop icons, "Open containing folder".
//  2. org.freedesktop.FileManager1 on D-Bus ("Show in folder" in Firefox,
//     Chrome…): a user service file in $XDG_DATA_HOME/dbus-1/services, which
//     takes priority over Nautilus's system one, starts `argonaut --service`.
// The previous default is remembered (QSettings "system/previousFileManager")
// and restored; Nautilus if there was none.
namespace DefaultFileManager {

QString desktopId();              // "argonaut.desktop"
QString installedDesktopFile();   // the installed one, or empty (not installed)
QString installedExecutable();    // absolute path from its Exec line, or empty
QString currentDefault();         // desktop id that opens folders now
bool isArgonautDefault();
QString serviceFilePath();        // our D-Bus service file
bool serviceFileIsOurs();

bool makeDefault(QString *error = nullptr);
// Back to the remembered default (or Nautilus). `restoredTo` gets its id.
bool restorePrevious(QString *error = nullptr, QString *restoredTo = nullptr);

// Starts the installed `argonaut --service` in the background, so it takes
// "Show in folder" now (or waits in line behind a running Nautilus) instead
// of at the next request. It quits by itself once Argonaut isn't the
// default any more. Not called by makeDefault(), so tests start nothing.
bool startService();

} // namespace DefaultFileManager
