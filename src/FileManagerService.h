#pragma once

#include <QObject>
#include <QStringList>

class QDBusConnection;

// org.freedesktop.FileManager1 on the session bus: what apps call for "Show
// in folder" (ShowItems), "Open folder" (ShowFolders) and "Properties"
// (ShowItemProperties). Requests arrive as signals with local paths; the
// app opens windows for them. Owned only by `argonaut --service`, which
// D-Bus starts through the service file DefaultFileManager writes. Linux.
//
// Nautilus also asks for this name whenever it runs (Ubuntu's desktop icons
// start it in the background) and waits in line if it's taken. So the
// service waits in line too (WaitInLine) and, once it has the name, keeps
// it: `argonaut --service` stays running while Argonaut is the default.
class FileManagerService : public QObject
{
    Q_OBJECT

public:
    static constexpr const char *ServiceName = "org.freedesktop.FileManager1";
    static constexpr const char *ObjectPath = "/org/freedesktop/FileManager1";

    explicit FileManagerService(QObject *parent = nullptr);
    ~FileManagerService() override;

    enum class IfTaken { Refuse, WaitInLine };

    // Exports the object and asks for the name. With Refuse, false if someone
    // else has it; with WaitInLine, true and ownsName() turns true (and
    // nameAcquired fires) when the owner lets go.
    bool registerOn(const QDBusConnection &bus, IfTaken ifTaken = IfTaken::Refuse);
    void unregister();
    bool isRegistered() const { return m_registered; }
    bool ownsName() const { return m_owns; }

    // "file:///a%20b" -> "/a b"; other URIs are skipped.
    static QStringList localPaths(const QStringList &uris);

signals:
    void showFolders(const QStringList &paths, const QString &startupId);
    void showItems(const QStringList &paths, const QString &startupId);
    void showItemProperties(const QStringList &paths, const QString &startupId);
    void nameAcquired();

private:
    QString m_bus; // connection name while registered
    bool m_registered = false; // object exported and name owned or asked for
    bool m_owns = false;
};
