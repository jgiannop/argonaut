// Tests for the file operations engine (FileJob / FileOperations).
//
// Everything runs inside a temporary sandbox: HOME and XDG_DATA_HOME point
// into it, so "move to trash" uses a throwaway trash, never the real one.

#include "FileOperations.h"
#include "Trash.h"

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

#include <QLocalServer>

#include <sys/stat.h>
#include <unistd.h>

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
    void safeWritesDetection();
    void restoreFromTrash();
    void restoreRecreatesMissingFolder();
    void deletePermanently();
    void emptyTrash();
    void trashInfoOnOtherDrives();
    void conflictReplace();
    void conflictSkipAndKeepBothNamed();
    void conflictApplyToAll();
    void conflictMergeFolders();
    void conflictCancel();
    void sameFolderCopyNeverAsks();
    void safeWritesCopyAndMove();
    void redoCopy();
    void redoMove();
    void redoTrash();
    void redoRenameAndNewFolder();
    void newActionClearsRedo();
    void driveTrashRejectsLinks();
    void copyKeepsRelativeLinks();
    void copyFolderWithPipeAndSocket();
    void unreadableFolderIsAnError();
    void replaceWithItemFromInside();

private:
    Outcome waitForJob();
    // Answer conflict questions in order; returns how many were asked.
    void answerConflicts(QList<FileJob::Resolution> answers);
    int m_asked = 0;

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

void TestFileOperations::answerConflicts(QList<FileJob::Resolution> answers)
{
    m_asked = 0;
    m_ops->setAskOnConflict(true);
    connect(m_ops, &FileOperations::conflictRaised, this,
            [this, answers](FileJob *job, const QString &, const QString &) mutable {
                ++m_asked;
                job->resolveConflict(answers.isEmpty() ? FileJob::Resolution{} : answers.takeFirst());
            });
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
    QTemporaryDir other(QStringLiteral("/dev/shm/argonaut-test-XXXXXX"));
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
    // Direct: cancel on the worker thread, mid-copy. A queued cancel could
    // arrive after a fast disk (CI) had already copied the whole file.
    connect(job, &FileJob::progressChanged, job, [job] {
        if (job->progress().bytesDone > (20 << 20))
            job->cancel();
    }, Qt::DirectConnection);
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

void TestFileOperations::safeWritesDetection()
{
    // The forced answer is what tests and users can rely on everywhere.
    qputenv("ARGONAUT_SAFE_WRITES", "1");
    QVERIFY(needsSafeWrites(m_dir));
    qputenv("ARGONAUT_SAFE_WRITES", "0");
    QVERIFY(!needsSafeWrites(m_dir));
    qunsetenv("ARGONAUT_SAFE_WRITES");
#if defined(Q_OS_LINUX)
    // RAM-backed /dev/shm is never a removable drive.
    if (QFileInfo(QStringLiteral("/dev/shm")).isDir())
        QVERIFY(!needsSafeWrites(QStringLiteral("/dev/shm")));
#endif
}

// With safe writes on (as for a USB stick), copies and moves must still be
// correct, go through the "writing to the drive" phase, and report the drive.
void TestFileOperations::safeWritesCopyAndMove()
{
    qputenv("ARGONAUT_SAFE_WRITES", "1");
    writeFile(m_dir + "/src/big.bin", 70 << 20); // > 2 flush intervals
    writeFile(m_dir + "/src/small.txt", 100);
    QDir().mkpath(m_dir + "/stick");

    FileJob *job = m_ops->copy({m_dir + "/src/big.bin", m_dir + "/src/small.txt"},
                               m_dir + "/stick");
    bool sawFinishing = false;
    QStringList drives;
    connect(job, &FileJob::progressChanged, job, [&, job] {
        sawFinishing = sawFinishing || job->progress().finishing;
    });
    connect(m_ops, &FileOperations::jobFinished, this,
            [&](FileJob *j) { drives = j->flushedDrives(); }, Qt::SingleShotConnection);
    Outcome o = waitForJob();

    QVERIFY2(o.errors.isEmpty(), qPrintable(o.errors.join('\n')));
    QVERIFY(sameContent(m_dir + "/src/big.bin", m_dir + "/stick/big.bin"));
    QVERIFY(sameContent(m_dir + "/src/small.txt", m_dir + "/stick/small.txt"));
    QVERIFY(sawFinishing);
    QCOMPARE(drives.size(), 1);

    // Same-filesystem move still just renames, then flushes.
    m_ops->move({m_dir + "/stick/small.txt"}, m_dir + "/src");
    o = waitForJob();
    QVERIFY(o.errors.isEmpty());
    QVERIFY(exists(m_dir + "/src/small (copy).txt")); // src already had small.txt
    QVERIFY(!exists(m_dir + "/stick/small.txt"));
    qunsetenv("ARGONAUT_SAFE_WRITES");
}

void TestFileOperations::restoreFromTrash()
{
    writeFile(m_dir + "/docs/report.txt", 100);
    QString inTrash;
    QVERIFY(QFile::moveToTrash(m_dir + "/docs/report.txt", &inTrash));
    QVERIFY(Trash::filesDirOf(inTrash) == Trash::homeFilesDir());
    const Trash::Info info = Trash::info(inTrash);
    QVERIFY(info.valid);
    QCOMPARE(info.originalPath, m_dir + "/docs/report.txt");
    QVERIFY(info.deletionDate.isValid());

    // Something new now has the original name: restoring must not overwrite it.
    writeFile(m_dir + "/docs/report.txt", 7);
    m_ops->restore({inTrash});
    const Outcome o = waitForJob();
    QVERIFY2(o.errors.isEmpty(), qPrintable(o.errors.join('\n')));
    QVERIFY(o.userAction);
    QCOMPARE(QFileInfo(m_dir + "/docs/report.txt").size(), 7);
    QCOMPARE(QFileInfo(m_dir + "/docs/report (copy).txt").size(), 100);
    QVERIFY(!exists(inTrash));
    QVERIFY(!QFile::exists(Trash::infoPathFor(inTrash))); // record removed too
}

void TestFileOperations::restoreRecreatesMissingFolder()
{
    writeFile(m_dir + "/gone/deep/file.txt", 10);
    QString inTrash;
    QVERIFY(QFile::moveToTrash(m_dir + "/gone/deep/file.txt", &inTrash));
    QVERIFY(QDir(m_dir + "/gone").removeRecursively());
    m_ops->restore({inTrash});
    const Outcome o = waitForJob();
    QVERIFY2(o.errors.isEmpty(), qPrintable(o.errors.join('\n')));
    QCOMPARE(QFileInfo(m_dir + "/gone/deep/file.txt").size(), 10);
}

void TestFileOperations::deletePermanently()
{
    writeFile(m_dir + "/a.txt", 10);
    writeFile(m_dir + "/folder/sub/b.txt", 10);
    m_ops->deletePermanently({m_dir + "/a.txt", m_dir + "/folder"});
    const Outcome o = waitForJob();
    QVERIFY(o.errors.isEmpty());
    QCOMPARE(o.kind, FileJob::Kind::Delete);
    QVERIFY(!exists(m_dir + "/a.txt"));
    QVERIFY(!exists(m_dir + "/folder"));
    QVERIFY(!m_ops->canUndo()); // there is no undo for this

    // Deleting an item that's in the Trash also drops its record.
    writeFile(m_dir + "/c.txt", 10);
    QString inTrash;
    QVERIFY(QFile::moveToTrash(m_dir + "/c.txt", &inTrash));
    const QString record = Trash::infoPathFor(inTrash);
    QVERIFY(QFile::exists(record));
    m_ops->deletePermanently({inTrash});
    waitForJob();
    QVERIFY(!exists(inTrash));
    QVERIFY(!QFile::exists(record));
}

void TestFileOperations::emptyTrash()
{
    // Safety: the only trash this test may ever see is the sandbox's.
    const QStringList dirs = Trash::filesDirs();
    QCOMPARE(dirs.size(), 1);
    QVERIFY2(dirs.first().startsWith(m_sandbox), qPrintable(dirs.first()));

    writeFile(m_dir + "/x.txt", 10);
    writeFile(m_dir + "/y/z.txt", 10);
    QVERIFY(QFile::moveToTrash(m_dir + "/x.txt"));
    QVERIFY(QFile::moveToTrash(m_dir + "/y"));
    // A stray record whose item is already gone.
    const QString infoDir = QFileInfo(Trash::homeFilesDir()).absolutePath() + "/info";
    writeFile(infoDir + "/orphan.trashinfo", 5);

    m_ops->emptyTrash();
    const Outcome o = waitForJob();
    QVERIFY2(o.errors.isEmpty(), qPrintable(o.errors.join('\n')));
    QVERIFY(QDir(Trash::homeFilesDir()).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden));
    QVERIFY(QDir(infoDir).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden));
}

// Drives other than home have their own ".Trash-<uid>" with paths stored
// relative to the drive's top folder. (Only reads files the test creates.)
void TestFileOperations::trashInfoOnOtherDrives()
{
    const QString uid = QString::number(::getuid());
    const QString files = m_dir + "/drive/.Trash-" + uid + "/files";
    writeFile(files + "/photo.jpg", 10);
    QDir().mkpath(m_dir + "/drive/.Trash-" + uid + "/info");
    {
        QFile f(m_dir + "/drive/.Trash-" + uid + "/info/photo.jpg.trashinfo");
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
        f.write("[Trash Info]\nPath=Pictures/My%20photo.jpg\nDeletionDate=2024-05-06T07:08:09\n");
    }
    QVERIFY(Trash::isFilesDir(files));
    QCOMPARE(Trash::filesDirOf(files + "/photo.jpg"), files);
    const Trash::Info info = Trash::info(files + "/photo.jpg");
    QVERIFY(info.valid);
    QCOMPARE(info.originalPath, m_dir + "/drive/Pictures/My photo.jpg");
    QCOMPARE(info.deletionDate, QDateTime(QDate(2024, 5, 6), QTime(7, 8, 9)));
    QVERIFY(!Trash::isFilesDir(m_dir + "/drive/files"));
    QVERIFY(Trash::filesDirOf(m_dir + "/drive/other").isEmpty());
}

using Action = FileJob::ConflictAction;

void TestFileOperations::conflictReplace()
{
    writeFile(m_dir + "/src/a.txt", 100);
    writeFile(m_dir + "/dest/a.txt", 7);
    answerConflicts({{Action::Replace, false, {}}});
    m_ops->copy({m_dir + "/src/a.txt"}, m_dir + "/dest");
    const Outcome o = waitForJob();
    QCOMPARE(m_asked, 1);
    QVERIFY(o.errors.isEmpty());
    QVERIFY(sameContent(m_dir + "/src/a.txt", m_dir + "/dest/a.txt"));
    QVERIFY(!exists(m_dir + "/dest/a (copy).txt"));
    // The replaced file is in the (sandboxed) Trash, not gone.
    const QStringList trashed = QDir(Trash::homeFilesDir()).entryList(QDir::Files);
    QVERIFY2(trashed.contains("a.txt"), qPrintable(trashed.join(',')));
    QCOMPARE(QFileInfo(Trash::homeFilesDir() + "/a.txt").size(), 7);
}

void TestFileOperations::conflictSkipAndKeepBothNamed()
{
    writeFile(m_dir + "/src/a.txt", 100);
    writeFile(m_dir + "/src/b.txt", 100);
    writeFile(m_dir + "/dest/a.txt", 7);
    writeFile(m_dir + "/dest/b.txt", 7);
    answerConflicts({{Action::Skip, false, {}}, {Action::KeepBoth, false, "b-new.txt"}});
    m_ops->copy({m_dir + "/src/a.txt", m_dir + "/src/b.txt"}, m_dir + "/dest");
    const Outcome o = waitForJob();
    QCOMPARE(m_asked, 2);
    QCOMPARE(QFileInfo(m_dir + "/dest/a.txt").size(), 7); // skipped: untouched
    QCOMPARE(QFileInfo(m_dir + "/dest/b.txt").size(), 7);
    QCOMPARE(QFileInfo(m_dir + "/dest/b-new.txt").size(), 100);
    QCOMPARE(o.results.size(), 1); // only what was created can be undone
}

void TestFileOperations::conflictApplyToAll()
{
    for (const char *n : {"1.txt", "2.txt", "3.txt"}) {
        writeFile(m_dir + "/src/" + n, 100);
        writeFile(m_dir + "/dest/" + n, 7);
    }
    answerConflicts({{Action::Skip, true, {}}});
    m_ops->copy({m_dir + "/src/1.txt", m_dir + "/src/2.txt", m_dir + "/src/3.txt"}, m_dir + "/dest");
    waitForJob();
    QCOMPARE(m_asked, 1); // asked once, applied to the rest
    for (const char *n : {"1.txt", "2.txt", "3.txt"})
        QCOMPARE(QFileInfo(m_dir + "/dest/" + n).size(), 7);
}

void TestFileOperations::conflictMergeFolders()
{
    writeFile(m_dir + "/src/Photos/new.jpg", 50);
    writeFile(m_dir + "/src/Photos/same.jpg", 50);
    writeFile(m_dir + "/src/Photos/sub/deep.jpg", 50);
    writeFile(m_dir + "/dest/Photos/old.jpg", 5);
    writeFile(m_dir + "/dest/Photos/same.jpg", 5);
    // Merge the folder, then for the clashing file inside: keep both.
    answerConflicts({{Action::Merge, false, {}}, {Action::KeepBoth, false, {}}});
    m_ops->move({m_dir + "/src/Photos"}, m_dir + "/dest");
    const Outcome o = waitForJob();
    QCOMPARE(m_asked, 2);
    QVERIFY2(o.errors.isEmpty(), qPrintable(o.errors.join('\n')));
    QCOMPARE(QFileInfo(m_dir + "/dest/Photos/old.jpg").size(), 5);   // kept
    QCOMPARE(QFileInfo(m_dir + "/dest/Photos/same.jpg").size(), 5);  // kept
    QCOMPARE(QFileInfo(m_dir + "/dest/Photos/same (copy).jpg").size(), 50);
    QCOMPARE(QFileInfo(m_dir + "/dest/Photos/new.jpg").size(), 50);
    QCOMPARE(QFileInfo(m_dir + "/dest/Photos/sub/deep.jpg").size(), 50);
    QVERIFY(!exists(m_dir + "/src/Photos")); // moved out entirely

    // Undo puts back only what was moved; the folder's old contents stay.
    m_ops->undo();
    waitForJob();
    QCOMPARE(QFileInfo(m_dir + "/src/Photos/new.jpg").size(), 50);
    QCOMPARE(QFileInfo(m_dir + "/src/Photos/same.jpg").size(), 50);
    QCOMPARE(QFileInfo(m_dir + "/src/Photos/sub/deep.jpg").size(), 50);
    QCOMPARE(QFileInfo(m_dir + "/dest/Photos/old.jpg").size(), 5);
    QCOMPARE(QFileInfo(m_dir + "/dest/Photos/same.jpg").size(), 5);
}

void TestFileOperations::conflictCancel()
{
    writeFile(m_dir + "/src/a.txt", 100);
    writeFile(m_dir + "/src/b.txt", 100);
    writeFile(m_dir + "/dest/a.txt", 7);
    answerConflicts({{Action::Cancel, false, {}}});
    m_ops->copy({m_dir + "/src/a.txt", m_dir + "/src/b.txt"}, m_dir + "/dest");
    const Outcome o = waitForJob();
    QVERIFY(o.canceled);
    QCOMPARE(QFileInfo(m_dir + "/dest/a.txt").size(), 7);
    QVERIFY(!exists(m_dir + "/dest/b.txt")); // stopped before it
}

void TestFileOperations::sameFolderCopyNeverAsks()
{
    writeFile(m_dir + "/a.txt", 10);
    answerConflicts({{Action::Cancel, false, {}}});
    m_ops->copy({m_dir + "/a.txt"}, m_dir);
    waitForJob();
    QCOMPARE(m_asked, 0);
    QVERIFY(exists(m_dir + "/a (copy).txt"));
}

void TestFileOperations::redoCopy()
{
    writeFile(m_dir + "/src/a.txt", 50);
    QDir().mkpath(m_dir + "/dest");
    m_ops->copy({m_dir + "/src/a.txt"}, m_dir + "/dest");
    waitForJob();
    QVERIFY(!m_ops->canRedo());
    m_ops->undo(); // the copy goes to the Trash
    waitForJob();
    QVERIFY(!exists(m_dir + "/dest/a.txt"));
    QVERIFY(m_ops->canRedo());
    QCOMPARE(m_ops->redoText(), QStringLiteral("Redo Copy (1 item)"));
    m_ops->redo(); // the very same copy comes back from the Trash
    const Outcome o = waitForJob();
    QCOMPARE(o.kind, FileJob::Kind::Restore);
    QVERIFY(exists(m_dir + "/dest/a.txt"));
    QVERIFY(sameContent(m_dir + "/src/a.txt", m_dir + "/dest/a.txt"));
    QVERIFY(!m_ops->canRedo());
    QCOMPARE(m_ops->undoText(), QStringLiteral("Undo Copy (1 item)"));
    m_ops->undo(); // and it can be undone again
    waitForJob();
    QVERIFY(!exists(m_dir + "/dest/a.txt"));
}

void TestFileOperations::redoMove()
{
    writeFile(m_dir + "/a.txt", 10);
    QDir().mkpath(m_dir + "/dest");
    m_ops->move({m_dir + "/a.txt"}, m_dir + "/dest");
    waitForJob();
    m_ops->undo();
    waitForJob();
    QVERIFY(exists(m_dir + "/a.txt") && !exists(m_dir + "/dest/a.txt"));
    QCOMPARE(m_ops->redoText(), QStringLiteral("Redo Move (1 item)"));
    m_ops->redo();
    waitForJob();
    QVERIFY(!exists(m_dir + "/a.txt") && exists(m_dir + "/dest/a.txt"));
    m_ops->undo();
    waitForJob();
    QVERIFY(exists(m_dir + "/a.txt"));
}

void TestFileOperations::redoTrash()
{
    writeFile(m_dir + "/a.txt", 10);
    m_ops->trash({m_dir + "/a.txt"});
    waitForJob();
    m_ops->undo(); // restored
    waitForJob();
    QVERIFY(exists(m_dir + "/a.txt"));
    QCOMPARE(m_ops->redoText(), QStringLiteral("Redo Move to Trash (1 item)"));
    m_ops->redo(); // trashed again (into the sandbox trash)
    const Outcome o = waitForJob();
    QCOMPARE(o.kind, FileJob::Kind::Trash);
    QVERIFY(!exists(m_dir + "/a.txt"));
    QVERIFY(o.results.first().target.startsWith(m_sandbox));
    m_ops->undo();
    waitForJob();
    QVERIFY(exists(m_dir + "/a.txt"));
}

void TestFileOperations::redoRenameAndNewFolder()
{
    writeFile(m_dir + "/old.txt", 10);
    QVERIFY(QFile::rename(m_dir + "/old.txt", m_dir + "/new.txt"));
    m_ops->recordUndo(FileJob::Kind::Move, {{m_dir + "/old.txt", m_dir + "/new.txt"}}, "Rename");
    QCOMPARE(m_ops->undoText(), QStringLiteral("Undo Rename"));
    m_ops->undo();
    waitForJob();
    QVERIFY(exists(m_dir + "/old.txt"));
    QCOMPARE(m_ops->redoText(), QStringLiteral("Redo Rename"));
    m_ops->redo();
    waitForJob();
    QVERIFY(exists(m_dir + "/new.txt") && !exists(m_dir + "/old.txt"));

    QVERIFY(QDir().mkdir(m_dir + "/New Folder"));
    m_ops->recordUndo(FileJob::Kind::Copy, {{QString(), m_dir + "/New Folder"}}, "New Folder");
    m_ops->undo();
    waitForJob();
    QVERIFY(!exists(m_dir + "/New Folder"));
    QCOMPARE(m_ops->redoText(), QStringLiteral("Redo New Folder"));
    m_ops->redo();
    waitForJob();
    QVERIFY(QFileInfo(m_dir + "/New Folder").isDir());
}

void TestFileOperations::newActionClearsRedo()
{
    writeFile(m_dir + "/a.txt", 10);
    writeFile(m_dir + "/b.txt", 10);
    m_ops->trash({m_dir + "/a.txt"});
    waitForJob();
    m_ops->undo();
    waitForJob();
    QVERIFY(m_ops->canRedo());
    m_ops->trash({m_dir + "/b.txt"}); // something new
    waitForJob();
    QVERIFY(!m_ops->canRedo());
    m_ops->redo(); // nothing happens
    QTest::qWait(200);
    QVERIFY(exists(m_dir + "/a.txt"));
}

// A drive's trash folder that is (or goes through) a symlink must never be
// used: Empty Trash would delete what the link points to.
void TestFileOperations::driveTrashRejectsLinks()
{
    const QString uid = QString::number(::getuid());
    writeFile(m_dir + "/victim/thesis.odt", 10);

    // 1. A proper .Trash-<uid>/files is accepted.
    const QString good = m_dir + "/good";
    QVERIFY(QDir().mkpath(good + "/.Trash-" + uid + "/files"));
    QCOMPARE(Trash::filesDirsOn(good), QStringList{good + "/.Trash-" + uid + "/files"});

    // 2. "files" is a link to someone's folder.
    const QString a = m_dir + "/a";
    QVERIFY(QDir().mkpath(a + "/.Trash-" + uid));
    QVERIFY(QFile::link(m_dir + "/victim", a + "/.Trash-" + uid + "/files"));
    QVERIFY(Trash::filesDirsOn(a).isEmpty());

    // 3. ".Trash-<uid>" itself is a link.
    const QString b = m_dir + "/b";
    QVERIFY(QDir().mkpath(b));
    QVERIFY(QDir().mkpath(m_dir + "/elsewhere/files"));
    QVERIFY(QFile::link(m_dir + "/elsewhere", b + "/.Trash-" + uid));
    QVERIFY(Trash::filesDirsOn(b).isEmpty());

    // 4. The shared ".Trash/<uid>": only with the sticky bit, and not a link.
    const QString c = m_dir + "/c";
    QVERIFY(QDir().mkpath(c + "/.Trash/" + uid + "/files"));
    QVERIFY(Trash::filesDirsOn(c).isEmpty()); // no sticky bit
    QCOMPARE(::chmod(QFile::encodeName(c + "/.Trash").constData(), 01777), 0);
    QCOMPARE(Trash::filesDirsOn(c), QStringList{c + "/.Trash/" + uid + "/files"});
    const QString d = m_dir + "/d";
    QVERIFY(QDir().mkpath(d));
    QVERIFY(QFile::link(c + "/.Trash", d + "/.Trash"));
    QVERIFY(Trash::filesDirsOn(d).isEmpty());

    QVERIFY(exists(m_dir + "/victim/thesis.odt"));
}

void TestFileOperations::copyKeepsRelativeLinks()
{
    writeFile(m_dir + "/proj/lib/tool.sh", 10);
    QVERIFY(QDir().mkpath(m_dir + "/proj/bin"));
    QCOMPARE(::symlink("../lib/tool.sh", QFile::encodeName(m_dir + "/proj/bin/tool").constData()), 0);
    QCOMPARE(::symlink("/etc/hostname", QFile::encodeName(m_dir + "/proj/abs").constData()), 0);
    QCOMPARE(::symlink("nowhere", QFile::encodeName(m_dir + "/proj/broken").constData()), 0);
    QVERIFY(QDir().mkpath(m_dir + "/dest"));

    m_ops->copy({m_dir + "/proj"}, m_dir + "/dest");
    const Outcome o = waitForJob();
    QVERIFY2(o.errors.isEmpty(), qPrintable(o.errors.join('\n')));
    auto text = [](const QString &link) {
        char buffer[512];
        const ssize_t n = ::readlink(QFile::encodeName(link).constData(), buffer, sizeof buffer);
        return n < 0 ? QByteArray("<not a link>") : QByteArray(buffer, n);
    };
    // The same text, so the copy's link points inside the copy.
    QCOMPARE(text(m_dir + "/dest/proj/bin/tool"), QByteArray("../lib/tool.sh"));
    QCOMPARE(QFileInfo(m_dir + "/dest/proj/bin/tool").canonicalFilePath(),
             QFileInfo(m_dir + "/dest/proj/lib/tool.sh").canonicalFilePath());
    QCOMPARE(text(m_dir + "/dest/proj/abs"), QByteArray("/etc/hostname"));
    QCOMPARE(text(m_dir + "/dest/proj/broken"), QByteArray("nowhere"));
}

// Opening a pipe blocks until someone writes to it: the copy used to hang,
// and Cancel couldn't stop it.
void TestFileOperations::copyFolderWithPipeAndSocket()
{
    writeFile(m_dir + "/run/data.txt", 10);
    QCOMPARE(::mkfifo(QFile::encodeName(m_dir + "/run/pipe").constData(), 0640), 0);
    QLocalServer server;
    QVERIFY(server.listen(m_dir + "/run/sock"));
    QVERIFY(QDir().mkpath(m_dir + "/dest"));

    m_ops->copy({m_dir + "/run"}, m_dir + "/dest");
    const Outcome o = waitForJob();
    QVERIFY(exists(m_dir + "/dest/run/data.txt"));
    struct stat st;
    QCOMPARE(::lstat(QFile::encodeName(m_dir + "/dest/run/pipe").constData(), &st), 0);
    QVERIFY(S_ISFIFO(st.st_mode)); // a new pipe, same permissions
    QCOMPARE(int(st.st_mode & 0777), 0640);
    QVERIFY(!exists(m_dir + "/dest/run/sock")); // left out, and said so
    QCOMPARE(o.errors.size(), 1);
    QVERIFY2(o.errors.first().contains("sock"), qPrintable(o.errors.first()));
    server.close();
}

void TestFileOperations::unreadableFolderIsAnError()
{
    if (::getuid() == 0)
        QSKIP("root can read everything");
    writeFile(m_dir + "/src/open/a.txt", 10);
    writeFile(m_dir + "/src/locked/secret.txt", 10);
    QVERIFY(QDir().mkpath(m_dir + "/dest"));
    QCOMPARE(::chmod(QFile::encodeName(m_dir + "/src/locked").constData(), 0), 0);

    m_ops->copy({m_dir + "/src"}, m_dir + "/dest");
    const Outcome o = waitForJob();
    ::chmod(QFile::encodeName(m_dir + "/src/locked").constData(), 0700);
    // Not a silent, incomplete copy that looks whole.
    QCOMPARE(o.errors.size(), 1);
    QVERIFY2(o.errors.first().contains("locked"), qPrintable(o.errors.first()));
    QVERIFY(!exists(m_dir + "/dest/src"));
    QVERIFY(o.results.isEmpty());
}

// Replace sends the existing item to the Trash first. If the item being
// copied lives inside it, that would trash the source too.
void TestFileOperations::replaceWithItemFromInside()
{
    writeFile(m_dir + "/box/box/inner.txt", 10);
    writeFile(m_dir + "/box/other.txt", 10);
    answerConflicts({{Action::Replace, false, {}}});
    m_ops->copy({m_dir + "/box/box"}, m_dir);
    const Outcome o = waitForJob();
    QCOMPARE(m_asked, 1);
    QCOMPARE(o.errors.size(), 1);
    QVERIFY(exists(m_dir + "/box/box/inner.txt")); // nothing moved to the Trash
    QVERIFY(exists(m_dir + "/box/other.txt"));
    QVERIFY(o.results.isEmpty());
}

int main(int argc, char **argv)
{
    QTemporaryDir sandbox;
    if (!sandbox.isValid())
        return 1;
    // Must happen before QApplication / QStandardPaths are used.
    qputenv("HOME", QFile::encodeName(sandbox.path() + "/home"));
    qputenv("XDG_DATA_HOME", QFile::encodeName(sandbox.path() + "/data"));
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(sandbox.path() + "/config"));
    // Never look at (or empty) the trash folders of real mounted drives.
    qputenv("ARGONAUT_TRASH_HOME_ONLY", "1");
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QDir().mkpath(sandbox.path() + "/home");
    QDir().mkpath(sandbox.path() + "/data");

    QApplication app(argc, argv);
    TestFileOperations test(sandbox.path());
    return QTest::qExec(&test, argc, argv);
}

#include "tst_fileoperations.moc"
