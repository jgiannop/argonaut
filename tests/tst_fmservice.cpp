// Tests for FileManagerService (org.freedesktop.FileManager1). Runs only on a
// private bus (ctest starts it under dbus-run-session), never the desktop's:
// taking the name there would steal "Show in folder" from Nautilus.

#include "FileManagerService.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusPendingCall>
#include <QDBusReply>
#include <QSignalSpy>
#include <QtTest>

class TestFileManagerService : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void localPaths();
    void answersCalls();
    void refusesWhenTaken();

private:
    QDBusConnection bus() const { return QDBusConnection::sessionBus(); }
};

void TestFileManagerService::initTestCase()
{
    if (qEnvironmentVariable("ARGONAUT_PRIVATE_BUS") != QLatin1String("1"))
        QSKIP("runs only on a private bus (ctest uses dbus-run-session)");
    QVERIFY(bus().isConnected());
    // A desktop session has these; a private bus has neither. Stop if in doubt.
    const QStringList names = bus().interface()->registeredServiceNames().value();
    QVERIFY2(!names.contains("org.gnome.Shell") && !names.contains("org.freedesktop.Notifications"),
             "this looks like the desktop's bus, not a private one");
}

void TestFileManagerService::localPaths()
{
    QCOMPARE(FileManagerService::localPaths({"file:///home/u/My%20Files/a.txt", "https://x.org/a",
                                             "/plain/path/", "smb://server/share"}),
             QStringList({"/home/u/My Files/a.txt", "/plain/path"}));
}

void TestFileManagerService::answersCalls()
{
    FileManagerService service;
    QVERIFY(service.registerOn(bus()));
    QSignalSpy items(&service, &FileManagerService::showItems);
    QSignalSpy folders(&service, &FileManagerService::showFolders);
    QSignalSpy props(&service, &FileManagerService::showItemProperties);

    // Like a separate app calling us.
    QDBusConnection client = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "client");
    // Asynchronous, and no QDBusInterface (it introspects synchronously): the
    // service answers on this same thread, so a blocking call would wait for itself.
    auto call = [&client](const char *method, const QStringList &uris, const QString &id) {
        QDBusMessage m = QDBusMessage::createMethodCall(FileManagerService::ServiceName, FileManagerService::ObjectPath,
                                                        "org.freedesktop.FileManager1", method);
        m << uris << id;
        return client.asyncCall(m);
    };
    QDBusPendingCall a = call("ShowItems", {"file:///tmp/a%20b.txt"}, "token-1");
    QDBusPendingCall b = call("ShowFolders", {"file:///tmp", "file:///var"}, QString());
    QDBusPendingCall c = call("ShowItemProperties", {"file:///etc/hostname"}, QString());
    QTRY_VERIFY(a.isFinished() && b.isFinished() && c.isFinished());
    QVERIFY(!a.isError() && !b.isError() && !c.isError());
    QTRY_COMPARE(items.count(), 1);
    QCOMPARE(items.first().at(0).toStringList(), QStringList({"/tmp/a b.txt"}));
    QCOMPARE(items.first().at(1).toString(), QStringLiteral("token-1"));
    QTRY_COMPARE(folders.count(), 1);
    QCOMPARE(folders.first().at(0).toStringList(), QStringList({"/tmp", "/var"}));
    QTRY_COMPARE(props.count(), 1);

    service.unregister();
    QVERIFY(!bus().interface()->isServiceRegistered(FileManagerService::ServiceName).value());
    QDBusConnection::disconnectFromBus("client");
}

void TestFileManagerService::refusesWhenTaken()
{
    // Someone else (say Nautilus) has the name: we don't queue or replace.
    QDBusConnection other = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "other");
    QVERIFY(other.registerService(FileManagerService::ServiceName));
    FileManagerService service;
    QVERIFY(!service.registerOn(bus()));
    QCOMPARE(bus().interface()->serviceOwner(FileManagerService::ServiceName).value(), other.baseService());
    QDBusConnection::disconnectFromBus("other");
}

QTEST_GUILESS_MAIN(TestFileManagerService)
#include "tst_fmservice.moc"
