#pragma once

#include <QObject>
#include <QStringList>

class QDBusConnection;

// org.freedesktop.FileManager1 on the session bus: what apps call for "Show
// in folder" (ShowItems), "Open folder" (ShowFolders) and "Properties"
// (ShowItemProperties). Requests arrive as signals with local paths; the
// app opens windows for them. Owned only by `argonaut --service`, which
// D-Bus starts through the service file DefaultFileManager writes. Linux.
class FileManagerService : public QObject
{
    Q_OBJECT

public:
    static constexpr const char *ServiceName = "org.freedesktop.FileManager1";
    static constexpr const char *ObjectPath = "/org/freedesktop/FileManager1";

    explicit FileManagerService(QObject *parent = nullptr);
    ~FileManagerService() override;

    // Exports the object and takes the name. False if someone else has it.
    bool registerOn(const QDBusConnection &bus);
    void unregister();
    bool isRegistered() const { return m_registered; }

    // "file:///a%20b" -> "/a b"; other URIs are skipped.
    static QStringList localPaths(const QStringList &uris);

signals:
    void showFolders(const QStringList &paths, const QString &startupId);
    void showItems(const QStringList &paths, const QString &startupId);
    void showItemProperties(const QStringList &paths, const QString &startupId);

private:
    QString m_bus; // connection name while registered
    bool m_registered = false;
};
