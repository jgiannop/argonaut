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

bool FileManagerService::registerOn(const QDBusConnection &bus, IfTaken ifTaken)
{
    QDBusConnection connection = bus;
    if (!connection.isConnected())
        return false;
    if (!connection.registerObject(QString::fromLatin1(ObjectPath), this, QDBusConnection::ExportAdaptors))
        return false;
    QDBusConnectionInterface *iface = connection.interface();
    // Connected before asking, so a name granted at once can't be missed.
    connect(iface, &QDBusConnectionInterface::serviceRegistered, this, [this](const QString &name) {
        if (name != QLatin1String(ServiceName) || !m_registered || m_owns)
            return;
        m_owns = true;
        emit nameAcquired();
    });
    const auto reply = iface->registerService(
        QString::fromLatin1(ServiceName),
        ifTaken == IfTaken::WaitInLine ? QDBusConnectionInterface::QueueService
                                       : QDBusConnectionInterface::DontQueueService,
        QDBusConnectionInterface::AllowReplacement);
    const bool owns = reply.isValid() && reply.value() == QDBusConnectionInterface::ServiceRegistered;
    const bool queued = reply.isValid() && reply.value() == QDBusConnectionInterface::ServiceQueued;
    if (!owns && !queued) {
        disconnect(iface, nullptr, this, nullptr);
        connection.unregisterObject(QString::fromLatin1(ObjectPath));
        return false;
    }
    m_bus = connection.name();
    m_registered = true;
    m_owns = owns;
    if (owns)
        emit nameAcquired();
    return true;
}

void FileManagerService::unregister()
{
    if (!m_registered)
        return;
    QDBusConnection connection(m_bus);
    disconnect(connection.interface(), nullptr, this, nullptr);
    connection.unregisterService(QString::fromLatin1(ServiceName)); // also leaves the line
    connection.unregisterObject(QString::fromLatin1(ObjectPath));
    m_registered = false;
    m_owns = false;
}

#include "FileManagerService.moc"
