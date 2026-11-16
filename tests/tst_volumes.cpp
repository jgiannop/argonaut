// Drives that aren't mounted (src/Volumes.h): what `gio mount -li` says is
// read right, they are rows of the places, and choosing one mounts it. No
// real drive is listed or mounted here: `gio` is a script of the test's.

#include "Places.h"
#include "TestSupport.h"
#include "Volumes.h"

#include <QSignalSpy>
#include <QtTest>

class TestVolumes : public QObject
{
    Q_OBJECT

public:
    explicit TestVolumes(const QString &root)
        : m_root(root)
    {
    }

private slots:
    void readsWhatGioSays();
    void addressesAreDeviceNamesOnly();
    void offInTests();
    void listedAndMounted();

private:
    QString m_root;
};

// As gio prints it: two disks with a volume each (one mounted), a stick,
// an empty optical drive, a volume on no drive, a mount with no volume.
static const char kListing[] = R"(Drive(0): Samsung SSD 870 EVO 1TB
  Type: GProxyDrive (GProxyVolumeMonitorUDisks2)
  ids:
   unix-device: '/dev/sda'
  is_removable=0
  is_media_removable=0
  can_eject=0
Drive(1): Hitachi HDS721010DLE630
  Type: GProxyDrive (GProxyVolumeMonitorUDisks2)
  ids:
   unix-device: '/dev/sdb'
  is_removable=0
  Volume(0): HITACHI
    Type: GProxyVolume (GProxyVolumeMonitorUDisks2)
    ids:
     class: 'device'
     unix-device: '/dev/sdb1'
     uuid: 'c9d2ce79-0ef1-4ab4-93c6-ded06c79ee79'
     label: 'HITACHI'
    uuid=c9d2ce79-0ef1-4ab4-93c6-ded06c79ee79
    can_mount=1
    can_eject=0
    should_automount=0
Drive(2): WDC WD20EZRX
  ids:
   unix-device: '/dev/sdc'
  is_removable=0
  Volume(0): WESTERN DIGITAL
    ids:
     unix-device: '/dev/sdc1'
     uuid: 'cef9cc25'
    can_mount=1
    Mount(0): WESTERN DIGITAL -> file:///media/ann/WESTERN%20DIGITAL
      Type: GProxyMount (GProxyVolumeMonitorUDisks2)
      default_location=file:///media/ann/WESTERN%20DIGITAL
      can_unmount=1
  Volume(1): 500 GB Volume
    ids:
     unix-device: '/dev/sdc2'
    can_mount=1
Drive(3): Kingston DataTraveler
  ids:
   unix-device: '/dev/sdd'
  is_removable=1
  is_media_removable=1
  Volume(0): KINGSTON
    ids:
     unix-device: '/dev/sdd1'
    can_mount=1
  Volume(1): locked
    ids:
     unix-device: '/dev/sdd2'
    can_mount=0
  Volume(2): odd
    ids:
     unix-device: '/dev/sd d3; rm -rf'
    can_mount=1
Drive(4): TSSTcorp CDDVDW SH-224BB
  ids:
   unix-device: '/dev/sr0'
  is_removable=1
Volume(0): Phone
  ids:
   unix-device: '/dev/bus/usb/001/004'
  can_mount=1
Mount(0): nas -> sftp://nas/
  Type: GDaemonMount
)";

void TestVolumes::readsWhatGioSays()
{
    const QList<Volumes::Volume> volumes = Volumes::parse(kListing);
    QStringList said;
    for (const Volumes::Volume &v : volumes)
        said << QStringLiteral("%1|%2|%3|%4").arg(v.name, v.device, v.uuid, v.removable ? "removable" : "fixed");
    // Not the mounted one, not the one that can't be mounted, not the one
    // whose device isn't a plain name; a volume on no drive counts.
    QCOMPARE(said, (QStringList{"HITACHI|/dev/sdb1|c9d2ce79-0ef1-4ab4-93c6-ded06c79ee79|fixed", "500 GB Volume|/dev/sdc2||fixed",
                                "KINGSTON|/dev/sdd1||removable", "Phone|/dev/bus/usb/001/004||fixed"}));
    QVERIFY(Volumes::parse("").isEmpty());
    QVERIFY(Volumes::parse("this is not what gio prints\nVolume(x): no\n").isEmpty());
}

void TestVolumes::addressesAreDeviceNamesOnly()
{
    QCOMPARE(Volumes::pathFor("/dev/sdb1"), QStringLiteral("volume:///dev/sdb1"));
    QVERIFY(Volumes::isVolumePath("volume:///dev/sdb1"));
    QVERIFY(!Volumes::isVolumePath("/dev/sdb1"));
    QVERIFY(Places::isAddress("volume:///dev/sdb1"));
    QVERIFY(!Places::isAddress("/media/ann/HITACHI"));
    QCOMPARE(Volumes::deviceOf("volume:///dev/sdb1"), QStringLiteral("/dev/sdb1"));
    QCOMPARE(Volumes::deviceOf("volume:///dev/disk/by-uuid/c9d2-ce79"), QStringLiteral("/dev/disk/by-uuid/c9d2-ce79"));
    for (const char *bad : {"volume:///etc/passwd", "volume:///dev/../etc/passwd", "volume:///dev/sdb1 -o exec", "volume://--help", "volume:///dev/",
                            "volume:///dev/sdb1;reboot", "/dev/sdb1"})
        QVERIFY2(Volumes::deviceOf(bad).isEmpty(), bad);
}

// A test never sees the drives of the computer it runs on.
void TestVolumes::offInTests()
{
    qunsetenv("ARGONAUT_VOLUMES");
    QVERIFY(qEnvironmentVariableIsSet("ARGONAUT_TRASH_HOME_ONLY"));
    QVERIFY(!Volumes::isActive());
    QVERIFY(Volumes::instance()->unmounted().isEmpty());
    for (const Places::Entry &entry : Places::list())
        QVERIFY2(!Volumes::isVolumePath(entry.path), qPrintable(entry.name));
    QSignalSpy failed(Volumes::instance(), &Volumes::failed);
    Volumes::instance()->mount("/dev/sdb1");
    QCOMPARE(failed.size(), 1); // and mounts nothing
}

void TestVolumes::listedAndMounted()
{
    // A pretend gio: lists what kListing says, "mounts" /dev/argonaut-test-sdb1 by making
    // a folder, refuses /dev/argonaut-test-sdc2, and writes down what it was asked.
    QDir().mkpath(m_root + "/gio");
    const QString log = m_root + "/gio/asked";
    const QString mountPoint = m_root + "/gio/media/HITACHI";
    {
        QFile listing(m_root + "/gio/listing");
        QVERIFY(listing.open(QIODevice::WriteOnly));
        listing.write(QByteArray(kListing).replace("/dev/sd", "/dev/argonaut-test-sd").replace("/dev/bus/usb", "/dev/argonaut-test-usb")); // no real machine has these
        QFile script(m_root + "/gio/gio");
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(QStringLiteral(R"(#!/bin/sh
echo "$@" >> '%1'
if [ "$1 $2" = "mount -li" ]; then cat '%2/gio/listing'; exit 0; fi
if [ "$1 $2 $3" = "mount -d /dev/argonaut-test-sdb1" ]; then mkdir -p '%3'; echo "Mounted /dev/argonaut-test-sdb1 at %3"; exit 0; fi
echo "gio: $3: Not authorized to perform operation" >&2
exit 2
)").arg(log, m_root, mountPoint).toUtf8());
        script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
    qputenv("ARGONAUT_GIO_PROGRAM", QFile::encodeName(m_root + "/gio/gio"));
    qputenv("ARGONAUT_VOLUMES", "1");
    const auto restore = qScopeGuard([] {
        qunsetenv("ARGONAUT_GIO_PROGRAM");
        qunsetenv("ARGONAUT_VOLUMES");
    });
    QVERIFY(Volumes::isActive());
    auto rows = [] {
        QStringList names;
        for (const Places::Entry &entry : Places::list()) {
            if (Volumes::isVolumePath(entry.path))
                names << entry.name + "=" + entry.path;
        }
        return names;
    };
    // Asked for in the background: nothing at first, then they are rows of
    // the places, after the mounted drives, and the list says it changed.
    QSignalSpy changed(Volumes::instance(), &Volumes::changed);
    rows();
    QTRY_VERIFY_WITH_TIMEOUT(changed.size() >= 1, 10000);
    QCOMPARE(rows(), (QStringList{"HITACHI=volume:///dev/argonaut-test-sdb1", "500 GB Volume=volume:///dev/argonaut-test-sdc2", "KINGSTON=volume:///dev/argonaut-test-sdd1",
                                  "Phone=volume:///dev/argonaut-test-usb/001/004"}));
    for (const Places::Entry &entry : Places::list()) {
        if (entry.path != "volume:///dev/argonaut-test-sdb1")
            continue;
        QCOMPARE(entry.group, Places::Group::Drives);
        QCOMPARE(entry.icon, Places::Icon::Drive);
        QVERIFY(!entry.eject);
        QVERIFY2(entry.tooltip.contains("Not mounted") && entry.tooltip.contains("/dev/argonaut-test-sdb1"), qPrintable(entry.tooltip));
    }
    QCOMPARE(Volumes::instance()->nameOf("/dev/argonaut-test-sdd1"), QStringLiteral("KINGSTON"));

    // Choosing one mounts it: the folder it got is told.
    QSignalSpy mounted(Volumes::instance(), &Volumes::mounted);
    QSignalSpy failed(Volumes::instance(), &Volumes::failed);
    Volumes::instance()->mount("/dev/argonaut-test-sdb1");
    Volumes::instance()->mount("/dev/argonaut-test-sdb1"); // twice is once
    QTRY_COMPARE_WITH_TIMEOUT(mounted.size(), 1, 10000);
    QCOMPARE(mounted.first().at(0).toString(), QStringLiteral("/dev/argonaut-test-sdb1"));
    QCOMPARE(mounted.first().at(1).toString(), mountPoint);
    QVERIFY(QFileInfo(mountPoint).isDir());
    QCOMPARE(failed.size(), 0);
    // One the desktop refuses: said in its words, without gio's own prefix.
    Volumes::instance()->mount("/dev/argonaut-test-sdc2");
    QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 10000);
    QCOMPARE(failed.first().at(1).toString(), QStringLiteral("Not authorized to perform operation"));
    QCOMPARE(mounted.size(), 1);
    // Not a device: gio is never asked.
    Volumes::instance()->mount("/dev/argonaut-test-sdb1 -o exec");
    QCOMPARE(failed.size(), 2);

    // gio was only ever asked to list, and to mount those two by name.
    QTest::qWait(300);
    QFile asked(log);
    QVERIFY(asked.open(QIODevice::ReadOnly));
    QStringList questions = QString::fromUtf8(asked.readAll()).split('\n', Qt::SkipEmptyParts);
    QCOMPARE(int(questions.count("mount -d /dev/argonaut-test-sdb1")), 1);
    QCOMPARE(int(questions.count("mount -d /dev/argonaut-test-sdc2")), 1);
    questions.removeAll("mount -li");
    questions.removeAll("mount -d /dev/argonaut-test-sdb1");
    questions.removeAll("mount -d /dev/argonaut-test-sdc2");
    QVERIFY2(questions.isEmpty(), qPrintable(questions.join('|')));
}

int main(int argc, char **argv)
{
    TestSupport::Sandbox sandbox; // before the application object
    if (!sandbox.isValid())
        return 1;
    QCoreApplication app(argc, argv);
    TestVolumes test(sandbox.path());
    return QTest::qExec(&test, argc, argv);
}

#include "tst_volumes.moc"
