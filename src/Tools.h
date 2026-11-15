#pragma once

#include <QString>

// The outside programs Argonaut runs (gio, git, tracker3, systemctl…), core
// code. Each has an ARGONAUT_*_PROGRAM environment variable that replaces it:
// tests point those at fake scripts, so a test never mounts, ejects, launches
// or changes anything real.
namespace Tools {

// What to start for `name`: the variable's value when it is set, else `name`
// itself (found in PATH when it is started).
QString program(const char *variable, const QString &name);

// The same as a full path, or empty when `name` isn't installed. Looked up
// once per name; safe to call from any thread.
QString find(const char *variable, const QString &name);

// GNOME's `gio` (mounts, default apps, launching): ARGONAUT_GIO_PROGRAM.
QString gio();

} // namespace Tools
