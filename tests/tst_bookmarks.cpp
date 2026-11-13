// Tests for the GTK bookmarks file (shared with Nautilus). Runs against a
// sandboxed config folder: never the real ~/.config/gtk-3.0/bookmarks.

#include "Bookmarks.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

class TestBookmarks : public QObject
{
    Q_OBJECT

public:
    explicit TestBookmarks(QString sandbox) : m_sandbox(std::move(sandbox)) {}

private slots:
    void init();
    void fileIsSandboxed();
    void readsNautilusFormat();
    void addRemoveRename();
    void keepsLinesItDoesNotUse();

private:
    void writeFile(const QByteArray &content);
    QByteArray readFile() const;
    QString m_sandbox;
};

void TestBookmarks::init()
{
    QFile::remove(Bookmarks::filePath());
}

void TestBookmarks::writeFile(const QByteArray &content)
{
    QDir().mkpath(QFileInfo(Bookmarks::filePath()).absolutePath());
    QFile f(Bookmarks::filePath());
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(content);
}

QByteArray TestBookmarks::readFile() const
{
    QFile f(Bookmarks::filePath());
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void TestBookmarks::fileIsSandboxed()
{
    QVERIFY2(Bookmarks::filePath().startsWith(m_sandbox), qPrintable(Bookmarks::filePath()));
}

void TestBookmarks::readsNautilusFormat()
{
    writeFile("file:///home/me/Documents\n"
              "file:///home/me/Old%20Projects Work stuff\n"
              "\n");
    const QList<Bookmarks::Bookmark> list = Bookmarks::load();
    QCOMPARE(list.size(), 2);
    QCOMPARE(list.at(0).path, QStringLiteral("/home/me/Documents"));
    QCOMPARE(list.at(0).displayName(), QStringLiteral("Documents"));
    QCOMPARE(list.at(1).path, QStringLiteral("/home/me/Old Projects"));
    QCOMPARE(list.at(1).label, QStringLiteral("Work stuff"));
    QCOMPARE(list.at(1).displayName(), QStringLiteral("Work stuff"));
    QVERIFY(Bookmarks::contains("/home/me/Old Projects/"));
}

void TestBookmarks::addRemoveRename()
{
    QVERIFY(Bookmarks::add("/data/Μουσική και ταινίες")); // non-ASCII, spaces
    QVERIFY(Bookmarks::add("/data/Μουσική και ταινίες")); // no duplicates
    QCOMPARE(Bookmarks::load().size(), 1);
    QVERIFY(readFile().startsWith("file:///data/%CE%9C")); // percent-encoded like GTK

    QVERIFY(Bookmarks::rename("/data/Μουσική και ταινίες", "Media"));
    QCOMPARE(Bookmarks::load().first().displayName(), QStringLiteral("Media"));

    QVERIFY(Bookmarks::remove("/data/Μουσική και ταινίες"));
    QVERIFY(Bookmarks::load().isEmpty());
    QVERIFY(!Bookmarks::remove("/not/there"));
}

void TestBookmarks::keepsLinesItDoesNotUse()
{
    writeFile("sftp://server/home/me Server\n"
              "file:///home/me/A\n"
              "smb://nas/share\n");
    QVERIFY(Bookmarks::add("/home/me/B"));
    QVERIFY(Bookmarks::remove("/home/me/A"));
    QCOMPARE(readFile(), QByteArray("sftp://server/home/me Server\n"
                                    "smb://nas/share\n"
                                    "file:///home/me/B\n"));
}

int main(int argc, char **argv)
{
    QTemporaryDir sandbox;
    if (!sandbox.isValid())
        return 1;
    // Before anything reads QStandardPaths.
    qputenv("HOME", QFile::encodeName(sandbox.path()));
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(sandbox.path() + "/config"));
    QCoreApplication app(argc, argv);
    TestBookmarks test(sandbox.path());
    return QTest::qExec(&test, argc, argv);
}

#include "tst_bookmarks.moc"
