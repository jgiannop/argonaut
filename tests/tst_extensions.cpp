// Tests for extensions (Extensions::Host): finding them, the on/off rule,
// the messages, what a misbehaving one can't do, and the Git extension end
// to end through the folder model.

#include "DirectoryModel.h"
#include "Extensions.h"
#include "Sandbox.h"
#include "TestSupport.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QImage>
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
    void removedWhileRunning();
    void commandsFitTheSelection();
    void invokeTellsTheExtension();
    void previewsAreAskedAndChecked();
    void documentPreview();
    void systemDashboard();
    void gitThroughTheModel();
    void gitStageAndUnstage();
    void gitChangesCommitAndBranches();
    void gitChangesAsAListOfFiles();
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

// Its folder is deleted while it runs: at the next look it is stopped and
// what it said goes; nothing else is disturbed. Put back under the same
// name, it is a stranger again: off until switched on.
void TestExtensions::removedWhileRunning()
{
    const QString folder = g_extensions + "/visitor";
    auto install = [&] {
        writeFile(folder + "/extension.json", R"({"id": "visitor", "name": "Visitor", "protocol": 1, "exec": ["fake.py"]})");
        QVERIFY(QFile::copy(QStringLiteral(FAKE_EXTENSION), folder + "/fake.py"));
        QFile::setPermissions(folder + "/fake.py", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    };
    install();
    Host host;
    host.setEnabled("visitor", true);
    QSignalSpy changed(&host, &Host::folderChanged);
    host.watch(m_dir);
    QTRY_VERIFY_WITH_TIMEOUT(host.badge(m_dir, m_dir + "/a.txt").valid, 10000);
    QVERIFY(host.isRunning("visitor"));
    QVERIFY(!host.commands(m_dir, {{m_dir + "/a.txt", "text/plain", false}}).isEmpty());

    QVERIFY(QDir(folder).removeRecursively());
    QVERIFY(host.badge(m_dir, m_dir + "/a.txt").valid); // still running until the next look
    changed.clear();
    host.rescan(); // what opening Preferences does
    QVERIFY(!host.isRunning("visitor"));
    QVERIFY(!changed.isEmpty()); // the views are told
    QVERIFY(!host.badge(m_dir, m_dir + "/a.txt").valid);
    QVERIFY(host.commands(m_dir, {{m_dir + "/a.txt", "text/plain", false}}).isEmpty());
    for (const Manifest &m : host.extensions())
        QVERIFY(m.id != "visitor");
    QVERIFY(!host.isEnabled("visitor"));

    // Back under the same name: listed, but not started.
    install();
    host.rescan();
    bool listed = false;
    for (const Manifest &m : host.extensions())
        listed = listed || m.id == "visitor";
    QVERIFY(listed);
    QVERIFY(!host.isEnabled("visitor"));
    QTest::qWait(300);
    QVERIFY(!host.isRunning("visitor"));
    QVERIFY(!host.badge(m_dir, m_dir + "/a.txt").valid);
    QDir(folder).removeRecursively();
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
    QCOMPARE(ids(m_dir, {}), (QStringList{"here", "typed", "showme", "listme", "live", "late"})); // the folder's empty space
    QCOMPARE(ids(QString(), {text}), (QStringList{"hello", "ask"}));    // Recent: no folder, no folder commands
    QVERIFY(ids(QString(), {}).isEmpty());
    // A command for the menu bar is in its menu and nowhere else; one for a
    // menu there isn't is nowhere.
    QCOMPARE(int(host.menuCommands("view").size()), 1);
    QCOMPARE(host.menuCommands("view").first().id, QStringLiteral("inmenu"));
    QVERIFY(host.menuCommands("file").isEmpty());
    QVERIFY(host.menuCommands("tools").isEmpty());
    // A command limited to items can't reach outside its folder.
    const Extensions::Item outside{QFileInfo(m_dir).path() + "/outside.txt", "text/plain", false};
    QVERIFY(!ids(m_dir, {outside}).contains("only-a"));
    // The question a command wants asked comes along.
    for (const Extensions::Command &c : host.commands(m_dir, {text})) {
        if (c.id == "ask")
            QCOMPARE(c.confirm, QStringLiteral("Really do the risky thing?"));
    }
    // A submenu (two levels at most), a tick, and a text it wants typed first.
    for (const Extensions::Command &c : host.commands(m_dir, {})) {
        if (c.id != "typed")
            continue;
        QCOMPARE(c.group, QStringLiteral("Fake/Sub"));
        QVERIFY(c.checked);
        QVERIFY(c.asks);
        QCOMPARE(c.askLabel, QStringLiteral("Say what:"));
        QCOMPARE(c.askText, QStringLiteral("default"));
        QVERIFY(!c.askMultiline);
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

    // What the user typed goes along; a result to show comes back as content.
    said.clear();
    QSignalSpy shown(&host, &Host::showRequested);
    for (const Extensions::Command &c : host.commands(m_dir, {})) {
        if (c.id == "typed")
            host.invoke(c, m_dir, {}, "some words");
        if (c.id == "showme")
            host.invoke(c, m_dir, {});
    }
    QTRY_COMPARE_WITH_TIMEOUT(said.size(), 2, 10000);
    QCOMPARE(said.first().at(1).toString(), QStringLiteral("invoked typed on 0:  [some words]"));
    QCOMPARE(shown.size(), 1);
    QCOMPARE(shown.first().at(0).toString(), QStringLiteral("Fake"));
    QCOMPARE(shown.first().at(1).toString(), QStringLiteral("A result from the fake")); // one line
    const auto content = shown.first().at(2).value<Extensions::PreviewReply>();
    QCOMPARE(content.kind, Extensions::PreviewReply::Diff);
    QVERIFY(content.text.contains("+new line"));
    QCOMPARE(content.note, QStringLiteral("2 lines"));
}

// Previews: only for the types an extension named, the answer comes back
// with the request's number, and a picture must be a real file by full path.
void TestExtensions::previewsAreAskedAndChecked()
{
    Host host;
    host.setEnabled("fake", true);
    QSignalSpy changed(&host, &Host::folderChanged);
    host.watch(m_dir);
    QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(host.canPreview("text/csv"), 5000);
    QVERIFY(!host.canPreview("text/plain"));
    QCOMPARE(host.requestPreview(m_dir + "/a.txt", "text/plain", QSize(800, 600)), 0); // nobody to ask

    QSignalSpy ready(&host, &Host::previewReady);
    auto ask = [&](const QString &name) {
        ready.clear();
        const int id = host.requestPreview(m_dir + "/" + name, "text/csv", QSize(800, 600));
        if (id == 0 || !ready.wait(10000))
            return Extensions::PreviewReply();
        if (ready.first().at(0).toInt() != id)
            return Extensions::PreviewReply();
        return ready.first().at(1).value<Extensions::PreviewReply>();
    };
    Extensions::PreviewReply reply = ask("table.csv");
    QCOMPARE(reply.kind, Extensions::PreviewReply::Text);
    QCOMPARE(reply.text, QStringLiteral("FAKE PREVIEW of table.csv as text/csv"));
    QVERIFY(reply.monospace);

    QCOMPARE(ask("nothing.csv").kind, Extensions::PreviewReply::None);

    reply = ask("picture.csv");
    QCOMPARE(reply.kind, Extensions::PreviewReply::Image);
    QVERIFY(QFileInfo(reply.image).isFile());
    QCOMPARE(reply.note, QStringLiteral("a picture from the fake")); // one line
    QCOMPARE(ask("relative.csv").kind, Extensions::PreviewReply::None); // not a full path: not used

    host.setEnabled("fake", false);
    QVERIFY(!host.canPreview("text/csv"));
}

// The Document Preview extension that comes with Argonaut: Markdown as it
// is, CSV and spreadsheets as a table. Spreadsheets go through the sandbox.
// The System Dashboard extension: its command answers with a picture as a
// named result; told the room there is, it draws for that and keeps
// drawing; told the result is gone, it stops.
void TestExtensions::systemDashboard()
{
    Host host;
    bool there = false;
    for (const Extensions::Manifest &m : host.extensions())
        there = there || m.id == "system-dashboard";
    if (!there)
        QSKIP("the System Dashboard extension isn't built here (Linux only)");
    host.setEnabled("system-dashboard", true);
    host.watch(m_dir);
    Extensions::Command command;
    QTRY_VERIFY_WITH_TIMEOUT([&] {
        for (const Extensions::Command &c : host.menuCommands("view")) {
            if (c.extension == "system-dashboard")
                command = c;
        }
        return !command.id.isEmpty();
    }(), 10000);
    QCOMPARE(command.label, QStringLiteral("System Dashboard"));
    for (const Extensions::Command &c : host.commands(m_dir, {})) // in the View menu, not in the right-click menu
        QVERIFY(c.extension != "system-dashboard");

    QSignalSpy shown(&host, &Host::showRequested);
    host.invoke(command, m_dir, {});
    QTRY_COMPARE_WITH_TIMEOUT(shown.size(), 1, 10000);
    auto content = shown.last().at(2).value<Extensions::PreviewReply>();
    QCOMPARE(content.kind, Extensions::PreviewReply::Image);
    QCOMPARE(content.extension, QStringLiteral("system-dashboard"));
    QCOMPARE(content.resultId, QStringLiteral("dashboard"));
    QVERIFY(!content.update);
    QVERIFY(!QImage(content.image).isNull());

    // On screen, 500 pixels wide, in a light window: drawn again for that.
    Extensions::ResultLook look; // a light theme of our own: its colours are the picture's
    look.window = "#fdf6e3";
    look.surface = "#eee8d5";
    look.text = "#073642";
    look.dim = "#657b83";
    look.accent = "#d33682";
    look.border = "#d6cfba";
    host.resultShown("system-dashboard", "dashboard", true, QSize(500, 900), look);
    QTRY_COMPARE_WITH_TIMEOUT(shown.size(), 2, 10000);
    content = shown.last().at(2).value<Extensions::PreviewReply>();
    QVERIFY(content.update);
    QImage image(content.image);
    QCOMPARE(image.width(), 500);
    QVERIFY(image.height() <= 900);
    QCOMPARE(image.pixelColor(2, 2), QColor("#fdf6e3")); // the theme's background
    bool card = false, accent = false;
    for (int y = 0; y < image.height(); y += 2) {
        for (int x = 0; x < image.width(); x += 2) {
            card = card || image.pixelColor(x, y) == QColor("#eee8d5");
            accent = accent || image.pixelColor(x, y) == QColor("#d33682");
        }
    }
    QVERIFY(card);   // its panels
    QVERIFY(accent); // its gauges
    // And again by itself, while it is shown.
    QTRY_VERIFY_WITH_TIMEOUT(shown.size() >= 3, 10000);
    // No longer shown: it stops.
    host.resultShown("system-dashboard", "dashboard", false);
    QTest::qWait(300);
    const int count = int(shown.size());
    QTest::qWait(2600);
    QCOMPARE(int(shown.size()), count);
}

void TestExtensions::documentPreview()
{
    Host host;
    host.setEnabled("document-preview", true);
    host.watch(m_dir);
    QTRY_VERIFY_WITH_TIMEOUT(host.canPreview("text/markdown"), 10000);
    QVERIFY(host.canPreview("application/vnd.oasis.opendocument.spreadsheet"));
    QVERIFY(!host.canPreview("application/pdf"));
    QSignalSpy ready(&host, &Host::previewReady);
    auto ask = [&](const QString &path, const QString &mime) {
        ready.clear();
        const int id = host.requestPreview(path, mime, QSize(800, 600));
        if (id == 0 || !ready.wait(20000))
            return Extensions::PreviewReply();
        return ready.first().at(1).value<Extensions::PreviewReply>();
    };

    writeFile(m_dir + "/notes.md", "# Heading\n\nSome *text*.\n");
    Extensions::PreviewReply reply = ask(m_dir + "/notes.md", "text/markdown");
    QCOMPARE(reply.kind, Extensions::PreviewReply::Markdown);
    QCOMPARE(reply.text, QStringLiteral("# Heading\n\nSome *text*.\n"));

    // CSV: quoted cells with the separator, a line break and quotes inside;
    // what looks like HTML in a cell is shown, not obeyed.
    writeFile(m_dir + "/people.csv", "name;city\n\"Smith; John\";\"New\nYork\"\n<b>bold</b>;\"say \"\"hi\"\"\"\n");
    reply = ask(m_dir + "/people.csv", "text/csv");
    QCOMPARE(reply.kind, Extensions::PreviewReply::Html);
    QVERIFY(reply.text.contains("<th align=\"left\">name</th>"));
    QVERIFY(reply.text.contains("<td>Smith; John</td>"));
    QVERIFY(reply.text.contains("New<br>York"));
    QVERIFY(reply.text.contains("&lt;b&gt;bold&lt;/b&gt;"));
    QVERIFY(reply.text.contains("say &quot;hi&quot;"));
    QCOMPARE(reply.note, QStringLiteral("3 rows"));

    // A file that isn't text gets no table.
    writeFile(m_dir + "/binary.csv", QByteArray("a,b\0c", 6));
    QCOMPARE(ask(m_dir + "/binary.csv", "text/csv").kind, Extensions::PreviewReply::None);

    // Spreadsheets, made here with Python's zipfile.
    const QByteArray script = R"PY(
import sys, zipfile
d = sys.argv[1]
with zipfile.ZipFile(d + "/book.xlsx", "w") as z:
    z.writestr("xl/workbook.xml", "<workbook/>")
    z.writestr("xl/sharedStrings.xml", "<sst><si><t>Item</t></si><si><r><t>Pri</t></r><r><t>ce</t></r></si><si><t>Bread &amp; &lt;butter&gt;</t></si></sst>")
    z.writestr("xl/worksheets/sheet1.xml", '<worksheet><sheetData>'
        '<row r="1"><c r="A1" t="s"><v>0</v></c><c r="B1" t="s"><v>1</v></c></row>'
        '<row r="2"><c r="A2" t="s"><v>2</v></c><c r="C2"><v>2.5</v></c></row>'
        '<row r="3"><c r="A3" t="inlineStr"><is><t>inline</t></is></c><c r="B3" t="b"><v>1</v></c></row>'
        '</sheetData></worksheet>')
    z.writestr("xl/worksheets/sheet2.xml", "<worksheet><sheetData/></worksheet>")
with zipfile.ZipFile(d + "/book.ods", "w") as z:
    z.writestr("content.xml", '<office:document-content xmlns:office="o" xmlns:table="t" xmlns:text="x"><office:body><office:spreadsheet>'
        '<table:table table:name="One">'
        '<table:table-row><table:table-cell><text:p>City</text:p></table:table-cell><table:table-cell table:number-columns-repeated="2"><text:p>same</text:p></table:table-cell></table:table-row>'
        '<table:table-row><table:table-cell><text:p>Athens</text:p><text:p>Greece</text:p></table:table-cell><table:table-cell table:number-columns-repeated="1000"/></table:table-row>'
        '<table:table-row table:number-rows-repeated="1048000"><table:table-cell table:number-columns-repeated="1024"/></table:table-row>'
        '</table:table><table:table table:name="Two"><table:table-row><table:table-cell><text:p>other sheet</text:p></table:table-cell></table:table-row></table:table>'
        '</office:spreadsheet></office:body></office:document-content>')
)PY";
    writeFile(m_dir + "/make.py", script);
    QCOMPARE(QProcess::execute("python3", {m_dir + "/make.py", m_dir}), 0);

    reply = ask(m_dir + "/book.xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet");
    if (reply.kind == Extensions::PreviewReply::None) {
        bool boxWorks = false;
        Sandbox::run("true", {}, m_dir + "/notes.md", &boxWorks);
        if (!boxWorks)
            QSKIP("no working sandbox here (bubblewrap): spreadsheets are not previewed without one");
    }
    QCOMPARE(reply.kind, Extensions::PreviewReply::Html);
    QVERIFY2(reply.text.contains("<th align=\"left\">Item</th><th align=\"left\">Price</th>"), qPrintable(reply.text));
    QVERIFY(reply.text.contains("<td>Bread &amp; &lt;butter&gt;</td><td></td><td>2.5</td>")); // B2 is empty
    QVERIFY(reply.text.contains("<td>inline</td><td>TRUE</td>"));
    QCOMPARE(reply.note, QStringLiteral("3 rows · first of 2 sheets"));

    reply = ask(m_dir + "/book.ods", "application/vnd.oasis.opendocument.spreadsheet");
    QCOMPARE(reply.kind, Extensions::PreviewReply::Html);
    QVERIFY2(reply.text.contains("<th align=\"left\">City</th><th align=\"left\">same</th><th align=\"left\">same</th>"),
             qPrintable(reply.text));
    QVERIFY(reply.text.contains("<td>Athens<br>Greece</td>"));
    QVERIFY(!reply.text.contains("other sheet"));
    QCOMPARE(reply.note, QStringLiteral("2 rows · first of 2 sheets")); // the million empty rows aren't rows
    host.setEnabled("document-preview", false);
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

// The Git menu on a repository: its changes as a diff, committing with a
// typed message, the branches, and discarding a file's changes.
void TestExtensions::gitChangesCommitAndBranches()
{
    if (QStandardPaths::findExecutable("git").isEmpty())
        QSKIP("git isn't installed");
    const QString repo = m_dir + "/repo";
    QDir().mkpath(repo);
    QVERIFY(git(repo, {"init", "-q"}));
    QVERIFY(git(repo, {"config", "user.name", "Test"}));
    QVERIFY(git(repo, {"config", "user.email", "t@example.com"}));
    QVERIFY(git(repo, {"config", "commit.gpgsign", "false"}));
    writeFile(repo + "/tracked.txt", "one\n");
    QVERIFY(git(repo, {"add", "."}));
    QVERIFY(git(repo, {"commit", "-q", "-m", "first"}));
    writeFile(repo + "/tracked.txt", "two\n");
    writeFile(repo + "/fresh.txt", "new\n");
    // A hook that would refuse every commit: hooks are off, so it isn't run.
    writeFile(repo + "/.git/hooks/pre-commit", "#!/bin/sh\ntouch \"$(dirname \"$0\")/ran\"\nexit 1\n", true);
    auto output = [&](const QStringList &args) {
        QProcess p;
        p.setWorkingDirectory(repo);
        p.start("git", args);
        p.waitForFinished(20000);
        return QString::fromUtf8(p.readAllStandardOutput()).trimmed();
    };

    Host host;
    host.setEnabled("git", true);
    host.watch(repo);
    auto onFolder = [&](const QString &id) {
        for (const Extensions::Command &c : host.commands(repo, {})) {
            if (c.extension == "git" && c.id == id)
                return c;
        }
        return Extensions::Command();
    };
    QSignalSpy said(&host, &Host::message);
    QSignalSpy shown(&host, &Host::showRequested);
    auto lastSaid = [&] { return said.isEmpty() ? QString() : said.last().at(1).toString(); };

    // The menu: everything under "Git", branches in their own submenu.
    QTRY_VERIFY_WITH_TIMEOUT(!onFolder("commit").id.isEmpty(), 10000);
    const Extensions::Command commit = onFolder("commit");
    QCOMPARE(commit.group, QStringLiteral("Git"));
    QCOMPARE(commit.label, QStringLiteral("Commit All Changed Files (1)…")); // nothing staged; the new file isn't tracked
    QVERIFY(commit.asks && commit.askMultiline);
    QCOMPARE(onFolder("switch:main").group, QStringLiteral("Git/Switch Branch"));
    QVERIFY(onFolder("switch:main").checked);
    QVERIFY(onFolder("new-branch").asks);

    // Show All Changes: a diff, with the untracked file named.
    host.invoke(onFolder("diff-all"), repo, {});
    QTRY_COMPARE_WITH_TIMEOUT(shown.size(), 1, 10000);
    auto content = shown.last().at(2).value<Extensions::PreviewReply>();
    QCOMPARE(content.kind, Extensions::PreviewReply::Diff);
    QVERIFY2(content.text.contains("-one") && content.text.contains("+two"), qPrintable(content.text));
    QVERIFY(content.text.contains("# New, not tracked yet: fresh.txt"));
    QCOMPARE(content.note, QStringLiteral("1 changed file · 1 new"));
    QCOMPARE(shown.last().at(1).toString(), QStringLiteral("Changes on main"));

    // Commit with the typed message: the tracked change, not the new file.
    host.invoke(commit, repo, {}, "Second commit");
    QTRY_COMPARE_WITH_TIMEOUT(lastSaid(), QStringLiteral("Committed"), 30000);
    QCOMPARE(output({"log", "-1", "--format=%s"}), QStringLiteral("Second commit"));
    QCOMPARE(output({"status", "--porcelain"}), QStringLiteral("?? fresh.txt"));
    QVERIFY(!QFile::exists(repo + "/.git/hooks/ran"));
    QTRY_VERIFY_WITH_TIMEOUT(onFolder("commit").id.isEmpty(), 10000); // nothing left to commit

    // Staged work is what a commit takes, and the label says so.
    QVERIFY(git(repo, {"add", "fresh.txt"}));
    host.refresh(repo);
    QTRY_COMPARE_WITH_TIMEOUT(onFolder("commit").label, QStringLiteral("Commit Staged Changes (1)…"), 10000);

    // A new branch, by a typed name; a name that would be an option is refused.
    host.invoke(onFolder("new-branch"), repo, {}, "--force");
    QTRY_COMPARE_WITH_TIMEOUT(lastSaid(), QStringLiteral("That can't be a branch's name."), 10000);
    host.invoke(onFolder("new-branch"), repo, {}, "feature");
    QTRY_COMPARE_WITH_TIMEOUT(lastSaid(), QStringLiteral("On the new branch feature"), 10000);
    QCOMPARE(output({"branch", "--show-current"}), QStringLiteral("feature"));
    QTRY_VERIFY_WITH_TIMEOUT(onFolder("switch:feature").checked, 10000);
    QVERIFY(!onFolder("switch:main").checked);
    QTRY_VERIFY_WITH_TIMEOUT(host.info(repo).text.startsWith("feature"), 10000);

    // Back to main through the branch list.
    host.invoke(onFolder("switch:main"), repo, {});
    QTRY_COMPARE_WITH_TIMEOUT(lastSaid(), QStringLiteral("Switched to main"), 10000);
    QCOMPARE(output({"branch", "--show-current"}), QStringLiteral("main"));

    // Discard: asks first (the question travels with the command), then the
    // file is as it was at the last commit.
    writeFile(repo + "/tracked.txt", "three\n");
    host.refresh(repo);
    const Extensions::Item file{repo + "/tracked.txt", "text/plain", false};
    auto onFile = [&](const QString &id) {
        for (const Extensions::Command &c : host.commands(repo, {file})) {
            if (c.extension == "git" && c.id == id)
                return c;
        }
        return Extensions::Command();
    };
    QTRY_VERIFY_WITH_TIMEOUT(!onFile("discard").id.isEmpty(), 10000);
    QVERIFY(onFile("discard").confirm.contains("can't be undone"));
    host.invoke(onFile("diff"), repo, {file.path});
    QTRY_COMPARE_WITH_TIMEOUT(shown.size(), 2, 10000);
    QCOMPARE(shown.last().at(1).toString(), QStringLiteral("Changes in tracked.txt"));
    host.invoke(onFile("discard"), repo, {file.path});
    QTRY_COMPARE_WITH_TIMEOUT(lastSaid(), QStringLiteral("Changes discarded"), 10000);
    QFile f(file.path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), QByteArray("two\n"));
    // A new file has no committed version: Discard isn't offered for it.
    const Extensions::Item added{repo + "/fresh.txt", "text/plain", false};
    bool offered = false;
    for (const Extensions::Command &c : host.commands(repo, {added}))
        offered = offered || c.id == "discard";
    QVERIFY(!offered);
    host.setEnabled("git", false);
}

// "Show Changes": the changed files as a list the file pane shows like a
// folder, each with its mark, its place in the repository and its changes
// as its preview; and the list follows what happens to the repository.
void TestExtensions::gitChangesAsAListOfFiles()
{
    if (QStandardPaths::findExecutable("git").isEmpty())
        QSKIP("git isn't installed");
    const QString repo = m_dir + "/repo";
    QDir().mkpath(repo);
    QVERIFY(git(repo, {"init", "-q"}));
    writeFile(repo + "/src/deep/tracked.txt", "one\n");
    writeFile(repo + "/same.txt", "same\n");
    QVERIFY(git(repo, {"add", "."}));
    QVERIFY(git(repo, {"commit", "-q", "-m", "first"}));
    writeFile(repo + "/src/deep/tracked.txt", "two\n");
    writeFile(repo + "/fresh.txt", "new\n");
    const QString tracked = repo + "/src/deep/tracked.txt", fresh = repo + "/fresh.txt";

    Host *host = Host::instance();
    host->rescan();
    host->setEnabled("git", true);
    DirectoryModel folder; // a pane on a folder inside the repository, not its top
    QSignalSpy folderLoaded(&folder, &DirectoryModel::loaded);
    folder.setPath(repo + "/src");
    QVERIFY(folderLoaded.wait(10000));
    auto onFolder = [&](const QString &id) {
        for (const Extensions::Command &c : host->commands(repo + "/src", {})) {
            if (c.extension == "git" && c.id == id)
                return c;
        }
        return Extensions::Command();
    };
    QTRY_VERIFY_WITH_TIMEOUT(!onFolder("changes").id.isEmpty(), 10000);
    QCOMPARE(onFolder("changes").label, QStringLiteral("Show Changes"));

    QSignalSpy asked(host, &Host::showListingRequested);
    host->invoke(onFolder("changes"), repo + "/src", {});
    QTRY_COMPARE_WITH_TIMEOUT(asked.size(), 1, 10000);
    const QString place = asked.first().at(0).toString();
    QVERIFY(Host::isListingPath(place));
    QVERIFY(DirectoryModel::isVirtualPath(place));
    Extensions::Listing list = host->listing(place);
    QVERIFY(list.valid);
    QCOMPARE(list.title, QStringLiteral("Changes on main"));
    QCOMPARE(list.folder, repo); // the whole repository, from wherever it was asked
    QVERIFY(list.previews);
    QStringList files = list.files;
    files.sort();
    QCOMPARE(files, (QStringList{fresh, tracked}));

    // In a pane: rows like in a folder, with marks and where they are.
    DirectoryModel model;
    QSignalSpy loaded(&model, &DirectoryModel::loaded);
    model.setPath(place);
    QVERIFY(loaded.wait(10000));
    QCOMPARE(model.rowCount(), 2);
    QVERIFY(model.indexForPath(tracked).isValid());
    QCOMPARE(model.extensionsFolder(), repo);
    QTRY_COMPARE_WITH_TIMEOUT(model.indexForPath(tracked).data(DirectoryModel::BadgeRole).value<Badge>().text,
                              QStringLiteral("M"), 10000);
    QCOMPARE(model.indexForPath(fresh).data(DirectoryModel::BadgeRole).value<Badge>().text, QStringLiteral("U"));
    QCOMPARE(model.index(model.indexForPath(tracked).row(), 2).data().toString(), QStringLiteral("src/deep"));
    QCOMPARE(model.index(model.indexForPath(fresh).row(), 2).data().toString(), QStringLiteral("repo"));

    // A file's preview there is its changes; a new file has none (Argonaut shows the file).
    QSignalSpy ready(host, &Host::previewReady);
    auto previewOf = [&](const QString &path) {
        ready.clear();
        const int id = host->requestListingPreview(place, path, "text/plain", QSize(800, 600));
        if (id == 0 || !ready.wait(10000))
            return Extensions::PreviewReply();
        return ready.first().at(1).value<Extensions::PreviewReply>();
    };
    const Extensions::PreviewReply diff = previewOf(tracked);
    QCOMPARE(diff.kind, Extensions::PreviewReply::Diff);
    QVERIFY(diff.text.contains("-one") && diff.text.contains("+two"));
    QCOMPARE(diff.note, QStringLiteral("1 added · 1 removed (lines)"));
    QCOMPARE(previewOf(fresh).kind, Extensions::PreviewReply::None);
    QCOMPARE(host->requestListingPreview(place, repo + "/same.txt", "text/plain", QSize(800, 600)), 0); // not in the list

    // Committed: the list shrinks by itself.
    QVERIFY(git(repo, {"commit", "-q", "-a", "-m", "second"}));
    host->refreshAll();
    QTRY_COMPARE_WITH_TIMEOUT(model.rowCount(), 1, 15000);
    QVERIFY(model.indexForPath(fresh).isValid());

    // The extension switched off: the list is gone, and so are its rows.
    host->setEnabled("git", false);
    QVERIFY(!host->listing(place).valid);
    QTRY_COMPARE_WITH_TIMEOUT(model.rowCount(), 0, 10000);
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
