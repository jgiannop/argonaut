// Tests for the file operations engine (FileJob / FileOperations).
//
// Everything runs inside a temporary sandbox: HOME and XDG_DATA_HOME point
// into it, so "move to trash" uses a throwaway trash, never the real one.

#include "FileOperations.h"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

namespace {

struct Outcome
{
    FileJob::Kind kind = FileJob::Kind::Copy;
    QList<TransferResult> results;
    QStringList errors;
    bool canceled = false;
    bool userAction = true;
};

void writeFile(const QString &path, qint64 size, bool random = true)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QByteArray chunk(int(qMin<qint64>(size, 1 << 20)), 'k');
    qint64 left = size;
    while (left > 0) {
        if (random)
            QRandomGenerator::global()->fillRange(reinterpret_cast<quint32 *>(chunk.data()),
                                                  chunk.size() / 4);
        const qint64 n = qMin<qint64>(left, chunk.size());
        QCOMPARE(file.write(chunk.constData(), n), n);
        left -= n;
    }
}

bool sameContent(const QString &a, const QString &b)
{
    QFile x(a);
    QFile y(b);
    if (!x.open(QIODevice::ReadOnly) || !y.open(QIODevice::ReadOnly))
        return false;
    return x.size() == y.size() && x.readAll() == y.readAll();
}

bool exists(const QString &path)
{
    const QFileInfo info(path);
    return info.exists() || info.isSymLink();
}

} // namespace

class TestFileOperations : public QObject
{
    Q_OBJECT

public:
    explicit TestFileOperations(QString sandbox) : m_sandbox(std::move(sandbox)) {}

private slots:
    void init();
    void cleanup();

    void uniqueTargetNames();
    void copyNeverOverwrites();
    void copyFolderTree();
    void missingDestinationFails();
    void copyIntoSameFolder();
    void moveAndUndo();
    void moveAcrossFilesystemsAndUndo();
    void trashAndRestore();
    void folderIntoItselfFails();
    void cancelRemovesPartialCopy();
    void undoCopyTrashesCopies();
    void clipboardRoundTrip();

private:
    Outcome waitForJob();

    QString m_sandbox;
    QString m_dir; // per-test work folder
    FileOperations *m_ops = nullptr;
};

void TestFileOperations::init()
{
    m_dir = m_sandbox + "/work/" + QTest::currentTestFunction();
    QDir(m_dir).removeRecursively();
    QVERIFY(QDir().mkpath(m_dir));
    m_ops = new FileOperations;
}

void TestFileOperations::cleanup()
{
    delete m_ops;
    m_ops = nullptr;
    QDir(m_dir).removeRecursively();
}

// Runs the event loop until the next job finishes.
Outcome TestFileOperations::waitForJob()
{
    Outcome outcome;
    bool done = false;
    QEventLoop loop;
    connect(m_ops, &FileOperations::jobFinished, &loop, [&](FileJob *job) {
        outcome.kind = job->kind();
        outcome.results = job->results();
        outcome.errors = job->errors();
        outcome.canceled = job->isCanceled();
        outcome.userAction = job->property(FileOperations::UserActionProperty).toBool();
        done = true;
        loop.quit();
    });
    QTimer::singleShot(120000, &loop, &QEventLoop::quit);
    loop.exec();
    if (!done)
        qWarning("timed out waiting for a file job");
    return outcome;
}

void TestFileOperations::uniqueTargetNames()
{
    writeFile(m_dir + "/a.txt", 1);
    writeFile(m_dir + "/a (copy).txt", 1);
    writeFile(m_dir + "/x.tar.gz", 1);
    QDir().mkpath(m_dir + "/my.folder");

    QCOMPARE(uniqueTargetPath(m_dir, "new.txt", false), m_dir + "/new.txt");
    QCOMPARE(uniqueTargetPath(m_dir, "a.txt", false), m_dir + "/a (copy 2).txt");
    QCOMPARE(uniqueTargetPath(m_dir, "x.tar.gz", false), m_dir + "/x (copy).tar.gz");
    QCOMPARE(uniqueTargetPath(m_dir, "my.folder", true), m_dir + "/my.folder (copy)");
}

void TestFileOperations::copyNeverOverwrites()
{
    writeFile(m_dir + "/src/a.txt", 1000);
    writeFile(m_dir + "/dest/a.txt", 7);

    m_ops->copy({m_dir + "/src/a.txt"}, m_dir + "/dest");
    const Outcome o = waitForJob();

    QVERIFY(o.errors.isEmpty());
    QCOMPARE(QFileInfo(m_dir + "/dest/a.txt").size(), 7); // untouched
    QVERIFY(sameContent(m_dir + "/src/a.txt", m_dir + "/dest/a (copy).txt"));
}

void TestFileOperations::copyFolderTree()
{
    writeFile(m_dir + "/src/dir/sub/b.txt", 5000);
    writeFile(m_dir + "/src/dir/.hidden", 10);
    writeFile(m_dir + "/src/big.bin", 30 << 20);
    {
        QFile f(m_dir + "/src/big.bin");
        QVERIFY(f.open(QIODevice::ReadWrite));
        f.setFileTime(QDateTime(QDate(2020, 1, 2), QTime(3, 4, 5)),
                      QFileDevice::FileModificationTime);
    }
#if !defined(Q_OS_WIN)
    QVERIFY(QFile::link(m_dir + "/src/big.bin", m_dir + "/src/dir/link"));
#endif
    QDir().mkpath(m_dir + "/dest");

    m_ops->copy({m_dir + "/src/dir", m_dir + "/src/big.bin"}, m_dir + "/dest");
    const Outcome o = waitForJob();

    QVERIFY2(o.errors.isEmpty(), qPrintable(o.errors.join('\n')));
    QCOMPARE(o.results.size(), 2);
    QVERIFY(sameContent(m_dir + "/src/dir/sub/b.txt", m_dir + "/dest/dir/sub/b.txt"));
    QVERIFY(exists(m_dir + "/dest/dir/.hidden"));
    QVERIFY(sameContent(m_dir + "/src/big.bin", m_dir + "/dest/big.bin"));
    QCOMPARE(QFileInfo(m_dir + "/dest/big.bin").lastModified(),
             QFileInfo(m_dir + "/src/big.bin").lastModified());
#if !defined(Q_OS_WIN)
    QVERIFY(QFileInfo(m_dir + "/dest/dir/link").isSymLink());
#endif
}

void TestFileOperations::missingDestinationFails()
{
    writeFile(m_dir + "/a.txt", 10);

    m_ops->copy({m_dir + "/a.txt"}, m_dir + "/nowhere");
    const Outcome o = waitForJob();

    QCOMPARE(o.errors.size(), 1);
    QVERIFY(o.errors.first().contains("doesn't exist"));
    QVERIFY(!exists(m_dir + "/nowhere"));
}

void TestFileOperations::copyIntoSameFolder()
{
    writeFile(m_dir + "/a.txt", 10);

    m_ops->copy({m_dir + "/a.txt"}, m_dir);
    waitForJob();
    m_ops->copy({m_dir + "/a.txt"}, m_dir);
    waitForJob();

    QVERIFY(exists(m_dir + "/a (copy).txt"));
    QVERIFY(exists(m_dir + "/a (copy 2).txt"));
}

void TestFileOperations::moveAndUndo()
{
    writeFile(m_dir + "/src/file.bin", 4096);
    QDir().mkpath(m_dir + "/dest");

    m_ops->move({m_dir + "/src/file.bin"}, m_dir + "/dest");
    Outcome o = waitForJob();
    QVERIFY(o.errors.isEmpty());
    QVERIFY(!exists(m_dir + "/src/file.bin"));
    QVERIFY(exists(m_dir + "/dest/file.bin"));
    QCOMPARE(m_ops->undoText(), QStringLiteral("Undo Move (1 item)"));

    m_ops->undo();
    o = waitForJob();
    QVERIFY(!o.userAction);
    QVERIFY(exists(m_dir + "/src/file.bin"));
    QVERIFY(!exists(m_dir + "/dest/file.bin"));
    QVERIFY(!m_ops->canUndo());
}

void TestFileOperations::moveAcrossFilesystemsAndUndo()
{
#if defined(Q_OS_LINUX)
    if (!QFileInfo(QStringLiteral("/dev/shm")).isWritable())
        QSKIP("/dev/shm is not available");
    QTemporaryDir other(QStringLiteral("/dev/shm/kaftilous-test-XXXXXX"));
    QVERIFY(other.isValid());
    if (QStorageInfo(other.path()).device() == QStorageInfo(m_dir).device())
        QSKIP("the sandbox and /dev/shm are on the same filesystem");

    writeFile(m_dir + "/xfs/f.txt", 12345);
    m_ops->move({m_dir + "/xfs"}, other.path());
    Outcome o = waitForJob();
    QVERIFY(o.errors.isEmpty());
    QVERIFY(!exists(m_dir + "/xfs"));
    QCOMPARE(QFileInfo(other.path() + "/xfs/f.txt").size(), 12345);

    m_ops->undo();
    o = waitForJob();
    QCOMPARE(QFileInfo(m_dir + "/xfs/f.txt").size(), 12345);
    QVERIFY(!exists(other.path() + "/xfs"));
#else
    QSKIP("needs a second filesystem (Linux /dev/shm)");
#endif
}

void TestFileOperations::trashAndRestore()
{
    writeFile(m_dir + "/dir/sub/b.txt", 100);

    m_ops->trash({m_dir + "/dir"});
    Outcome o = waitForJob();
    QVERIFY(o.errors.isEmpty());
    QCOMPARE(o.results.size(), 1);
    const QString inTrash = o.results.first().target;
    QVERIFY(!exists(m_dir + "/dir"));
    QVERIFY(exists(inTrash));
    QVERIFY2(inTrash.startsWith(m_sandbox), "must use the sandboxed trash");

    m_ops->undo();
    o = waitForJob();
    QCOMPARE(o.kind, FileJob::Kind::Restore);
    QVERIFY(exists(m_dir + "/dir/sub/b.txt"));
    QVERIFY(!exists(inTrash));
#if defined(Q_OS_LINUX)
    const QFileInfo trashed(inTrash);
    QVERIFY(!QFile::exists(QDir::cleanPath(trashed.dir().absolutePath() + "/../info/"
                                           + trashed.fileName() + ".trashinfo")));
#endif
}

void TestFileOperations::folderIntoItselfFails()
{
    writeFile(m_dir + "/dir/sub/b.txt", 10);

    m_ops->copy({m_dir + "/dir"}, m_dir + "/dir/sub");
    Outcome o = waitForJob();
    QCOMPARE(o.errors.size(), 1);
    QVERIFY(!exists(m_dir + "/dir/sub/dir"));

    m_ops->move({m_dir + "/dir"}, m_dir + "/dir");
    o = waitForJob();
    QCOMPARE(o.errors.size(), 1);
    QVERIFY(exists(m_dir + "/dir/sub/b.txt"));
}

void TestFileOperations::cancelRemovesPartialCopy()
{
    writeFile(m_dir + "/huge.bin", 300 << 20, false);
    QDir().mkpath(m_dir + "/dest");

    FileJob *job = m_ops->copy({m_dir + "/huge.bin"}, m_dir + "/dest");
    connect(job, &FileJob::progressChanged, job, [job] {
        if (job->progress().bytesDone > (20 << 20))
            job->cancel();
    });
    const Outcome o = waitForJob();

    QVERIFY(o.canceled);
    QVERIFY(!exists(m_dir + "/dest/huge.bin"));
    QVERIFY(exists(m_dir + "/huge.bin"));
}

void TestFileOperations::undoCopyTrashesCopies()
{
    writeFile(m_dir + "/src/a.txt", 50);
    QDir().mkpath(m_dir + "/dest");

    m_ops->copy({m_dir + "/src/a.txt"}, m_dir + "/dest");
    waitForJob();
    QVERIFY(exists(m_dir + "/dest/a.txt"));

    m_ops->undo();
    const Outcome o = waitForJob();
    QCOMPARE(o.kind, FileJob::Kind::Trash);
    QVERIFY(!exists(m_dir + "/dest/a.txt"));
    QVERIFY(exists(m_dir + "/src/a.txt"));
    QVERIFY(exists(o.results.first().target)); // in the trash, not deleted
}

void TestFileOperations::clipboardRoundTrip()
{
    const QStringList files{m_dir + "/a.txt", m_dir + "/folder"};
    QStringList paths;
    bool cut = false;

    FileOperations::setClipboardFiles(files, true);
    QVERIFY(FileOperations::clipboardFiles(&paths, &cut));
    QVERIFY(cut);
    QCOMPARE(paths, files);

    FileOperations::setClipboardFiles(files, false);
    QVERIFY(FileOperations::clipboardFiles(&paths, &cut));
    QVERIFY(!cut);
}

int main(int argc, char **argv)
{
    QTemporaryDir sandbox;
    if (!sandbox.isValid())
        return 1;
    // Must happen before QApplication / QStandardPaths are used.
    qputenv("HOME", QFile::encodeName(sandbox.path() + "/home"));
    qputenv("XDG_DATA_HOME", QFile::encodeName(sandbox.path() + "/data"));
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QDir().mkpath(sandbox.path() + "/home");
    QDir().mkpath(sandbox.path() + "/data");

    QApplication app(argc, argv);
    TestFileOperations test(sandbox.path());
    return QTest::qExec(&test, argc, argv);
}

#include "tst_fileoperations.moc"
