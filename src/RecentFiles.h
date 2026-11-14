#pragma once

#include <QList>
#include <QString>
#include <QStringList>

// The freedesktop "recently used" list (~/.local/share/recently-used.xbel),
// which GTK apps, LibreOffice and Nautilus share. Argonaut shows it as the
// Recent place and adds the files it opens, as Nautilus does.
//
// Writing keeps everything else in the file (other apps' entries, groups,
// metadata) and replaces it atomically (QSaveFile), like GTK.
namespace RecentFiles {

struct Item
{
    QString path; // local file or folder
    qint64 used = 0; // ms since epoch: the latest of added/modified/visited
};

QString filePath(); // $XDG_DATA_HOME/recently-used.xbel
// Local items, most recent first, at most `max`. Doesn't check existence.
QList<Item> read(int max = 500);
// GNOME's privacy switch (org.gnome.desktop.privacy remember-recent-files).
// ARGONAUT_REMEMBER_RECENT=0/1 overrides it (tests).
bool rememberEnabled();
// Record that Argonaut opened `path` (no-op if remembering is off).
bool add(const QString &path, const QString &mimeType);
// Take items out of the list (the files themselves are untouched).
bool remove(const QStringList &paths);

} // namespace RecentFiles
