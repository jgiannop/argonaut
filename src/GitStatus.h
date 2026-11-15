#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTimer>

class QFileSystemWatcher;
class QProcess;

// Git status for the folder a view shows (core code, no widgets).
//
// setFolder() finds the repository the folder is in (a ".git" in it or a
// folder above, on the same drive) and runs `git status --porcelain=v2` in
// the background, at low priority and with a time limit. It runs again
// when the files change (refresh(), debounced) and when the repository's
// own data changes (git add, commit, checkout: its .git folder is watched).
//
// Safety: a repository's own config can make `git status` run programs
// (clean/smudge filters, fsmonitor), so a downloaded folder could run code
// just by being opened. Before the first status, its local config is read
// with `git config` (which runs nothing); a repository that sets any of
// those is left alone (blocked()). Submodules are never entered, and git
// never takes locks or asks for passwords.
class GitStatus : public QObject
{
    Q_OBJECT

public:
    enum State : int {
        None = 0,
        Changed,    // a folder with changes inside
        Modified,   // M: changed (staged or not), renamed, deleted inside
        Added,      // A: new and staged
        Untracked,  // U: new, not added
        Conflicted, // C: merge conflict
    };

    explicit GitStatus(QObject *parent = nullptr);
    ~GitStatus() override;

    // The folder shown; empty for none. Finds its repository.
    void setFolder(const QString &folder);
    void refresh(); // soon (files changed)

    bool inRepository() const { return !m_root.isEmpty(); }
    QString root() const { return m_root; }
    QString branch() const { return m_branch; } // "main", or a short commit id when detached
    int changeCount() const { return m_changeCount; }
    bool blocked() const { return m_blocked; }   // its config runs programs: no status
    bool tooSlow() const { return m_tooSlow; }   // status took too long: given up
    bool ready() const { return m_ready; }       // a status has arrived

    // State of an item, by full path.
    State state(const QString &path) const;

    // Every marked item inside `folder`, at any depth, by its path relative
    // to the folder. `deep` marks a folder git lists only as a whole (not
    // tracked): everything inside it has the same state. A folder that is
    // itself inside such a folder gets one mark with an empty path.
    struct Mark
    {
        QString relativePath;
        State state = None;
        bool deep = false;
    };
    QList<Mark> marks(const QString &folder) const;

    // The repository that contains `folder` (its top folder), or empty.
    static QString findRoot(const QString &folder);
    // Parses `git status --porcelain=v2 --branch -z` output (exposed for tests).
    struct Parsed
    {
        QString branch;
        QHash<QString, State> files;  // relative path -> state
        QSet<QString> untrackedDirs;  // relative, without the trailing '/'
        int changes = 0;
    };
    static Parsed parse(const QByteArray &output);

    static constexpr int TimeoutMs = 8000;

signals:
    void changed(); // states, branch or repository changed

private:
    void start();
    void checkConfigThenStatus();
    void runStatus();
    void finished(const QByteArray &output);
    void clear();
    void watchGitDir();

    QString m_folder;
    QString m_root;
    QString m_gitDir;
    QString m_branch;
    int m_changeCount = 0;
    bool m_blocked = false;
    bool m_tooSlow = false;
    bool m_ready = false;
    bool m_configChecked = false;
    QHash<QString, State> m_files;   // full path -> state
    QHash<QString, State> m_folders; // full path -> Changed / Conflicted (worst inside)
    QSet<QString> m_untrackedDirs;   // full paths
    QTimer m_debounce;
    QPointer<QProcess> m_process;
    bool m_again = false; // something changed while git was running
    QFileSystemWatcher *m_watcher = nullptr;
};
