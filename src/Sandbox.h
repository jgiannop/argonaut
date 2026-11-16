#pragma once

#include <QByteArray>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

// Runs a tool on one untrusted file inside bubblewrap (core code): read-only
// /usr and /etc, its own empty /tmp, no network (--unshare-all), and only
// that file visible, at /tmp/<name>. Used for pdftotext (Organizer) and
// exiftool (MediaInfo); thumbnailers have their own, similar box.
namespace Sandbox {

bool available(); // bwrap is installed
// …and the system lets us make a box with it (tried once, with a program
// that does nothing). On Ubuntu 24.04 and later a program may make a
// sandbox only if an AppArmor profile says so: the package brings one for
// /usr/bin/argonaut (data/apparmor/argonaut-files); an Argonaut started from
// somewhere else by the desktop is refused, and everything that needs the
// box then shows nothing. About → System says which it is.
bool works();

// The bwrap arguments every box starts with: read-only /usr and /etc (and
// the /bin, /lib… links of merged-/usr systems), /proc, /dev, no network or
// other namespaces shared, and the box dies with us. The caller adds its own
// /tmp and the one file to read.
QStringList systemArgs();

// The environment for a program that reads an untrusted file: only what it
// needs to run (PATH, HOME, the language and time zone, the XDG cache and
// data folders). Everything else stays out, so secrets in the environment
// (ANTHROPIC_API_KEY, tokens, D-Bus and display addresses) never reach a
// parser. Also used for thumbnailers.
QProcessEnvironment cleanEnvironment();

// Runs `program` with `args`, where "@FILE@" stands for the file inside the
// box. Returns its standard output (at most `maxBytes`); `ok` is false if
// it couldn't run, failed or took longer than `timeoutMs`. Without bwrap it
// does nothing (returns false): untrusted files are never parsed unboxed.
// A program outside the system folders (one of ours in a build folder) is
// added to the box read-only, at its own path.
QByteArray run(const QString &program, const QStringList &args, const QString &file, bool *ok = nullptr,
               int timeoutMs = 15000, qsizetype maxBytes = 1024 * 1024);

} // namespace Sandbox
