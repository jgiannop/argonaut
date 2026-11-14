// Tests for remote locations: addresses, GVfs mount folder names, and the
// Mounter driving a fake `gio mount` (ARGONAUT_GIO_PROGRAM) through its
// prompts the way the real one asks them (checked against gvfs 1.54).

#include "Remote.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

class TestRemote : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void addresses();
    void mountFolderNames_data();
    void mountFolderNames();
    void listsMounts();
    void loginWithRetry();
    void passwordOnly();
    void hostKeyQuestion();
    void failureMessage();
    void alreadyMounted();
    void anonymous();
    void cancel();
    void unmountRuns();

private:
    void fakeGio(const QString &mountScript);

    QTemporaryDir m_tmp;
    QString m_bin;
    QString m_log;
};

void TestRemote::initTestCase()
{
    QVERIFY(m_tmp.isValid());
    m_bin = m_tmp.path() + "/gio";
    m_log = m_tmp.path() + "/log.txt";
}

// `gio info` always answers with a local path; `gio mount` runs the script.
void TestRemote::fakeGio(const QString &mountScript)
{
    QFile f(m_bin);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(QString("#!/bin/sh\n"
                    "echo \"$@\" >> '%1'\n"
                    "if [ \"$1\" = info ]; then echo 'display name: x'; echo 'local path: /run/user/1000/gvfs/sftp:host=example.com'; exit 0; fi\n"
                    "if [ \"$1\" = mount ] && [ \"$2\" = -u ]; then exit 0; fi\n"
                    "%2\n")
                .arg(m_log, mountScript)
                .toUtf8());
    f.close();
    QFile::setPermissions(m_bin, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    QFile::remove(m_log);
    qputenv("ARGONAUT_GIO_PROGRAM", QFile::encodeName(m_bin));
}

void TestRemote::addresses()
{
    QVERIFY(Remote::isRemoteAddress("sftp://example.com"));
    QVERIFY(Remote::isRemoteAddress("SMB://nas/photos"));
    QVERIFY(Remote::isRemoteAddress("  ftp://x/"));
    QVERIFY(Remote::isRemoteAddress("davs://cloud.example.com/remote.php/webdav"));
    QVERIFY(!Remote::isRemoteAddress("/home/me"));
    QVERIFY(!Remote::isRemoteAddress("file:///home"));
    QVERIFY(!Remote::isRemoteAddress("https://example.com"));
}

void TestRemote::mountFolderNames_data()
{
    QTest::addColumn<QString>("dir");
    QTest::addColumn<QString>("name");
    QTest::addColumn<QString>("address");
    QTest::newRow("sftp") << "sftp:host=example.com,user=john" << "john@example.com" << "sftp://john@example.com/";
    QTest::newRow("sftp port") << "sftp:host=10.0.0.2,port=2222" << "10.0.0.2" << "sftp://10.0.0.2:2222/";
    QTest::newRow("smb") << "smb-share:server=nas,share=photos" << "photos on nas" << "smb://nas/photos/";
    QTest::newRow("smb space") << "smb-share:server=nas,share=my%20files,user=me" << "my files on nas"
                               << "smb://me@nas/my%20files/";
    QTest::newRow("smb domain") << "smb-share:domain=WORK,server=nas,share=x,user=me" << "x on nas"
                                << "smb://WORK%3Bme@nas/x/";
    QTest::newRow("ftp") << "ftp:host=ftp.example.org" << "ftp.example.org" << "ftp://ftp.example.org/";
    QTest::newRow("davs") << "dav:host=cloud.example.com,ssl=true,prefix=%2Fremote.php%2Fwebdav"
                          << "cloud.example.com/remote.php/webdav" << "davs://cloud.example.com/remote.php/webdav/";
    QTest::newRow("phone") << "mtp:host=Google_Pixel_7_1A2B3C4D" << "Google Pixel 7" << "mtp://Google_Pixel_7_1A2B3C4D/";
    QTest::newRow("ipv6") << "sftp:host=fe80::1" << "fe80::1" << "sftp://[fe80::1]/";
}

void TestRemote::mountFolderNames()
{
    QFETCH(QString, dir);
    QFETCH(QString, name);
    QFETCH(QString, address);
    Remote::Mount m;
    QVERIFY(Remote::parseMountDir(dir, &m));
    QCOMPARE(m.name, name);
    QCOMPARE(m.address, address);
    QVERIFY(!Remote::parseMountDir("not-a-mount", &m));
    QVERIFY(!Remote::parseMountDir("sftp:user=x", &m)); // no host
}

void TestRemote::listsMounts()
{
    const QString root = m_tmp.path() + "/gvfs";
    QDir().mkpath(root + "/sftp:host=example.com,user=john/home/john");
    QDir().mkpath(root + "/smb-share:server=nas,share=photos");
    QDir().mkpath(root + "/weird");
    qputenv("ARGONAUT_GVFS_ROOT", QFile::encodeName(root));
    const QList<Remote::Mount> list = Remote::mounts();
    QCOMPARE(list.size(), 2);
    QCOMPARE(list[0].localPath, root + "/sftp:host=example.com,user=john");
    const Remote::Mount m = Remote::mountOf(root + "/sftp:host=example.com,user=john/home/john");
    QCOMPARE(m.name, QStringLiteral("john@example.com"));
    QCOMPARE(m.localPath, root + "/sftp:host=example.com,user=john");
    QVERIFY(Remote::mountOf("/home/john").localPath.isEmpty());
    qunsetenv("ARGONAUT_GVFS_ROOT");
}

void TestRemote::loginWithRetry()
{
    // Like gvfs's smb backend: a heading, then User/Domain/Password, and
    // everything again after a wrong password.
    fakeGio(R"(
while true; do
  printf 'Authentication Required\nEnter user and password for share \342\200\234photos\342\200\235 on \342\200\234nas\342\200\235:\n'
  printf 'User [me]: '; read u || exit 2
  printf 'Domain [WORKGROUP]: '; read d || exit 2
  printf 'Password: '; read p || exit 2
  echo "$u|$d|$p" >> ")" + m_log + R"("
  [ "$p" = right ] && exit 0
done)");
    Remote::Mounter mounter("smb://nas/photos");
    QSignalSpy login(&mounter, &Remote::Mounter::needLogin);
    QSignalSpy done(&mounter, &Remote::Mounter::finished);
    mounter.start();
    QVERIFY(login.wait(5000));
    QVERIFY(login.last().at(0).toString().contains("photos")); // the message
    QCOMPARE(login.last().at(1).toBool(), true);               // asks for the user
    QCOMPARE(login.last().at(2).toString(), QStringLiteral("me"));
    QCOMPARE(login.last().at(3).toBool(), true);               // smb: domain
    QCOMPARE(login.last().at(5).toBool(), false);              // not a retry
    mounter.answer({"alice", "HOME", "wrong"});
    QVERIFY(login.wait(5000));
    QCOMPARE(login.last().at(5).toBool(), true); // asked again: wrong password
    mounter.answer({"alice", "HOME", "right"});
    QVERIFY(done.wait(5000));
    QCOMPARE(done.last().at(0).toBool(), true);
    QCOMPARE(done.last().at(1).toString(), QStringLiteral("/run/user/1000/gvfs/sftp:host=example.com"));
    QFile log(m_log);
    QVERIFY(log.open(QIODevice::ReadOnly));
    const QString text = QString::fromUtf8(log.readAll());
    QVERIFY(text.contains("alice|HOME|wrong"));
    QVERIFY(text.contains("alice|HOME|right"));
    QVERIFY(text.contains("info smb://nas/photos"));
}

void TestRemote::passwordOnly()
{
    // sftp://user@host: only a password.
    fakeGio(R"(printf 'Enter password for ssh as john on example.com\nPassword: '; read p || exit 2
[ "$p" = secret ] && exit 0; exit 2)");
    Remote::Mounter mounter("sftp://john@example.com");
    QSignalSpy login(&mounter, &Remote::Mounter::needLogin);
    QSignalSpy done(&mounter, &Remote::Mounter::finished);
    mounter.start();
    QVERIFY(login.wait(5000));
    QCOMPARE(login.last().at(1).toBool(), false); // no user field
    QCOMPARE(login.last().at(3).toBool(), false); // no domain for sftp
    mounter.answer({QString(), QString(), "secret"});
    QVERIFY(done.wait(5000));
    QCOMPARE(done.last().at(0).toBool(), true);
}

void TestRemote::hostKeyQuestion()
{
    fakeGio(R"(printf 'Can\342\200\231t verify the identity of \342\200\234example.com\342\200\235.\nThe fingerprint is SHA256:abc.\n[1] Log In Anyway\n[2] Cancel Login\nChoice: '
read c || exit 2; [ "$c" = 1 ] && exit 0; exit 2)");
    Remote::Mounter mounter("sftp://example.com");
    QSignalSpy question(&mounter, &Remote::Mounter::question);
    QSignalSpy done(&mounter, &Remote::Mounter::finished);
    mounter.start();
    QVERIFY(question.wait(5000));
    QVERIFY(question.last().at(0).toString().contains("fingerprint"));
    QCOMPARE(question.last().at(1).toStringList(), QStringList({"Log In Anyway", "Cancel Login"}));
    QCOMPARE(question.last().at(2).value<QList<int>>(), QList<int>({1, 2}));
    mounter.choose(1);
    QVERIFY(done.wait(5000));
    QCOMPARE(done.last().at(0).toBool(), true);
}

void TestRemote::failureMessage()
{
    fakeGio("echo 'gio: sftp://nowhere/: Error resolving “nowhere”: Name or service not known' >&2; exit 2");
    Remote::Mounter mounter("sftp://nowhere");
    QSignalSpy done(&mounter, &Remote::Mounter::finished);
    mounter.start();
    QVERIFY(done.wait(5000));
    QCOMPARE(done.last().at(0).toBool(), false);
    QCOMPARE(done.last().at(2).toString(), QStringLiteral("Error resolving “nowhere”: Name or service not known"));
}

void TestRemote::alreadyMounted()
{
    fakeGio("echo 'gio: sftp://example.com/: Location is already mounted' >&2; exit 2");
    Remote::Mounter mounter("sftp://example.com");
    QSignalSpy done(&mounter, &Remote::Mounter::finished);
    mounter.start();
    QVERIFY(done.wait(5000));
    QCOMPARE(done.last().at(0).toBool(), true); // fine: just open it
}

void TestRemote::anonymous()
{
    fakeGio("[ \"$2\" = --anonymous ] && exit 0; exit 2");
    Remote::Mounter mounter("ftp://ftp.example.org");
    QSignalSpy done(&mounter, &Remote::Mounter::finished);
    mounter.start(true);
    QVERIFY(done.wait(5000));
    QCOMPARE(done.last().at(0).toBool(), true);
}

void TestRemote::cancel()
{
    fakeGio("printf 'Password: '; read p; sleep 30");
    Remote::Mounter mounter("sftp://john@example.com");
    QSignalSpy login(&mounter, &Remote::Mounter::needLogin);
    QSignalSpy done(&mounter, &Remote::Mounter::finished);
    mounter.start();
    QVERIFY(login.wait(5000));
    mounter.cancel();
    QCOMPARE(done.size(), 1);
    QCOMPARE(done.last().at(0).toBool(), false);
    QVERIFY(done.last().at(2).toString().isEmpty()); // no error to show
}

void TestRemote::unmountRuns()
{
    fakeGio("exit 0");
    bool finished = false;
    bool ok = false;
    QObject context;
    Remote::unmount("sftp://example.com/", &context, [&](bool success, const QString &) {
        finished = true;
        ok = success;
    });
    QTRY_VERIFY_WITH_TIMEOUT(finished, 5000);
    QVERIFY(ok);
    QFile log(m_log);
    QVERIFY(log.open(QIODevice::ReadOnly));
    QVERIFY(QString::fromUtf8(log.readAll()).contains("mount -u sftp://example.com/"));
}

QTEST_GUILESS_MAIN(TestRemote)
#include "tst_remote.moc"
