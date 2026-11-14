// Tests for Naming (Select Items Matching, Batch Rename) and Templates.
// Everything happens in a temporary folder; HOME and XDG_* point into it.

#include "FileOperations.h"
#include "Naming.h"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

namespace {

void writeFile(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(content);
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("<missing>");
}

QStringList names(const QString &dir)
{
    return QDir(dir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name);
}

} // namespace

class TestNaming : public QObject
{
    Q_OBJECT

public:
    explicit TestNaming(QString sandbox) : m_sandbox(std::move(sandbox)) {}

private slots:
    void init();
    void cleanup();

    void baseNames();
    void patterns_data();
    void patterns();
    void templateNames();
    void replaceNames();
    void checkFindsClashes();
    void batchRenames();
    void batchChainAndSwap();
    void batchRollsBack();
    void batchUndoAndRedo();
    void templatesFolder();
    void templatesList();
    void templatesCreate();

private:
    void waitForJob();

    QString m_sandbox;
    QString m_dir;
    FileOperations *m_ops = nullptr;
};

void TestNaming::init()
{
    m_dir = m_sandbox + "/work";
    QDir(m_dir).removeRecursively();
    QVERIFY(QDir().mkpath(m_dir));
    m_ops = new FileOperations(this);
}

void TestNaming::cleanup()
{
    delete m_ops;
    m_ops = nullptr;
    QDir(m_dir).removeRecursively();
}

void TestNaming::waitForJob()
{
    QEventLoop loop;
    bool done = false;
    connect(m_ops, &FileOperations::jobFinished, &loop, [&] {
        done = true;
        loop.quit();
    });
    QTimer::singleShot(30000, &loop, &QEventLoop::quit);
    loop.exec();
    QVERIFY2(done, "timed out waiting for a file job");
}

void TestNaming::baseNames()
{
    QString ext;
    QCOMPARE(Naming::baseName("Photo.jpg", false, &ext), QStringLiteral("Photo"));
    QCOMPARE(ext, QStringLiteral(".jpg"));
    QCOMPARE(Naming::baseName("backup.tar.gz", false, &ext), QStringLiteral("backup"));
    QCOMPARE(ext, QStringLiteral(".tar.gz"));
    QCOMPARE(Naming::baseName(".bashrc", false, &ext), QStringLiteral(".bashrc"));
    QCOMPARE(ext, QString());
    QCOMPARE(Naming::baseName("my.folder", true, &ext), QStringLiteral("my.folder"));
    QCOMPARE(ext, QString());
    QCOMPARE(Naming::baseName(".tar.gz", false, &ext), QStringLiteral(".tar"));
}

void TestNaming::patterns_data()
{
    QTest::addColumn<QString>("name");
    QTest::addColumn<QString>("pattern");
    QTest::addColumn<bool>("match");
    QTest::newRow("star ext") << "IMG_001.JPG" << "*.jpg" << true;
    QTest::newRow("star ext no") << "notes.txt" << "*.jpg" << false;
    QTest::newRow("question") << "IMG_2024.png" << "IMG_20??*" << true;
    QTest::newRow("question short") << "IMG_2.png" << "IMG_20??*" << false;
    QTest::newRow("class") << "alpha" << "[ab]*" << true;
    QTest::newRow("class no") << "gamma" << "[ab]*" << false;
    QTest::newRow("anchored") << "x.jpg.bak" << "*.jpg" << false;
    QTest::newRow("plain contains") << "Summer Holiday 2024" << "holiday" << true;
    QTest::newRow("plain no") << "Winter" << "holiday" << false;
    QTest::newRow("greek") << "Θάλασσα.jpg" << "θάλασσα*" << true;
    QTest::newRow("empty") << "anything" << "  " << false;
}

void TestNaming::patterns()
{
    QFETCH(QString, name);
    QFETCH(QString, pattern);
    QFETCH(bool, match);
    QCOMPARE(Naming::matchesPattern(name, pattern), match);
}

void TestNaming::templateNames()
{
    Naming::RenameOptions o;
    o.text = "Trip [01]";
    QCOMPARE(Naming::renamed("DSC1234.jpg", false, 0, o), QStringLiteral("Trip 01.jpg"));
    QCOMPARE(Naming::renamed("DSC1235.jpg", false, 11, o), QStringLiteral("Trip 12.jpg"));
    o.text = "[Original name]-[001]";
    o.start = 7;
    QCOMPARE(Naming::renamed("a.tar.gz", false, 0, o), QStringLiteral("a-007.tar.gz"));
    o.text = "[1] [Original name]";
    o.start = 1;
    QCOMPARE(Naming::renamed("Folder.v2", true, 99, o), QStringLiteral("100 Folder.v2"));
    // More digits than the tag's width are kept.
    o.text = "[01]";
    QCOMPARE(Naming::renamed("x.png", false, 122, o), QStringLiteral("123.png"));
}

void TestNaming::replaceNames()
{
    Naming::RenameOptions o;
    o.mode = Naming::RenameOptions::Mode::Replace;
    o.text = "img";
    o.replacement = "Photo";
    QCOMPARE(Naming::renamed("IMG_01.img", false, 0, o), QStringLiteral("Photo_01.img")); // not the extension
    o.caseSensitive = true;
    QCOMPARE(Naming::renamed("IMG_01.img", false, 0, o), QStringLiteral("IMG_01.img"));
    o.text = "_";
    o.replacement = " ";
    o.caseSensitive = false;
    QCOMPARE(Naming::renamed("a_b_c.txt", false, 0, o), QStringLiteral("a b c.txt"));
}

void TestNaming::checkFindsClashes()
{
    writeFile(m_dir + "/a.txt", "a");
    writeFile(m_dir + "/b.txt", "b");
    writeFile(m_dir + "/taken.txt", "t");
    QStringList problems;

    // Two items to the same name.
    QVERIFY(!Naming::checkBatch({{m_dir + "/a.txt", "x.txt"}, {m_dir + "/b.txt", "x.txt"}}, &problems));
    QVERIFY(!problems[0].isEmpty() && !problems[1].isEmpty());

    // A name taken by a file outside the batch.
    QVERIFY(!Naming::checkBatch({{m_dir + "/a.txt", "taken.txt"}, {m_dir + "/b.txt", "y.txt"}}, &problems));
    QVERIFY(!problems[0].isEmpty());
    QVERIFY(problems[1].isEmpty());

    // Bad names.
    QVERIFY(!Naming::checkBatch({{m_dir + "/a.txt", "a/b"}}, &problems));
    QVERIFY(!Naming::checkBatch({{m_dir + "/a.txt", ""}}, &problems));
    QVERIFY(!Naming::checkBatch({{m_dir + "/a.txt", ".."}}, &problems));

    // Taking another batch item's name is fine (it moves away), and so is
    // keeping your own.
    QVERIFY(Naming::checkBatch({{m_dir + "/a.txt", "b.txt"}, {m_dir + "/b.txt", "c.txt"}}, &problems));
    QVERIFY(Naming::checkBatch({{m_dir + "/a.txt", "a.txt"}}, &problems));
}

void TestNaming::batchRenames()
{
    writeFile(m_dir + "/one.txt", "1");
    writeFile(m_dir + "/two.txt", "2");
    QDir().mkdir(m_dir + "/folder");
    QString error;
    const auto steps = Naming::applyBatch({{m_dir + "/one.txt", "A.txt"},
                                           {m_dir + "/two.txt", "two.txt"}, // unchanged: skipped
                                           {m_dir + "/folder", "B"}},
                                          &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(steps.size(), 2);
    QCOMPARE(names(m_dir), QStringList({"A.txt", "B", "two.txt"}));
    QCOMPARE(readFile(m_dir + "/A.txt"), QByteArray("1"));
}

void TestNaming::batchChainAndSwap()
{
    // Chain: f1 -> f2, f2 -> f3 (numbers shifted by one).
    writeFile(m_dir + "/f1", "1");
    writeFile(m_dir + "/f2", "2");
    QString error;
    auto steps = Naming::applyBatch({{m_dir + "/f1", "f2"}, {m_dir + "/f2", "f3"}}, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(readFile(m_dir + "/f2"), QByteArray("1"));
    QCOMPARE(readFile(m_dir + "/f3"), QByteArray("2"));
    QVERIFY(!QFileInfo::exists(m_dir + "/f1"));

    // Swap and a 3-cycle go through a temporary name and leave nothing behind.
    writeFile(m_dir + "/x", "x");
    writeFile(m_dir + "/y", "y");
    writeFile(m_dir + "/p", "p");
    writeFile(m_dir + "/q", "q");
    writeFile(m_dir + "/r", "r");
    steps = Naming::applyBatch({{m_dir + "/x", "y"},
                                {m_dir + "/y", "x"},
                                {m_dir + "/p", "q"},
                                {m_dir + "/q", "r"},
                                {m_dir + "/r", "p"}},
                               &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(readFile(m_dir + "/x"), QByteArray("y"));
    QCOMPARE(readFile(m_dir + "/y"), QByteArray("x"));
    QCOMPARE(readFile(m_dir + "/p"), QByteArray("r"));
    QCOMPARE(readFile(m_dir + "/q"), QByteArray("p"));
    QCOMPARE(readFile(m_dir + "/r"), QByteArray("q"));
    QCOMPARE(names(m_dir), QStringList({"f2", "f3", "p", "q", "r", "x", "y"}));
}

void TestNaming::batchRollsBack()
{
    writeFile(m_dir + "/a", "a");
    writeFile(m_dir + "/b", "b");
    // "b" is renamed first, then "a" fails: its new name appeared after the
    // check. "b" must be renamed back; nothing may stay renamed.
    writeFile(m_dir + "/late", "someone else's");
    QString error;
    const auto steps = Naming::applyBatch({{m_dir + "/b", "c"}, {m_dir + "/a", "late"}}, &error);
    QVERIFY(steps.isEmpty());
    QVERIFY(!error.isEmpty());
    QCOMPARE(names(m_dir), QStringList({"a", "b", "late"}));
    QCOMPARE(readFile(m_dir + "/late"), QByteArray("someone else's"));
}

void TestNaming::batchUndoAndRedo()
{
    // A chain and a swap: undo must run the steps backwards.
    writeFile(m_dir + "/f1", "1");
    writeFile(m_dir + "/f2", "2");
    writeFile(m_dir + "/x", "x");
    writeFile(m_dir + "/y", "y");
    QString error;
    const auto steps = Naming::applyBatch({{m_dir + "/f1", "f2"},
                                           {m_dir + "/f2", "f3"},
                                           {m_dir + "/x", "y"},
                                           {m_dir + "/y", "x"}},
                                          &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    m_ops->recordUndo(FileJob::Kind::Move, steps, "Rename");

    m_ops->undo();
    waitForJob();
    QCOMPARE(names(m_dir), QStringList({"f1", "f2", "x", "y"}));
    QCOMPARE(readFile(m_dir + "/f1"), QByteArray("1"));
    QCOMPARE(readFile(m_dir + "/f2"), QByteArray("2"));
    QCOMPARE(readFile(m_dir + "/x"), QByteArray("x"));
    QCOMPARE(readFile(m_dir + "/y"), QByteArray("y"));

    m_ops->redo();
    waitForJob();
    QCOMPARE(names(m_dir), QStringList({"f2", "f3", "x", "y"}));
    QCOMPARE(readFile(m_dir + "/f2"), QByteArray("1"));
    QCOMPARE(readFile(m_dir + "/f3"), QByteArray("2"));
    QCOMPARE(readFile(m_dir + "/x"), QByteArray("y"));

    m_ops->undo();
    waitForJob();
    QCOMPARE(names(m_dir), QStringList({"f1", "f2", "x", "y"}));
    QCOMPARE(readFile(m_dir + "/x"), QByteArray("x"));
}

void TestNaming::templatesFolder()
{
    const QString home = QDir::homePath();
    const QString config = qEnvironmentVariable("XDG_CONFIG_HOME");
    QDir(home + "/Templates").removeRecursively();
    QDir(home + "/Vorlagen").removeRecursively();
    QFile::remove(config + "/user-dirs.dirs");

    QCOMPARE(Templates::folder(), QString()); // ~/Templates doesn't exist
    QVERIFY(QDir().mkpath(home + "/Templates"));
    QCOMPARE(Templates::folder(), home + "/Templates");

#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    // A translated folder from user-dirs.dirs.
    QVERIFY(QDir().mkpath(home + "/Vorlagen"));
    writeFile(config + "/user-dirs.dirs", "# comment\nXDG_DESKTOP_DIR=\"$HOME/Desktop\"\n"
                                          "XDG_TEMPLATES_DIR=\"$HOME/Vorlagen\"\n");
    QCOMPARE(Templates::folder(), home + "/Vorlagen");
    // Set to the home folder: turned off.
    writeFile(config + "/user-dirs.dirs", "XDG_TEMPLATES_DIR=\"$HOME/\"\n");
    QCOMPARE(Templates::folder(), QString());
    QFile::remove(config + "/user-dirs.dirs");
#endif
    QDir(home + "/Vorlagen").removeRecursively();
}

void TestNaming::templatesList()
{
    const QString t = m_dir + "/Templates";
    writeFile(t + "/Letter.odt", "odt");
    writeFile(t + "/note 10.txt", "");
    writeFile(t + "/note 2.txt", "");
    writeFile(t + "/.hidden", "");
    writeFile(t + "/Office/Invoice.ods", "ods");
    QDir().mkpath(t + "/Empty");

    const QList<Templates::Entry> entries = Templates::list(t);
    QStringList shown;
    for (const auto &e : entries)
        shown << e.name;
    QCOMPARE(shown, QStringList({"Office", "Letter", "note 2", "note 10"}));
    QVERIFY(entries[0].isFolder);
    QCOMPARE(entries[0].children.size(), 1);
    QCOMPARE(entries[0].children[0].name, QStringLiteral("Invoice"));
    QCOMPARE(entries[1].path, t + "/Letter.odt");
}

void TestNaming::templatesCreate()
{
    const QString t = m_dir + "/Templates";
    const QString target = m_dir + "/docs";
    QDir().mkpath(target);
    writeFile(t + "/Letter.odt", "template");
    QFile::setPermissions(t + "/Letter.odt", QFile::ReadOwner | QFile::ReadUser);

    const QString first = Templates::create(t + "/Letter.odt", target);
    QCOMPARE(first, target + "/Letter.odt");
    QCOMPARE(readFile(first), QByteArray("template"));
    QVERIFY(QFileInfo(first).isWritable()); // a read-only template gives a writable document

    writeFile(target + "/Letter (copy).odt", "mine"); // never overwritten
    const QString second = Templates::create(t + "/Letter.odt", target);
    QCOMPARE(second, target + "/Letter (copy 2).odt");
    QCOMPARE(readFile(target + "/Letter (copy).odt"), QByteArray("mine"));

    QString error;
    QVERIFY(Templates::create(t + "/gone.odt", target, &error).isEmpty());
    QVERIFY(!error.isEmpty());
    QFile::setPermissions(t + "/Letter.odt", QFile::ReadOwner | QFile::WriteOwner);
}

int main(int argc, char **argv)
{
    QTemporaryDir sandbox;
    if (!sandbox.isValid())
        return 1;
    qputenv("HOME", QFile::encodeName(sandbox.path() + "/home"));
    qputenv("XDG_DATA_HOME", QFile::encodeName(sandbox.path() + "/data"));
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(sandbox.path() + "/config"));
    qputenv("XDG_CACHE_HOME", QFile::encodeName(sandbox.path() + "/cache"));
    qputenv("KOSMOS_TRASH_HOME_ONLY", "1");
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    for (const char *dir : {"/home", "/data", "/config", "/cache"})
        QDir().mkpath(sandbox.path() + dir);

    QApplication app(argc, argv);
    TestNaming test(sandbox.path());
    return QTest::qExec(&test, argc, argv);
}

#include "tst_naming.moc"
