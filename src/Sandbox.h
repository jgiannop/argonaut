#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

// Runs a tool on one untrusted file inside bubblewrap (core code): read-only
// /usr and /etc, its own empty /tmp, no network (--unshare-all), and only
// that file visible, at /tmp/<name>. Used for pdftotext (Organizer) and
// exiftool (MediaInfo); thumbnailers have their own, similar box.
namespace Sandbox {

bool available(); // bwrap is installed

// Runs `program` with `args`, where "@FILE@" stands for the file inside the
// box. Returns its standard output (at most `maxBytes`); `ok` is false if
// it couldn't run, failed or took longer than `timeoutMs`. Without bwrap it
// does nothing (returns false): untrusted files are never parsed unboxed.
QByteArray run(const QString &program, const QStringList &args, const QString &file, bool *ok = nullptr,
               int timeoutMs = 15000, qsizetype maxBytes = 1024 * 1024);

} // namespace Sandbox
