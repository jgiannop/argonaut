#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QSet>
#include <QStringList>

// Starred files and folders: the Starred place, and the ★ on their icons.
// Kept in $XDG_DATA_HOME/kosmos/starred (one file:// URI per line).
// (Nautilus keeps its stars in GNOME's Tracker database, which other apps
// can't use reliably, so this is Kosmos's own list.)
// Stars follow items that Kosmos renames or moves (renamed()). Items that
// are gone (e.g. in the Trash) stay starred but aren't shown, so restoring
// them brings the star back. GUI thread only.
class Starred : public QObject
{
    Q_OBJECT

public:
    static Starred &instance();

    bool isStarred(const QString &path) const;
    void setStarred(const QStringList &paths, bool starred);
    QStringList paths() const; // in the order they were starred, newest first
    // `from` now lives at `to` (also everything inside, for folders).
    void renamed(const QString &from, const QString &to);
    QString filePath() const;

signals:
    void changed();

private:
    Starred();
    void load();
    void save();

    QStringList m_order; // newest first
    QSet<QString> m_set;
    QFileSystemWatcher m_watcher; // other Kosmos windows/processes
    bool m_saving = false;
};
