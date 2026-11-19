// Editing a text file from the preview: what may be edited, written back as
// it was (line ends, byte order mark, permissions), the version from before
// kept in the Trash, and another program's change noticed.
#include "TestSupport.h"
#include "TextFile.h"
#include "Trash.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QtTest>

class TestTextFile : public QObject
{
    Q_OBJECT

public:
    explicit TestTextFile(const QString &root)
        : m_home(root + "/home")
    {
    }

private slots:
    void init();
    void whatCanBeEdited();
    void writtenBackAsItWas();
    void theOldVersionGoesToTheTrash();
    void anotherProgramsChangeIsNoticed();
    void aLinkEditsItsTarget();

private:
    QString make(const QString &name, const QByteArray &data)
    {
        const QString path = m_dir + "/" + name;
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return {};
        file.write(data);
        return path;
    }
    QByteArray read(const QString &path)
    {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }
    QStringList trashed()
    {
        return QDir(Trash::homeFilesDir()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
    }

    QString m_home, m_dir;
};

void TestTextFile::init()
{
    m_dir = m_home + "/work";
    QDir(m_dir).removeRecursively();
    QDir().mkpath(m_dir);
    QDir(Trash::homeFilesDir()).removeRecursively();
    QDir(QFileInfo(Trash::homeFilesDir()).absolutePath() + "/info").removeRecursively();
}

void TestTextFile::whatCanBeEdited()
{
    QVERIFY(TextFile::problem(make("notes.txt", "hello\n")).isEmpty());
    QVERIFY(TextFile::problem(make("empty.txt", "")).isEmpty());
    QVERIFY(TextFile::problem(make("code.py", "print('γειά')\n")).isEmpty());
    QCOMPARE(TextFile::problem(make("binary.dat", QByteArray("ab\0cd", 5))), QStringLiteral("isn't text"));
    QCOMPARE(TextFile::problem(make("latin1.txt", "caf\xe9\n")), QStringLiteral("isn't UTF-8 text"));
    QCOMPARE(TextFile::problem(make("big.txt", QByteArray(TextFile::MaxBytes + 1, 'a'))), QStringLiteral("is too big to edit here (over 5 MB)"));
    QCOMPARE(TextFile::problem(make("holiday.pdf", "#!/bin/sh\necho gotcha\n")), QStringLiteral("is a program disguised as something else"));
    QCOMPARE(TextFile::problem(m_dir), QStringLiteral("isn't a file"));
    QCOMPARE(TextFile::problem(m_dir + "/nothing.txt"), QStringLiteral("is gone"));
    const QString locked = make("locked.txt", "x\n");
    QFile::setPermissions(locked, QFile::ReadOwner);
    QCOMPARE(TextFile::problem(locked), QStringLiteral("is read-only"));
    // In the Trash: restore it first.
    const QString gone = make("gone.txt", "x\n");
    QString inTrash;
    QVERIFY(QFile::moveToTrash(gone, &inTrash));
    QCOMPARE(TextFile::problem(inTrash), QStringLiteral("is in the Trash (restore it first)"));
    // load() says the same and loads nothing.
    QVERIFY(TextFile::load(m_dir + "/binary.dat").text.isEmpty());
    QCOMPARE(TextFile::load(m_dir + "/binary.dat").problem, QStringLiteral("isn't text"));
}

void TestTextFile::writtenBackAsItWas()
{
    const QString windows = make("windows.txt", "\xEF\xBB\xBF" "one\r\ntwo\r\n");
    QFile::setPermissions(windows, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    TextFile::Loaded file = TextFile::load(windows);
    QVERIFY(file.problem.isEmpty());
    QCOMPARE(file.text, QStringLiteral("one\ntwo\n")); // the editor sees plain line ends
    QVERIFY(file.crlf && file.bom);
    const TextFile::Saved saved = TextFile::save(&file, "one\ntwo\nthree\n", false);
    QVERIFY2(saved.ok, qPrintable(saved.error));
    QCOMPARE(read(windows), QByteArray("\xEF\xBB\xBF" "one\r\ntwo\r\nthree\r\n"));
    QVERIFY(QFile::permissions(windows) & QFile::ExeOwner);
    QVERIFY(!TextFile::changedOnDisk(file)); // what we wrote is what it is now

    const QString plain = make("unix.txt", "a\nb");
    file = TextFile::load(plain);
    QVERIFY(!file.crlf && !file.bom);
    QVERIFY(TextFile::save(&file, "a\nb\nc", false).ok);
    QCOMPARE(read(plain), QByteArray("a\nb\nc")); // no newline added at the end
    // Nothing left behind in the folder.
    QCOMPARE(QDir(m_dir).entryList(QDir::Files | QDir::Hidden).size(), 2);
}

void TestTextFile::theOldVersionGoesToTheTrash()
{
    const QString path = make("letter.txt", "the first words\n");
    TextFile::Loaded file = TextFile::load(path);
    const TextFile::Saved saved = TextFile::save(&file, "other words\n", true);
    QVERIFY2(saved.ok, qPrintable(saved.error));
    QCOMPARE(read(path), QByteArray("other words\n"));
    QVERIFY(!saved.oldVersion.isEmpty());
    QCOMPARE(read(saved.oldVersion), QByteArray("the first words\n"));
    const Trash::Info info = Trash::info(saved.oldVersion);
    QVERIFY(info.valid);
    QCOMPARE(info.originalPath, path); // restoring it brings it back where it was
    QCOMPARE(trashed().size(), 1);
    QCOMPARE(QDir(m_dir).entryList(QDir::Files | QDir::Hidden).size(), 1); // no copies left about

    // Kept and taken back (nothing was changed after all): the Trash is as it was, the file too.
    QString error;
    const QString again = TextFile::keepOldVersion(path, &error);
    QVERIFY2(!again.isEmpty(), qPrintable(error));
    QCOMPARE(trashed().size(), 2);
    TextFile::forgetOldVersion(again);
    QCOMPARE(trashed().size(), 1);
    QVERIFY(!QFile::exists(Trash::infoPathFor(again)));
    QCOMPARE(read(path), QByteArray("other words\n"));
    // It takes back only something in a Trash.
    TextFile::forgetOldVersion(path);
    QVERIFY(QFile::exists(path));
}

void TestTextFile::anotherProgramsChangeIsNoticed()
{
    const QString path = make("shared.txt", "mine\n");
    TextFile::Loaded file = TextFile::load(path);
    QVERIFY(!TextFile::changedOnDisk(file));
    QTest::qWait(20);
    make("shared.txt", "theirs, longer\n");
    QVERIFY(TextFile::changedOnDisk(file));
    QFile::remove(path);
    QVERIFY(TextFile::changedOnDisk(file));
}

void TestTextFile::aLinkEditsItsTarget()
{
    const QString target = make("real.txt", "real\n");
    const QString link = m_dir + "/link.txt";
    QVERIFY(QFile::link(target, link));
    TextFile::Loaded file = TextFile::load(link);
    QCOMPARE(file.path, target);
    QVERIFY(TextFile::save(&file, "changed\n", true).ok);
    QCOMPARE(read(target), QByteArray("changed\n"));
    QVERIFY(QFileInfo(link).isSymLink()); // the link is still a link
}

int main(int argc, char **argv)
{
    TestSupport::Sandbox sandbox;
    QCoreApplication app(argc, argv);
    TestTextFile test(sandbox.path());
    return QTest::qExec(&test, argc, argv);
}

#include "tst_textfile.moc"
