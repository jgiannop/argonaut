// Tests for the desktop helpers (email, wallpaper, Nautilus scripts). The
// outside programs are fake scripts that write down how they were called.

#include "DesktopActions.h"
#include "TestSupport.h"

#include <QCoreApplication>
#include <QStandardPaths>
#include <QtTest>

class TestDesktopActions : public QObject
{
    Q_OBJECT

public:
    explicit TestDesktopActions(const QString &root) : m_root(root) {}

private slots:
    void init();
    void emailAttachesEveryFile();
    void backgroundIsACopy();
    void backgroundFailureIsReported();
    void scriptsAreListedLikeNautilus();
    void scriptRunsWithNautilusVariables();

private:
    QString write(const QString &path, const QByteArray &content, bool executable = false);
    QString read(const QString &path) const;
    // A program that appends its arguments to <root>/calls.txt and exits with `exitCode`.
    QString fakeProgram(int exitCode = 0);

    QString m_root;
};

void TestDesktopActions::init()
{
    QFile::remove(m_root + "/calls.txt");
}

QString TestDesktopActions::write(const QString &path, const QByteArray &content, bool executable)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        qFatal("couldn't write %s", qPrintable(path));
    f.write(content);
    f.close();
    if (executable)
        QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return path;
}

QString TestDesktopActions::read(const QString &path) const
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

QString TestDesktopActions::fakeProgram(int exitCode)
{
    return write(m_root + "/fake", QStringLiteral("#!/bin/sh\nprintf '%s\\n' \"$@\" >> '%1/calls.txt'\nexit %2\n")
                                       .arg(m_root)
                                       .arg(exitCode)
                                       .toUtf8(),
                 true);
}

void TestDesktopActions::emailAttachesEveryFile()
{
    qputenv("ARGONAUT_EMAIL_PROGRAM", QFile::encodeName(fakeProgram()));
    QVERIFY(DesktopActions::sendByEmail({"/tmp/a b.pdf", "/tmp/c.txt"}));
    QTRY_COMPARE(read(m_root + "/calls.txt"), QStringLiteral("--attach\n/tmp/a b.pdf\n--attach\n/tmp/c.txt\n"));

    qputenv("ARGONAUT_EMAIL_PROGRAM", QFile::encodeName(m_root + "/missing"));
    QVERIFY(!DesktopActions::sendByEmail({"/tmp/c.txt"}));
}

// The desktop is pointed at a copy, so the original can be moved or deleted.
void TestDesktopActions::backgroundIsACopy()
{
    qputenv("ARGONAUT_GSETTINGS_PROGRAM", QFile::encodeName(fakeProgram()));
    const QString image = write(m_root + "/pictures/sea.jpg", "first picture");
    QString error;
    QVERIFY2(DesktopActions::setAsBackground(image, &error), qPrintable(error));

    const QString copy = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/backgrounds/sea.jpg";
    QCOMPARE(read(copy), QStringLiteral("first picture"));
    const QString calls = read(m_root + "/calls.txt");
    QVERIFY(calls.contains("picture-uri\n" + QUrl::fromLocalFile(copy).toString()));
    QVERIFY(calls.contains("picture-uri-dark\n" + QUrl::fromLocalFile(copy).toString()));
    QVERIFY(QFile::exists(image)); // the original is untouched

    // Another picture with the same name doesn't replace the first copy.
    const QString other = write(m_root + "/other/sea.jpg", "a different picture!");
    QVERIFY(DesktopActions::setAsBackground(other, &error));
    QCOMPARE(read(copy), QStringLiteral("first picture"));
    QCOMPARE(QDir(QFileInfo(copy).absolutePath()).entryList(QDir::Files).size(), 2);
}

void TestDesktopActions::backgroundFailureIsReported()
{
    qputenv("ARGONAUT_GSETTINGS_PROGRAM", QFile::encodeName(fakeProgram(1)));
    QString error;
    QVERIFY(!DesktopActions::setAsBackground(write(m_root + "/pictures/hill.jpg", "x"), &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!DesktopActions::setAsBackground(m_root + "/pictures/missing.jpg", &error));
}

void TestDesktopActions::scriptsAreListedLikeNautilus()
{
    QVERIFY(DesktopActions::scriptsFolder().endsWith("/nautilus/scripts"));
    const QString dir = m_root + "/scripts";
    write(dir + "/b-resize", "#!/bin/sh\n", true);
    write(dir + "/a-notes.txt", "not a script"); // not executable: not offered
    write(dir + "/Images/rotate", "#!/bin/sh\n", true);
    QDir().mkpath(dir + "/Empty"); // a folder without scripts isn't shown
    write(dir + "/1/2/3/4/too-deep", "#!/bin/sh\n", true);

    const QList<DesktopActions::Script> scripts = DesktopActions::scripts(dir);
    QStringList names;
    for (const DesktopActions::Script &s : scripts)
        names << s.name + (s.isFolder() ? "/" : "");
    QCOMPARE(names, (QStringList{"Images/", "b-resize"})); // folders first; "1" holds nothing within three levels
    QCOMPARE(scripts.first().children.size(), 1);
    QCOMPARE(scripts.first().children.first().path, dir + "/Images/rotate");
    QVERIFY(DesktopActions::scripts(m_root + "/no-such-folder").isEmpty());
}

void TestDesktopActions::scriptRunsWithNautilusVariables()
{
    const QString folder = m_root + "/work";
    const QString inside = write(folder + "/a b.txt", "x");
    const QString outside = write(m_root + "/elsewhere/c.txt", "x");
    const QString out = m_root + "/script-out.txt";
    const QString script = write(m_root + "/scripts/show",
                                 QStringLiteral("#!/bin/sh\n{ pwd; printf '%s\\n' \"$@\"; printf '%s' \"$NAUTILUS_SCRIPT_SELECTED_FILE_PATHS\";"
                                                " echo \"$NAUTILUS_SCRIPT_CURRENT_URI\"; echo \"$NAUTILUS_SCRIPT_WINDOW_GEOMETRY\"; } > '%1'\n")
                                     .arg(out)
                                     .toUtf8(),
                                 true);
    QVERIFY(DesktopActions::runScript(script, folder, {inside, outside}, QRect(10, 20, 800, 600)));
    QTRY_VERIFY(read(out).endsWith("800x600+10+20\n"));
    const QStringList lines = read(out).split('\n');
    QCOMPARE(QDir(lines.value(0)), QDir(folder));               // runs in the open folder
    QCOMPARE(lines.value(1), QStringLiteral("a b.txt"));         // an item of that folder: its name
    QCOMPARE(lines.value(2), outside);                           // another folder's: the full path
    QCOMPARE(lines.value(3), inside);                            // the variable has full paths
    QCOMPARE(lines.value(4), outside);
    QCOMPARE(lines.value(5), QUrl::fromLocalFile(folder).toString());

    QVERIFY(!DesktopActions::runScript(m_root + "/scripts/missing", folder, {}, QRect()));
}

int main(int argc, char **argv)
{
    TestSupport::Sandbox sandbox; // before the application object
    if (!sandbox.isValid())
        return 1;
    QCoreApplication app(argc, argv);
    TestDesktopActions test(sandbox.path());
    return QTest::qExec(&test, argc, argv);
}

#include "tst_desktopactions.moc"
