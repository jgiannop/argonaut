#include "FileManagerService.h"

#include <QDBusAbstractAdaptor>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDir>
#include <QUrl>

namespace {

// The D-Bus face of FileManagerService (method names are the spec's).
class FileManagerAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.FileManager1")

public:
    explicit FileManagerAdaptor(FileManagerService *service)
        : QDBusAbstractAdaptor(service), m_service(service)
    {
    }

public slots:
    void ShowFolders(const QStringList &URIs, const QString &StartupId)
    {
        emit m_service->showFolders(FileManagerService::localPaths(URIs), StartupId);
    }
    void ShowItems(const QStringList &URIs, const QString &StartupId)
    {
        emit m_service->showItems(FileManagerService::localPaths(URIs), StartupId);
    }
    void ShowItemProperties(const QStringList &URIs, const QString &StartupId)
    {
        emit m_service->showItemProperties(FileManagerService::localPaths(URIs), StartupId);
    }

private:
    FileManagerService *m_service;
};

} // namespace

FileManagerService::FileManagerService(QObject *parent)
    : QObject(parent)
{
    new FileManagerAdaptor(this);
}

FileManagerService::~FileManagerService()
{
    unregister();
}

QStringList FileManagerService::localPaths(const QStringList &uris)
{
    QStringList paths;
    for (const QString &uri : uris) {
        const QUrl url(uri);
        if (url.isLocalFile())
            paths << QDir::cleanPath(url.toLocalFile());
        else if (url.scheme().isEmpty() && QDir::isAbsolutePath(uri))
            paths << QDir::cleanPath(uri); // some callers send plain paths
    }
    return paths;
}

bool FileManagerService::registerOn(const QDBusConnection &bus)
{
    QDBusConnection connection = bus;
    if (!connection.isConnected())
        return false;
    if (!connection.registerObject(QString::fromLatin1(ObjectPath), this, QDBusConnection::ExportAdaptors))
        return false;
    // No queueing: if Nautilus (or another Kaftilous) has it, we don't.
    const auto reply = connection.interface()->registerService(
        QString::fromLatin1(ServiceName), QDBusConnectionInterface::DontQueueService,
        QDBusConnectionInterface::AllowReplacement);
    if (!reply.isValid() || reply.value() != QDBusConnectionInterface::ServiceRegistered) {
        connection.unregisterObject(QString::fromLatin1(ObjectPath));
        return false;
    }
    m_bus = connection.name();
    m_registered = true;
    return true;
}

void FileManagerService::unregister()
{
    if (!m_registered)
        return;
    QDBusConnection connection(m_bus);
    connection.unregisterService(QString::fromLatin1(ServiceName));
    connection.unregisterObject(QString::fromLatin1(ObjectPath));
    m_registered = false;
}

#include "FileManagerService.moc"
