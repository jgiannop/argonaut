// Tests for DefaultFileManager: making Kosmos the file manager and giving
// folders back, with a fake `gio` (KOSMOS_GIO_PROGRAM) that keeps the
// default in a file. HOME and XDG_* are sandboxed, so the real mimeapps.list
// and D-Bus services are never touched.

#include "DefaultFileManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

class TestDefaultFileManager : public QObject
{
    Q_OBJECT

public:
    explicit TestDefaultFileManager(QString sandbox) : m_sandbox(std::move(sandbox)) {}

private slots:
    void init();
    void notInstalledRefuses();
    void makeDefaultAndRestore();
    void restoresThePreviousOne();
    void leavesOthersServiceFileAlone();

private:
    void install();
    QString state() const;
    void writeFile(const QString &path, const QByteArray &data);
    QString m_sandbox;
    QString m_apps;
};

void TestDefaultFileManager::writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
}

void TestDefaultFileManager::init()
{
    m_apps = m_sandbox + "/data/applications";
    QDir(m_sandbox + "/data").removeRecursively();
    QDir(m_sandbox + "/config").removeRecursively();
    QFile::remove(m_sandbox + "/default");
    // Fake gio: `gio mime TYPE` prints the default, `gio mime TYPE APP` sets it.
    writeFile(m_sandbox + "/gio.sh",
              QStringLiteral("#!/bin/sh\nS=%1/default\n"
                             "if [ $# -eq 2 ]; then d=$(cat $S 2>/dev/null || echo org.gnome.Nautilus.desktop)\n"
                             "  printf 'Default application for \"%s\": %s\\nRegistered applications:\\n\\t%s\\n' \"$2\" \"$d\" \"$d\"\n"
                             "elif [ $# -eq 3 ]; then echo \"$3\" > $S; fi\n").arg(m_sandbox).toUtf8());
    QFile::setPermissions(m_sandbox + "/gio.sh", QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    qputenv("KOSMOS_GIO_PROGRAM", QFile::encodeName(m_sandbox + "/gio.sh"));
    writeFile(m_apps + "/org.gnome.Nautilus.desktop", "[Desktop Entry]\nName=Files\nExec=nautilus %U\n");
    writeFile(m_apps + "/thunar.desktop", "[Desktop Entry]\nName=Thunar\nExec=thunar %U\n");
}

void TestDefaultFileManager::install()
{
    // What `cmake --install` puts there (the binary just has to be executable).
    writeFile(m_sandbox + "/bin/kosmos", "#!/bin/sh\n");
    QFile::setPermissions(m_sandbox + "/bin/kosmos", QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    writeFile(m_apps + "/kosmos.desktop",
              QStringLiteral("[Desktop Entry]\nName=Kosmos\nExec=%1/bin/kosmos %U\n"
                             "[Desktop Action new-window]\nExec=/wrong\n").arg(m_sandbox).toUtf8());
}

QString TestDefaultFileManager::state() const
{
    QFile f(m_sandbox + "/default");
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()).trimmed()
                                       : QStringLiteral("org.gnome.Nautilus.desktop");
}

void TestDefaultFileManager::notInstalledRefuses()
{
    QString error;
    QVERIFY(DefaultFileManager::installedDesktopFile().isEmpty());
    QVERIFY(!DefaultFileManager::makeDefault(&error));
    QVERIFY(error.contains("isn't installed"));
    QCOMPARE(state(), QStringLiteral("org.gnome.Nautilus.desktop"));
    QVERIFY(!QFile::exists(DefaultFileManager::serviceFilePath()));
}

void TestDefaultFileManager::makeDefaultAndRestore()
{
    install();
    QCOMPARE(DefaultFileManager::installedExecutable(), m_sandbox + "/bin/kosmos");
    QVERIFY(!DefaultFileManager::isKosmosDefault());
    QVERIFY(DefaultFileManager::serviceFilePath().startsWith(m_sandbox));

    QString error;
    QVERIFY2(DefaultFileManager::makeDefault(&error), qPrintable(error));
    QCOMPARE(state(), QStringLiteral("kosmos.desktop"));
    QVERIFY(DefaultFileManager::isKosmosDefault());
    QFile service(DefaultFileManager::serviceFilePath());
    QVERIFY(service.open(QIODevice::ReadOnly));
    const QByteArray text = service.readAll();
    QVERIFY(text.contains("Name=org.freedesktop.FileManager1"));
    QVERIFY(text.contains("Exec=" + QFile::encodeName(m_sandbox) + "/bin/kosmos --service"));
    QVERIFY(DefaultFileManager::serviceFileIsOurs());

    QString restored;
    QVERIFY2(DefaultFileManager::restorePrevious(&error, &restored), qPrintable(error));
    QCOMPARE(restored, QStringLiteral("org.gnome.Nautilus.desktop"));
    QCOMPARE(state(), QStringLiteral("org.gnome.Nautilus.desktop"));
    QVERIFY(!QFile::exists(DefaultFileManager::serviceFilePath()));
    QVERIFY(!DefaultFileManager::isKosmosDefault());
}

void TestDefaultFileManager::restoresThePreviousOne()
{
    install();
    writeFile(m_sandbox + "/default", "thunar.desktop\n"); // the user had Thunar
    QVERIFY(DefaultFileManager::makeDefault());
    QVERIFY(DefaultFileManager::makeDefault()); // twice: still remembers Thunar, not us
    QString restored;
    QVERIFY(DefaultFileManager::restorePrevious(nullptr, &restored));
    QCOMPARE(restored, QStringLiteral("thunar.desktop"));
    QCOMPARE(state(), QStringLiteral("thunar.desktop"));
    // Thunar uninstalled meanwhile: Nautilus.
    QVERIFY(DefaultFileManager::makeDefault());
    QFile::remove(m_apps + "/thunar.desktop");
    QVERIFY(DefaultFileManager::restorePrevious(nullptr, &restored));
    QCOMPARE(restored, QStringLiteral("org.gnome.Nautilus.desktop"));
}

void TestDefaultFileManager::leavesOthersServiceFileAlone()
{
    install();
    const QByteArray theirs = "[D-BUS Service]\nName=org.freedesktop.FileManager1\nExec=/usr/bin/other-fm\n";
    writeFile(DefaultFileManager::serviceFilePath(), theirs);
    QString error;
    QVERIFY(!DefaultFileManager::makeDefault(&error)); // never overwrites it
    QVERIFY(error.contains("Another program"));
    QCOMPARE(state(), QStringLiteral("org.gnome.Nautilus.desktop"));
    QVERIFY(DefaultFileManager::restorePrevious(&error)); // and never removes it
    QFile f(DefaultFileManager::serviceFilePath());
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), theirs);
}

int main(int argc, char **argv)
{
    QTemporaryDir sandbox;
    qputenv("HOME", QFile::encodeName(sandbox.path() + "/home"));
    qputenv("XDG_DATA_HOME", QFile::encodeName(sandbox.path() + "/data"));
    qputenv("XDG_DATA_DIRS", QFile::encodeName(sandbox.path() + "/nothing")); // not /usr/share
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(sandbox.path() + "/config"));
    qputenv("XDG_CACHE_HOME", QFile::encodeName(sandbox.path() + "/cache"));
    QCoreApplication app(argc, argv);
    app.setOrganizationName("KosmosTest");
    TestDefaultFileManager test(sandbox.path());
    return QTest::qExec(&test, argc, argv);
}

#include "tst_defaultfm.moc"
