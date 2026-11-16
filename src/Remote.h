#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>

class QProcess;

// Remote locations (Connect to Server): sftp://, smb://, ftp://, WebDAV…
// mounted by GVfs (`gio mount`, Linux). With gvfs-fuse every mount is also a
// folder under $XDG_RUNTIME_DIR/gvfs, so the rest of Argonaut (listing, copy,
// undo, preview) works on it like on any other folder. Core code, no widgets.
namespace Remote {

// "sftp://host", "smb://nas/share", "ftp://…", "dav(s)://…", "afp://…",
// "nfs://…", "mtp://…": addresses GVfs mounts. Not file:// or http(s)://.
bool isRemoteAddress(const QString &text);

// The folder GVfs puts mounts in ($XDG_RUNTIME_DIR/gvfs); empty if none.
// ARGONAUT_GVFS_ROOT replaces it (tests).
QString gvfsRoot();

struct Mount
{
    QString name;      // "john@example.com", "photos on nas", "Pixel 7"
    QString address;   // "sftp://john@example.com/", "smb://nas/photos/"
    QString localPath; // its folder under gvfsRoot()
    QString scheme;    // "sftp", "smb", "mtp"…
};

// The mounts now, from the folder names GVfs uses ("sftp:host=x,user=y",
// "smb-share:server=nas,share=photos"…): no program is run.
QList<Mount> mounts();
// Parses one folder name (exposed for tests); false if it isn't a mount.
bool parseMountDir(const QString &dirName, Mount *mount);
// The mount a path is on (its localPath), or an empty Mount.
Mount mountOf(const QString &path);

// Mounts one address with `gio mount`, answering its questions through
// signals (it asks on its standard input: user, domain, password, and
// choices such as an unknown host key). ARGONAUT_GIO_PROGRAM replaces gio.
class Mounter : public QObject
{
    Q_OBJECT

public:
    explicit Mounter(const QString &address, QObject *parent = nullptr);
    ~Mounter() override;

    QString address() const { return m_address; }
    void start(bool anonymous = false);

    struct Login
    {
        QString user;
        QString domain;
        QString password;
    };
    void answer(const Login &login); // after needLogin
    void choose(int choice);         // after question: the number shown next to it
    void cancel();

signals:
    // `retry`: the last answer was wrong. `askUser`/`askDomain`: whether
    // gio will ask for them (sftp with user@ in the address asks only for
    // the password). Defaults are what gio proposes.
    void needLogin(const QString &message, bool askUser, const QString &defaultUser, bool askDomain,
                   const QString &defaultDomain, bool retry);
    void question(const QString &message, const QStringList &choices, const QList<int> &numbers);
    // Done: `localPath` is the folder to open (empty if the address has
    // none, e.g. smb://server/ itself: a list of shares).
    void finished(bool ok, const QString &localPath, const QString &error);

private:
    void readOutput();
    void processFinished(int code);
    void resolveLocalPath();
    void write(const QString &line);

    QString m_address;
    QPointer<QProcess> m_process;
    QByteArray m_buffer;   // output since the last answered prompt
    QString m_message;     // text before the prompts ("Enter user and password for…")
    bool m_haveLogin = false;
    bool m_askedOnce = false;
    Login m_login;
    QString m_pendingPrompt; // the prompt waiting for an answer
    QString m_error;
    bool m_done = false;
};

// The servers connected to before, newest first (at most 10), in Argonaut's
// settings ("network/recent").
QStringList recentServers();
void rememberServer(const QString &address);

// Disconnects a mount (`gio mount -u`). Runs in the background; calls
// `done(ok, error)` when finished.
void unmount(const QString &address, QObject *context, const std::function<void(bool, const QString &)> &done);

} // namespace Remote
