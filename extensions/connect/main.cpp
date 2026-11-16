// Connect to Server, as an extension: other computers as folders (sftp, smb,
// ftp, WebDAV…), mounted by GVfs (Remote, in Argonaut's core library).
//
// It adds "Connect to Server…" to the Go menu and rows to the sidebar (the
// servers connected now, each with ⏏; network bookmarks not connected; and
// "Connect to Server…"), and the kinds of address in its extension.json are
// its to open when typed in the path bar. What a server asks on the way (a
// login, an unknown host key, which shared folder) is asked with forms;
// when the server's folder is there, Argonaut is asked to open it.

#include "../shared/Conversation.h"
#include "Bookmarks.h"
#include "Remote.h"
#include "Tools.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QSet>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#include <cstdio>
#include <unistd.h>

namespace {

void say(const QString &text)
{
    send({{QStringLiteral("type"), QStringLiteral("message")}, {QStringLiteral("text"), text}});
}

QString hostOf(const QString &address)
{
    const QString host = QUrl(address).host();
    return host.isEmpty() ? address : host;
}

class Connector : public QObject
{
public:
    explicit Connector(QObject *parent = nullptr) : QObject(parent)
    {
        // The servers and the bookmarks change behind our back (another
        // program connects; Nautilus edits the bookmarks).
        m_later.setSingleShot(true);
        m_later.setInterval(300);
        connect(&m_later, &QTimer::timeout, this, [this] { sendPlaces(); });
        connect(&m_watcher, &QFileSystemWatcher::directoryChanged, &m_later, qOverload<>(&QTimer::start));
        watch();
    }

    void handle(const QJsonObject &message)
    {
        const QString type = message.value(QLatin1String("type")).toString();
        if (type == QLatin1String("hello")) {
            send({{QStringLiteral("type"), QStringLiteral("commands")},
                  {QStringLiteral("commands"), QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("connect")},
                                                                      {QStringLiteral("label"), QStringLiteral("Connect to Server…")},
                                                                      {QStringLiteral("menu"), QStringLiteral("go")}}}}});
            sendPlaces(true);
        } else if (type == QLatin1String("watch") || type == QLatin1String("refresh")) {
            watch();      // GVfs's folder appears with the first mount
            sendPlaces(); // only if they changed
        } else if (type == QLatin1String("invoke")) {
            if (message.value(QLatin1String("command")).toString() == QLatin1String("connect"))
                askAddress();
        } else if (type == QLatin1String("address")) {
            connectTo(message.value(QLatin1String("address")).toString());
        } else if (type == QLatin1String("place")) {
            const QString id = message.value(QLatin1String("id")).toString();
            const bool eject = message.value(QLatin1String("action")).toString() == QLatin1String("eject");
            if (id == QLatin1String("connect") && !eject)
                askAddress();
            else if (id.startsWith(QLatin1String("bookmark:")) && !eject)
                connectTo(id.mid(9));
            else if (id.startsWith(QLatin1String("mount:")) && eject)
                disconnectFrom(id.mid(6));
        } else if (type == QLatin1String("form")) {
            answered(message);
        }
    }

private:
    void watch()
    {
        const QString gvfs = Remote::gvfsRoot();
        if (!gvfs.isEmpty() && !m_watcher.directories().contains(gvfs))
            m_watcher.addPath(gvfs);
        const QString bookmarks = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + QStringLiteral("/gtk-3.0");
        if (QFileInfo(bookmarks).isDir() && !m_watcher.directories().contains(bookmarks))
            m_watcher.addPath(bookmarks);
    }

    // The sidebar rows: what is connected, the bookmarks that aren't, and
    // the way to connect. Said again only when something is different.
    void sendPlaces(bool always = false)
    {
        QJsonArray places;
        QSet<QString> connected;
        const QList<Remote::Mount> mounts = Remote::mounts();
        for (const Remote::Mount &m : mounts) {
            places.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("mount:") + m.address},
                                      {QStringLiteral("label"), m.name},
                                      {QStringLiteral("icon"), QStringLiteral("network")},
                                      {QStringLiteral("folder"), m.localPath},
                                      {QStringLiteral("eject"), true},
                                      {QStringLiteral("tooltip"), m.address}});
            connected.insert(QUrl(m.address).host());
        }
        const QList<Bookmarks::Bookmark> bookmarks = Bookmarks::loadRemote();
        for (const Bookmarks::Bookmark &b : bookmarks) {
            const QUrl url(b.path);
            if (connected.contains(url.host()))
                continue;
            places.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("bookmark:") + b.path},
                                      {QStringLiteral("label"), b.label.isEmpty() ? url.host() : b.label},
                                      {QStringLiteral("icon"), QStringLiteral("network")},
                                      {QStringLiteral("tooltip"), b.path}});
        }
        places.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("connect")},
                                  {QStringLiteral("label"), QStringLiteral("Connect to Server…")},
                                  {QStringLiteral("icon"), QStringLiteral("network")},
                                  {QStringLiteral("tooltip"), QStringLiteral("sftp, smb, ftp, WebDAV…")}});
        const QByteArray now = QJsonDocument(places).toJson(QJsonDocument::Compact);
        if (!always && now == m_placesSent)
            return;
        m_placesSent = now;
        send({{QStringLiteral("type"), QStringLiteral("places")}, {QStringLiteral("places"), places}});
    }

    void form(const QString &id, QJsonObject form)
    {
        m_form = id;
        form.insert(QStringLiteral("type"), QStringLiteral("form"));
        form.insert(QStringLiteral("id"), id);
        send(form);
    }

    // Go → Connect to Server…: which server.
    void askAddress()
    {
        QJsonObject address{{QStringLiteral("id"), QStringLiteral("address")},
                            {QStringLiteral("label"), QStringLiteral("Server address")},
                            {QStringLiteral("placeholder"), QStringLiteral("sftp://example.com")}};
        const QStringList recent = Remote::recentServers();
        if (!recent.isEmpty())
            address.insert(QStringLiteral("choices"), QJsonArray::fromStringList(recent)); // the servers used before
        form(QStringLiteral("address"),
             {{QStringLiteral("title"), QStringLiteral("Connect to Server")},
              {QStringLiteral("text"), QStringLiteral("For example sftp://me@example.com (SSH), smb://nas/photos (Windows share), "
                                                      "ftp://ftp.example.org, or davs://cloud.example.com/dav (WebDAV).")},
              {QStringLiteral("fields"), QJsonArray{address}},
              {QStringLiteral("buttons"), QJsonArray{QStringLiteral("Connect")}}});
    }

    void connectTo(const QString &address)
    {
        QString clean = address.trimmed();
        if (clean.isEmpty())
            return;
        if (!clean.contains(QLatin1String("://")))
            clean.prepend(QStringLiteral("sftp://")); // a bare host name: SSH, like Nautilus
        if (clean.startsWith(QLatin1String("ssh://")))
            clean.replace(0, 3, QStringLiteral("sftp")); // GVfs knows it as sftp
        if (!Remote::isRemoteAddress(clean)) {
            say(QStringLiteral("Argonaut can connect with sftp, smb, ftp, ftps, dav, davs, afp and nfs."));
            return;
        }
        if (QUrl(clean).host().isEmpty() && !clean.startsWith(QLatin1String("mtp"))) {
            say(QStringLiteral("The address needs a server name."));
            return;
        }
        if (m_mounter) {
            say(QStringLiteral("Still connecting to %1…").arg(hostOf(m_mounter->address())));
            return;
        }
        auto *mounter = new Remote::Mounter(clean, this);
        m_mounter = mounter;
        say(QStringLiteral("Connecting to %1…").arg(hostOf(clean)));
        connect(mounter, &Remote::Mounter::needLogin, this,
                [this, clean](const QString &message, bool askUser, const QString &defaultUser, bool askDomain, const QString &defaultDomain,
                              bool retry) {
                    QJsonArray fields;
                    // Anonymous: for servers that allow it (smb, ftp, WebDAV), not ssh.
                    if (!clean.startsWith(QLatin1String("sftp")))
                        fields.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("anonymous")},
                                                  {QStringLiteral("kind"), QStringLiteral("check")},
                                                  {QStringLiteral("label"), QStringLiteral("Connect anonymously")},
                                                  {QStringLiteral("alone"), true}});
                    if (askUser)
                        fields.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("user")},
                                                  {QStringLiteral("label"), QStringLiteral("User")},
                                                  {QStringLiteral("value"), defaultUser}});
                    if (askDomain)
                        fields.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("domain")},
                                                  {QStringLiteral("label"), QStringLiteral("Domain")},
                                                  {QStringLiteral("value"), defaultDomain}});
                    fields.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("password")},
                                              {QStringLiteral("kind"), QStringLiteral("secret")},
                                              {QStringLiteral("label"), QStringLiteral("Password")}});
                    QJsonObject login{{QStringLiteral("title"), QStringLiteral("Log In")},
                                      {QStringLiteral("heading"), QStringLiteral("Log in to %1").arg(hostOf(clean))},
                                      {QStringLiteral("text"), message.isEmpty() ? clean : message},
                                      {QStringLiteral("fields"), fields},
                                      {QStringLiteral("buttons"), QJsonArray{QStringLiteral("Connect")}}};
                    if (retry)
                        login.insert(QStringLiteral("warning"), QStringLiteral("That user name or password didn't work. Try again."));
                    form(QStringLiteral("login"), login);
                });
        connect(mounter, &Remote::Mounter::question, this,
                [this](const QString &message, const QStringList &choices, const QList<int> &numbers) {
                    // An unknown SSH host key: "Log In Anyway" / "Cancel Login".
                    m_numbers = numbers;
                    form(QStringLiteral("question"), {{QStringLiteral("title"), QStringLiteral("Connect to Server")},
                                                      {QStringLiteral("text"), message},
                                                      {QStringLiteral("buttons"), QJsonArray::fromStringList(choices)}});
                });
        connect(mounter, &Remote::Mounter::finished, this, [this, mounter, clean](bool ok, const QString &local, const QString &error) {
            mounter->deleteLater();
            if (m_mounter == mounter)
                m_mounter = nullptr;
            m_form.clear();
            watch();
            sendPlaces();
            if (!ok) {
                if (!error.isEmpty())
                    say(error);
                return;
            }
            Remote::rememberServer(clean);
            if (local.isEmpty()) {
                chooseShare(clean); // smb://server/: a list of shares, no folder
                return;
            }
            send({{QStringLiteral("type"), QStringLiteral("open")}, {QStringLiteral("folder"), local}});
        });
        mounter->start();
    }

    // smb://server/ has no folder of its own: one of its shares.
    void chooseShare(const QString &server)
    {
        auto *list = new QProcess(this);
        connect(list, &QProcess::finished, this, [this, list, server] {
            QStringList shares;
            const QStringList lines = QString::fromUtf8(list->readAllStandardOutput()).split(QLatin1Char('\n'));
            for (const QString &line : lines) {
                const QString name = line.trimmed();
                if (!name.isEmpty() && !name.endsWith(QLatin1Char('$'))) // hidden admin shares (C$, IPC$)
                    shares << name;
            }
            list->deleteLater();
            if (shares.isEmpty()) {
                say(QStringLiteral("%1 has no shared folders").arg(hostOf(server)));
                return;
            }
            m_server = server;
            form(QStringLiteral("share"),
                 {{QStringLiteral("title"), QStringLiteral("Shared Folders")},
                  {QStringLiteral("text"), QStringLiteral("Shared folders on %1:").arg(hostOf(server))},
                  {QStringLiteral("fields"), QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("share")},
                                                                    {QStringLiteral("kind"), QStringLiteral("choice")},
                                                                    {QStringLiteral("label"), QStringLiteral("Folder")},
                                                                    {QStringLiteral("choices"), QJsonArray::fromStringList(shares)}}}},
                  {QStringLiteral("buttons"), QJsonArray{QStringLiteral("Open")}}});
        });
        connect(list, &QProcess::errorOccurred, this, [list](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart)
                list->deleteLater();
        });
        list->start(Tools::gio(), {QStringLiteral("list"), server});
    }

    // What the user filled in, or that they backed out.
    void answered(const QJsonObject &message)
    {
        const QString id = message.value(QLatin1String("id")).toString();
        if (id != m_form)
            return; // not the question that is open
        m_form.clear();
        const bool cancelled = message.value(QLatin1String("cancelled")).toBool();
        const QJsonObject values = message.value(QLatin1String("values")).toObject();
        if (id == QLatin1String("address")) {
            if (!cancelled)
                connectTo(values.value(QLatin1String("address")).toString());
        } else if (id == QLatin1String("login")) {
            if (!m_mounter)
                return;
            if (cancelled)
                m_mounter->cancel();
            else if (values.value(QLatin1String("anonymous")).toString() == QLatin1String("true"))
                m_mounter->start(true);
            else
                m_mounter->answer({values.value(QLatin1String("user")).toString(), values.value(QLatin1String("domain")).toString(),
                                   values.value(QLatin1String("password")).toString()});
        } else if (id == QLatin1String("question")) {
            if (!m_mounter)
                return;
            const int at = message.value(QLatin1String("button")).toInt(-1);
            if (cancelled || at < 0)
                m_mounter->cancel();
            else
                m_mounter->choose(m_numbers.value(at, at));
        } else if (id == QLatin1String("share")) {
            const QString share = values.value(QLatin1String("share")).toString();
            if (cancelled || share.isEmpty())
                return;
            QString base = m_server;
            if (!base.endsWith(QLatin1Char('/')))
                base += QLatin1Char('/');
            connectTo(base + QString::fromUtf8(QUrl::toPercentEncoding(share)));
        }
    }

    void disconnectFrom(const QString &address)
    {
        QString name = hostOf(address);
        const QList<Remote::Mount> mounts = Remote::mounts();
        for (const Remote::Mount &m : mounts) {
            if (m.address == address)
                name = m.name;
        }
        say(QStringLiteral("Disconnecting from “%1”…").arg(name));
        Remote::unmount(address, this, [this, name](bool ok, const QString &error) {
            say(ok ? QStringLiteral("Disconnected from “%1”").arg(name) : QStringLiteral("Couldn't disconnect from “%1”: %2").arg(name, error));
            sendPlaces();
        });
    }

    QFileSystemWatcher m_watcher;
    QTimer m_later;
    QByteArray m_placesSent;
    QPointer<Remote::Mounter> m_mounter; // the connection being made (one at a time)
    QString m_form;                      // the form that is open
    QList<int> m_numbers;                // "question": gio's number for each button
    QString m_server;                    // "share": whose shares
};

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    // Argonaut's own settings: the servers used before are kept there.
    QCoreApplication::setOrganizationName(QStringLiteral("Argonaut"));
    QCoreApplication::setApplicationName(QStringLiteral("Argonaut"));

    Connector connector;
    Conversation conversation([&](const QJsonObject &message) {
        connector.handle(message);
    });
    return app.exec();
}
