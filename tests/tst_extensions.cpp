// Tests for extensions (Extensions::Host): finding them, the on/off rule,
// the messages, what a misbehaving one can't do, and the Git extension end
// to end through the folder model.

#include "DirectoryModel.h"
#include "Extensions.h"
#include "TestSupport.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

using Extensions::Badge;
using Extensions::Host;
using Extensions::Manifest;

namespace {

QString g_extensions; // the folder of test extensions (ARGONAUT_EXTENSIONS_PATH)

void writeFile(const QString &path, const QByteArray &content = "x\n", bool executable = false)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(content);
    f.close();
    if (executable)
        f.setPermissions(f.permissions() | QFile::ExeOwner);
}

// tests/fake_extension.py is a small extension in Python. What it does depends
// on the folder's name, so one program plays the well-behaved and the
// misbehaving extension.
bool git(const QString &dir, const QStringList &args)
{
    QProcess p;
    p.setWorkingDirectory(dir);
    p.start(QStringLiteral("git"), QStringList{"-c", "user.name=Test", "-c", "user.email=t@example.com",
                                               "-c", "init.defaultBranch=main", "-c", "commit.gpgsign=false"}
                                       + args);
    p.waitForFinished(20000);
    return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

} // namespace

class TestExtensions : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void manifests();
    void offUntilSwitchedOn();
    void badgesAndInfoAreCheckedAndCut();
    void countsWatchers();
    void switchingOffTakesTheMarksAway();
    void nonsenseIsSkipped();
    void aCrashingExtensionIsGivenUp();
    void commandsFitTheSelection();
    void invokeTellsTheExtension();
    void gitThroughTheModel();
    void gitStageAndUnstage();
    void sendByEmailIsAnExtension();

private:
    int starts() const;
    QTemporaryDir *m_tmp = nullptr;
    QString m_dir;
};

int TestExtensions::starts() const
{
    QFile f(g_extensions + "/fake/starts");
    return f.open(QIODevice::ReadOnly) ? int(f.readAll().size()) : 0;
}

void TestExtensions::initTestCase()
{
    if (QStandardPaths::findExecutable("python3").isEmpty())
        QSKIP("python3 isn't installed (the test extension is a Python script)");
    writeFile(g_extensions + "/fake/extension.json",
              R"({"id": "fake", "name": "Fake", "description": "For tests", "version": "1.0",
                  "protocol": 1, "exec": ["fake.py"]})");
    QVERIFY(QFile::copy(QStringLiteral(FAKE_EXTENSION), g_extensions + "/fake/fake.py"));
    QFile::setPermissions(g_extensions + "/fake/fake.py", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}

void TestExtensions::init()
{
    m_tmp = new QTemporaryDir;
    QVERIFY(m_tmp->isValid());
    m_dir = QDir::cleanPath(m_tmp->path());
    QFile::remove(g_extensions + "/fake/starts");
    QSettings().clear();
}

void TestExtensions::cleanup()
{
    delete m_tmp;
}

void TestExtensions::manifests()
{
    const Manifest good = Extensions::readManifest(g_extensions + "/fake", false);
    QVERIFY2(good.isUsable(), qPrintable(good.problem));
    QCOMPARE(good.id, QStringLiteral("fake"));
    QCOMPARE(good.name, QStringLiteral("Fake"));
    QCOMPARE(good.program, g_extensions + "/fake/fake.py"); // relative: inside its folder

    auto problemOf = [&](const QByteArray &json) {
        writeFile(m_dir + "/x/extension.json", json);
        writeFile(m_dir + "/x/run", "#!/bin/sh\n", true);
        return Extensions::readManifest(m_dir + "/x", false).problem;
    };
    QVERIFY(problemOf(R"({"id": "x", "protocol": 1, "exec": "run"})").isEmpty());
    QVERIFY(!problemOf("not json").isEmpty());
    QVERIFY(!problemOf(R"({"id": "../x", "protocol": 1, "exec": "run"})").isEmpty()); // an id is a plain name
    QVERIFY(!problemOf(R"({"id": "x", "protocol": 99, "exec": "run"})").isEmpty());   // made for another version
    QVERIFY(!problemOf(R"({"id": "x", "protocol": 1})").isEmpty());                    // nothing to start
    QVERIFY(!problemOf(R"({"id": "x", "protocol": 1, "exec": "missing"})").isEmpty());
    QVERIFY(!Extensions::readManifest(m_dir + "/nothing", false).isUsable());
}

// An extension from the user's folder is a stranger's program: it isn't
// started until it is switched on.
void TestExtensions::offUntilSwitchedOn()
{
    Host host;
    QVERIFY(!host.isEnabled("fake"));
    host.watch(m_dir);
    QTest::qWait(300);
    QVERIFY(!host.isRunning("fake"));
    QCOMPARE(starts(), 0);
    QVERIFY(!host.badge(m_dir, m_dir + "/a.txt").valid);

    QSignalSpy changed(&host, &Host::folderChanged);
    host.setEnabled("fake", true);
    QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), 10000);
    QVERIFY(host.isRunning("fake"));
    QCOMPARE(starts(), 1);
    QVERIFY(host.badge(m_dir, m_dir + "/a.txt").valid);
}

void TestExtensions::badgesAndInfoAreCheckedAndCut()
{
    qputenv("ANTHROPIC_API_KEY", "secret");
    Host host;
    host.setEnabled("fake", true);
    QSignalSpy changed(&host, &Host::folderChanged);
    host.watch(m_dir);
    QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), 10000);
    qunsetenv("ANTHROPIC_API_KEY");

    const Badge a = host.badge(m_dir, m_dir + "/a.txt");
    QVERIFY(a.valid);
    QCOMPARE(a.text, QStringLiteral("LO")); // two characters at most
    QCOMPARE(a.color, QStringLiteral("accent"));
    QCOMPARE(a.tooltip, QStringLiteral("A mark"));
    // An unknown colour becomes the plain one; "deep" covers what is inside.
    const Badge sub = host.badge(m_dir, m_dir + "/sub");
    QVERIFY(sub.valid);
    QCOMPARE(sub.color, QStringLiteral("dim"));
    QVERIFY(host.badge(m_dir, m_dir + "/sub/deeper/file.txt").valid);
    QVERIFY(!host.badge(m_dir, m_dir + "/other.txt").valid);
    QVERIFY(!host.badge(m_dir, m_dir + "/subway").valid); // a name that only starts the same
    // Nothing outside the folder it was asked about, and no other folder.
    QVERIFY(!host.badge(m_dir, QFileInfo(m_dir).path() + "/outside.txt").valid);
    QVERIFY(!host.badge(m_dir, "/etc/passwd").valid);
    QVERIFY(!host.badge("/not/watched", "/not/watched/z").valid);
    // The path bar text is one short line, with only the icons we have.
    const Extensions::Info info = host.info(m_dir);
    QCOMPARE(info.text, QStringLiteral("fake watch second line"));
    QVERIFY(info.icon.isEmpty());
    // Our API key isn't handed to extensions.
    QCOMPARE(info.tooltip, QStringLiteral("key=none"));

    // refresh() asks again.
    changed.clear();
    host.refresh(m_dir);
    QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), 10000);
    QCOMPARE(host.info(m_dir).text, QStringLiteral("fake refresh second line"));
}

void TestExtensions::countsWatchers()
{
    Host host;
    host.setEnabled("fake", true);
    QSignalSpy changed(&host, &Host::folderChanged);
    host.watch(m_dir); // two views on one folder
    host.watch(m_dir);
    QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), 10000);
    host.unwatch(m_dir);
    QVERIFY(host.badge(m_dir, m_dir + "/a.txt").valid); // one still shows it
    host.unwatch(m_dir);
    QVERIFY(!host.badge(m_dir, m_dir + "/a.txt").valid);
    QVERIFY(host.info(m_dir).text.isEmpty());
}

void TestExtensions::switchingOffTakesTheMarksAway()
{
    Host host;
    host.setEnabled("fake", true);
    QSignalSpy changed(&host, &Host::folderChanged);
    host.watch(m_dir);
    QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), 10000);
    changed.clear();
    host.setEnabled("fake", false);
    QVERIFY(!changed.isEmpty()); // the views are told
    QVERIFY(!host.isRunning("fake"));
    QVERIFY(!host.badge(m_dir, m_dir + "/a.txt").valid);
}

void TestExtensions::nonsenseIsSkipped()
{
    const QString folder = m_dir + "/garbage";
    QDir().mkpath(folder);
    Host host;
    host.setEnabled("fake", true);
    QSignalSpy changed(&host, &Host::folderChanged);
    host.watch(folder);
    QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), 10000);
    QVERIFY(host.badge(folder, folder + "/a.txt").valid); // the good message after the bad lines
    QVERIFY(host.isRunning("fake"));
}

void TestExtensions::aCrashingExtensionIsGivenUp()
{
    const QString folder = m_dir + "/crash";
    QDir().mkpath(folder);
    Host host;
    host.setEnabled("fake", true);
    host.watch(folder);
    // It is started again a few times, then left alone; nothing else stops.
    QTRY_COMPARE_WITH_TIMEOUT(starts(), 3, 20000);
    QTest::qWait(3500);
    QCOMPARE(starts(), 3);
    QVERIFY(!host.isRunning("fake"));
    QVERIFY(!host.badge(folder, folder + "/a.txt").valid);
}

// Which commands a right-click offers: by what is selected (files, folders,
// one or many, their types), by folder, and only well-formed ones.
void TestExtensions::commandsFitTheSelection()
{
    Host host;
    host.setEnabled("fake", true);
    QSignalSpy changed(&host, &Host::folderChanged);
    host.watch(m_dir);
    QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), 10000);
    auto ids = [&](const QString &folder, const QList<Extensions::Item> &selection) {
        QStringList out;
        for (const Extensions::Command &c : host.commands(folder, selection))
            out << c.id;
        return out;
    };
    const Extensions::Item text{m_dir + "/a.txt", "text/plain", false};
    const Extensions::Item photo{m_dir + "/p.jpg", "image/jpeg", false};
    const Extensions::Item dir{m_dir + "/sub", "inode/directory", true};
    QCOMPARE(ids(m_dir, {text}), (QStringList{"hello", "ask", "only-a"}));
    QCOMPARE(ids(m_dir, {photo}), (QStringList{"hello", "one-image", "ask"}));
    QCOMPARE(ids(m_dir, {photo, photo}), (QStringList{"hello", "ask"})); // "one image" means one
    QCOMPARE(ids(m_dir, {dir}), (QStringList{"hello", "folders", "ask"}));
    QCOMPARE(ids(m_dir, {dir, text}), (QStringList{"hello", "ask"}));
    QCOMPARE(ids(m_dir, {}), (QStringList{"here"}));                    // the folder's empty space
    QCOMPARE(ids(QString(), {text}), (QStringList{"hello", "ask"}));    // Recent: no folder, no folder commands
    QVERIFY(ids(QString(), {}).isEmpty());
    // A command limited to items can't reach outside its folder.
    const Extensions::Item outside{QFileInfo(m_dir).path() + "/outside.txt", "text/plain", false};
    QVERIFY(!ids(m_dir, {outside}).contains("only-a"));
    // The question a command wants asked comes along.
    for (const Extensions::Command &c : host.commands(m_dir, {text})) {
        if (c.id == "ask")
            QCOMPARE(c.confirm, QStringLiteral("Really do the risky thing?"));
    }
    // Switched off: nothing is offered.
    host.setEnabled("fake", false);
    QVERIFY(ids(m_dir, {text}).isEmpty());
}

void TestExtensions::invokeTellsTheExtension()
{
    Host host;
    host.setEnabled("fake", true);
    QSignalSpy changed(&host, &Host::folderChanged);
    host.watch(m_dir);
    QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), 10000);
    QSignalSpy said(&host, &Host::message);
    const QList<Extensions::Command> commands = host.commands(m_dir, {{m_dir + "/a.txt", "text/plain", false}});
    QVERIFY(!commands.isEmpty());
    host.invoke(commands.first(), m_dir, {m_dir + "/a.txt", m_dir + "/b.txt"});
    QTRY_VERIFY_WITH_TIMEOUT(!said.isEmpty(), 10000);
    QCOMPARE(said.first().at(0).toString(), QStringLiteral("Fake")); // who says it
    QCOMPARE(said.first().at(1).toString(), QStringLiteral("invoked hello on 2: a.txt,b.txt"));
}

// The Git extension, as the app uses it: the model asks the shared host, the
// extension's program runs git and its marks arrive as the model's BadgeRole.
void TestExtensions::gitThroughTheModel()
{
    if (QStandardPaths::findExecutable("git").isEmpty())
        QSKIP("git isn't installed");
    const QString repo = m_dir + "/repo";
    QDir().mkpath(repo);
    QVERIFY(git(repo, {"init", "-q"}));
    writeFile(repo + "/tracked.txt", "one\n");
    writeFile(repo + "/src/main.cpp", "int main() {}\n");
    QVERIFY(git(repo, {"add", "."}));
    QVERIFY(git(repo, {"commit", "-q", "-m", "first"}));
    writeFile(repo + "/tracked.txt", "two\n");

    Host *host = Host::instance();
    host->rescan();
    QVERIFY2(!host->isEnabled("git"), "test extensions start switched off");
    host->setEnabled("git", true);

    auto badgeOf = [](const DirectoryModel &model, const QString &path) {
        return model.indexForPath(path).data(DirectoryModel::BadgeRole).value<Badge>();
    };
    {
        DirectoryModel model;
        QSignalSpy loaded(&model, &DirectoryModel::loaded);
        model.setPath(repo);
        QVERIFY(loaded.wait(10000));
        QTRY_COMPARE_WITH_TIMEOUT(badgeOf(model, repo + "/tracked.txt").text, QStringLiteral("M"), 10000);
        const QModelIndex tracked = model.indexForPath(repo + "/tracked.txt");
        QCOMPARE(badgeOf(model, repo + "/tracked.txt").color, QStringLiteral("warning"));
        QVERIFY(tracked.data(Qt::ToolTipRole).toString().contains("Git: modified"));
        QVERIFY(!model.indexForPath(repo + "/src").data(DirectoryModel::BadgeRole).isValid());
        // The path bar's text: the branch and the number of changes.
        const Extensions::Info info = host->info(model.extensionsFolder());
        QCOMPARE(info.text, QStringLiteral("main · 1"));
        QCOMPARE(info.icon, QStringLiteral("branch"));
        // A change in the open folder refreshes the marks with the listing.
        writeFile(repo + "/top.txt");
        QTRY_COMPARE_WITH_TIMEOUT(badgeOf(model, repo + "/top.txt").text, QStringLiteral("U"), 10000);
        // Deeper changes aren't watched: the window asks again (activation, jobs).
        writeFile(repo + "/src/main.cpp", "int main() { return 1; }\n");
        model.refreshExtensions();
        QTRY_VERIFY_WITH_TIMEOUT(model.indexForPath(repo + "/src").data(DirectoryModel::BadgeRole).isValid(), 10000);
        QVERIFY(badgeOf(model, repo + "/src").text.isEmpty()); // a dot: changes inside

        // Switched off in Preferences: the marks go at once.
        host->setEnabled("git", false);
        QVERIFY(!model.indexForPath(repo + "/tracked.txt").data(DirectoryModel::BadgeRole).isValid());
        QVERIFY(host->info(model.extensionsFolder()).text.isEmpty());
    }
}

// Stage and Unstage: offered for marked items only, and they really run git.
void TestExtensions::gitStageAndUnstage()
{
    if (QStandardPaths::findExecutable("git").isEmpty())
        QSKIP("git isn't installed");
    const QString repo = m_dir + "/repo";
    QDir().mkpath(repo);
    QVERIFY(git(repo, {"init", "-q"}));
    writeFile(repo + "/tracked.txt", "one\n");
    writeFile(repo + "/same.txt", "same\n");
    QVERIFY(git(repo, {"add", "."}));
    QVERIFY(git(repo, {"commit", "-q", "-m", "first"}));
    writeFile(repo + "/tracked.txt", "two\n");
    auto staged = [&] {
        QProcess p;
        p.setWorkingDirectory(repo);
        p.start("git", {"diff", "--cached", "--name-only"});
        p.waitForFinished(20000);
        return QString::fromUtf8(p.readAllStandardOutput()).trimmed();
    };

    Host host;
    host.setEnabled("git", true);
    host.watch(repo);
    const Extensions::Item changed{repo + "/tracked.txt", "text/plain", false};
    const Extensions::Item same{repo + "/same.txt", "text/plain", false};
    auto command = [&](const Extensions::Item &item, const QString &id) {
        for (const Extensions::Command &c : host.commands(repo, {item})) {
            if (c.extension == "git" && c.id == id)
                return c;
        }
        return Extensions::Command();
    };
    QTRY_VERIFY_WITH_TIMEOUT(!command(changed, "stage").id.isEmpty(), 10000);
    QVERIFY(command(same, "stage").id.isEmpty()); // nothing to stage in an unchanged file

    QSignalSpy said(&host, &Host::message);
    QVERIFY(staged().isEmpty());
    host.invoke(command(changed, "stage"), repo, {changed.path});
    QTRY_VERIFY_WITH_TIMEOUT(!said.isEmpty(), 20000);
    QCOMPARE(said.last().at(1).toString(), QStringLiteral("Staged"));
    QCOMPARE(staged(), QStringLiteral("tracked.txt"));

    said.clear();
    QTRY_VERIFY_WITH_TIMEOUT(!command(changed, "unstage").id.isEmpty(), 10000);
    host.invoke(command(changed, "unstage"), repo, {changed.path});
    QTRY_VERIFY_WITH_TIMEOUT(!said.isEmpty(), 20000);
    QCOMPARE(said.last().at(1).toString(), QStringLiteral("Unstaged"));
    QVERIFY(staged().isEmpty());
    host.setEnabled("git", false);
}

// Send by Email moved out of the window into an extension: offered for
// files, not folders, and it starts the email program with them attached.
void TestExtensions::sendByEmailIsAnExtension()
{
    const QString log = m_dir + "/email.log";
    writeFile(m_dir + "/xdg-email", "#!/bin/sh\nprintf '%s\\n' \"$@\" > '" + log.toUtf8() + "'\n", true);
    qputenv("ARGONAUT_EMAIL_PROGRAM", QFile::encodeName(m_dir + "/xdg-email"));
    Host host;
    host.setEnabled("send-by-email", true);
    host.watch(m_dir);
    const Extensions::Item file{m_dir + "/report.pdf", "application/pdf", false};
    const Extensions::Item dir{m_dir + "/sub", "inode/directory", true};
    QTRY_VERIFY_WITH_TIMEOUT(!host.commands(m_dir, {file}).isEmpty(), 10000);
    const Extensions::Command send = host.commands(m_dir, {file}).first();
    QCOMPARE(send.label, QStringLiteral("Send by Email…"));
    QVERIFY(host.commands(m_dir, {dir}).isEmpty());
    QVERIFY(host.commands(m_dir, {file, dir}).isEmpty());
    host.invoke(send, m_dir, {file.path});
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(log), 10000);
    QFile f(log);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(QString::fromUtf8(f.readAll()), QStringLiteral("--attach\n%1\n").arg(file.path));
    qunsetenv("ARGONAUT_EMAIL_PROGRAM");
    host.setEnabled("send-by-email", false);
}

int main(int argc, char **argv)
{
    TestSupport::Sandbox sandbox;
    if (!sandbox.isValid())
        return 1;
    qputenv("GIT_CONFIG_NOSYSTEM", "1");
    // Only these extensions: a fake one made by the test, and the Git
    // extension from the build folder. Never the ones installed here.
    g_extensions = sandbox.path() + "/extensions";
    QDir().mkpath(g_extensions);
    qputenv("ARGONAUT_EXTENSIONS_PATH", QFile::encodeName(g_extensions) + ":" GIT_EXTENSION_FOLDER);
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("argonaut-test");
    QCoreApplication::setApplicationName("extensions");
    TestExtensions test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_extensions.moc"
