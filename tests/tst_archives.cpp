// Tests for Compress / Extract (libarchive). Everything happens inside a
// temporary sandbox, including the trash used by undo.

#include "FileOperations.h"

#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <archive.h>
#include <archive_entry.h>

using Format = FileJob::ArchiveFormat;
Q_DECLARE_METATYPE(FileJob::ArchiveFormat)

namespace {

struct Outcome
{
    FileJob::Kind kind = FileJob::Kind::Copy;
    QList<TransferResult> results;
    QStringList errors;
    bool canceled = false;
};

void writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(data), qint64(data.size()));
}

QByteArray randomBytes(int size)
{
    QByteArray data(size, Qt::Uninitialized);
    QRandomGenerator::global()->fillRange(reinterpret_cast<quint32 *>(data.data()), size / 4);
    return data;
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("<missing>");
}

// Relative path -> content ("<dir>" for folders), for comparing trees.
QMap<QString, QByteArray> tree(const QString &root)
{
    QMap<QString, QByteArray> out;
    QDirIterator it(root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        out.insert(QDir(root).relativeFilePath(path),
                   it.fileInfo().isDir() ? QByteArray("<dir>") : readFile(path));
    }
    return out;
}

// A hand-made tar with whatever (malicious) entries we like.
struct Entry
{
    QByteArray path;
    QByteArray data;
    QByteArray symlink; // non-empty: a symlink to this
};
void writeTar(const QString &file, const QList<Entry> &entries)
{
    struct archive *a = archive_write_new();
    archive_write_set_format_pax_restricted(a);
    QCOMPARE(archive_write_open_filename(a, QFile::encodeName(file).constData()), ARCHIVE_OK);
    for (const Entry &e : entries) {
        struct archive_entry *entry = archive_entry_new();
        archive_entry_set_pathname(entry, e.path.constData());
        if (!e.symlink.isEmpty()) {
            archive_entry_set_filetype(entry, AE_IFLNK);
            archive_entry_set_symlink(entry, e.symlink.constData());
            archive_entry_set_perm(entry, 0777);
        } else {
            archive_entry_set_filetype(entry, AE_IFREG);
            archive_entry_set_perm(entry, 0644);
            archive_entry_set_size(entry, e.data.size());
        }
        archive_write_header(a, entry);
        if (e.symlink.isEmpty())
            archive_write_data(a, e.data.constData(), size_t(e.data.size()));
        archive_entry_free(entry);
    }
    archive_write_close(a);
    archive_write_free(a);
}

} // namespace

class TestArchives : public QObject
{
    Q_OBJECT

public:
    explicit TestArchives(QString sandbox) : m_sandbox(std::move(sandbox)) {}

private slots:
    void init();
    void cleanup();

    void roundTrip_data();
    void roundTrip();
    void severalItemsGoIntoAFolder();
    void extractNeverOverwrites();
    void maliciousEntriesStayInside();
    void symlinkEscapeBlocked();
    void damagedArchiveLeavesNothing();
    void cancelCompressLeavesNothing();
    void cancelExtractLeavesNothing();
    void mislabelledArchiveStillExtracts();
    void plainGzipFile();
    void undoExtractAndCompress();
    void archiveMimeTypes();

private:
    Outcome waitForJob();
    void makeSample(const QString &root); // a small tree with tricky names
    QString m_sandbox;
    QString m_dir;
    FileOperations *m_ops = nullptr;
};

void TestArchives::init()
{
    m_dir = m_sandbox + "/work/" + QTest::currentTestFunction();
    QDir(m_dir).removeRecursively();
    QVERIFY(QDir().mkpath(m_dir));
    m_ops = new FileOperations;
}

void TestArchives::cleanup()
{
    delete m_ops;
    m_ops = nullptr;
    QDir(m_dir).removeRecursively();
}

Outcome TestArchives::waitForJob()
{
    Outcome outcome;
    bool done = false;
    QEventLoop loop;
    connect(m_ops, &FileOperations::jobFinished, &loop, [&](FileJob *job) {
        outcome.kind = job->kind();
        outcome.results = job->results();
        outcome.errors = job->errors();
        outcome.canceled = job->isCanceled();
        done = true;
        loop.quit();
    });
    QTimer::singleShot(120000, &loop, &QEventLoop::quit);
    loop.exec();
    if (!done)
        qWarning("timed out waiting for a file job");
    return outcome;
}

void TestArchives::makeSample(const QString &root)
{
    writeFile(root + "/readme.txt", "hello\n");
    writeFile(root + "/Φωτογραφίες/θάλασσα.jpg", randomBytes(300 * 1024));
    writeFile(root + "/deep/er/still/file name with spaces.bin", randomBytes(4096));
    writeFile(root + "/.hidden", "secret");
    QVERIFY(QDir().mkpath(root + "/empty folder"));
}

void TestArchives::roundTrip_data()
{
    QTest::addColumn<Format>("format");
    QTest::newRow("zip") << Format::Zip;
    QTest::newRow("tar.gz") << Format::TarGz;
    QTest::newRow("tar.xz") << Format::TarXz;
    QTest::newRow("7z") << Format::SevenZip;
}

void TestArchives::roundTrip()
{
    QFETCH(Format, format);
    makeSample(m_dir + "/src/Project");
    const QString name = "Project" + FileJob::archiveSuffix(format);

    m_ops->compress({m_dir + "/src/Project"}, m_dir, name, format);
    Outcome o = waitForJob();
    QVERIFY2(o.errors.isEmpty(), qPrintable(o.errors.join('\n')));
    QCOMPARE(o.results.size(), 1);
    QCOMPARE(o.results.first().target, m_dir + "/" + name);
    QVERIFY(QFileInfo(m_dir + "/" + name).size() > 0);
    QCOMPARE(QDir(m_dir).entryList(QDir::Hidden | QDir::Files).size(), 1); // no .part left

    QDir().mkpath(m_dir + "/out");
    m_ops->extract({m_dir + "/" + name}, m_dir + "/out");
    o = waitForJob();
    QVERIFY2(o.errors.isEmpty(), qPrintable(o.errors.join('\n')));
    // One top-level folder: it comes out as is, not wrapped in another.
    QCOMPARE(o.results.first().target, m_dir + "/out/Project");
    QCOMPARE(tree(m_dir + "/out/Project"), tree(m_dir + "/src/Project"));
    QCOMPARE(QDir(m_dir + "/out").entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden),
             QStringList{"Project"}); // no temporary folder left
}

void TestArchives::severalItemsGoIntoAFolder()
{
    writeFile(m_dir + "/src/a.txt", "a");
    writeFile(m_dir + "/src/b.txt", "b");
    m_ops->compress({m_dir + "/src/a.txt", m_dir + "/src/b.txt"}, m_dir, "Archive.zip", Format::Zip);
    QVERIFY(waitForJob().errors.isEmpty());

    m_ops->extract({m_dir + "/Archive.zip"}, m_dir);
    const Outcome o = waitForJob();
    QVERIFY2(o.errors.isEmpty(), qPrintable(o.errors.join('\n')));
    QCOMPARE(o.results.first().target, m_dir + "/Archive");
    QCOMPARE(readFile(m_dir + "/Archive/a.txt"), QByteArray("a"));
    QCOMPARE(readFile(m_dir + "/Archive/b.txt"), QByteArray("b"));
}

void TestArchives::extractNeverOverwrites()
{
    writeFile(m_dir + "/src/notes.txt", "from the archive");
    m_ops->compress({m_dir + "/src/notes.txt"}, m_dir, "notes.tar.gz", Format::TarGz);
    QVERIFY(waitForJob().errors.isEmpty());
    writeFile(m_dir + "/notes.txt", "mine, don't touch");

    m_ops->extract({m_dir + "/notes.tar.gz"}, m_dir);
    const Outcome o = waitForJob();
    QVERIFY(o.errors.isEmpty());
    QCOMPARE(readFile(m_dir + "/notes.txt"), QByteArray("mine, don't touch"));
    QVERIFY(o.results.first().target != m_dir + "/notes.txt");
    QCOMPARE(readFile(o.results.first().target), QByteArray("from the archive"));

    // Compressing again under a taken name gets a new name too.
    m_ops->compress({m_dir + "/src/notes.txt"}, m_dir, "notes.tar.gz", Format::TarGz);
    const Outcome again = waitForJob();
    QVERIFY(again.results.first().target != m_dir + "/notes.tar.gz");
}

void TestArchives::maliciousEntriesStayInside()
{
    const QString outside = m_dir + "/outside.txt";
    const QString absolute = m_dir + "/absolute.txt";
    writeTar(m_dir + "/evil.tar", {
                                      {"good.txt", "fine", {}},
                                      {"../outside.txt", "escaped!", {}},
                                      {"sub/../../outside.txt", "escaped!", {}},
                                      {QFile::encodeName(absolute), "escaped!", {}},
                                      {"other.txt", "fine too", {}},
                                  });
    QDir().mkpath(m_dir + "/out/deeper");
    m_ops->extract({m_dir + "/evil.tar"}, m_dir + "/out/deeper");
    const Outcome o = waitForJob();

    QVERIFY(!QFileInfo::exists(outside));
    QVERIFY(!QFileInfo::exists(m_dir + "/out/outside.txt"));
    QVERIFY(!QFileInfo::exists(absolute));
    QCOMPARE(o.errors.size(), 1); // one warning: 3 items skipped
    QVERIFY2(o.errors.first().contains("3"), qPrintable(o.errors.first()));
    QCOMPARE(readFile(m_dir + "/out/deeper/evil/good.txt"), QByteArray("fine"));
    QCOMPARE(readFile(m_dir + "/out/deeper/evil/other.txt"), QByteArray("fine too"));
}

void TestArchives::symlinkEscapeBlocked()
{
    QDir().mkpath(m_dir + "/victim");
    writeTar(m_dir + "/links.tar", {
                                       {"link", {}, QFile::encodeName(m_dir + "/victim")},
                                       {"link/planted.txt", "escaped through a symlink", {}},
                                   });
    QDir().mkpath(m_dir + "/out");
    m_ops->extract({m_dir + "/links.tar"}, m_dir + "/out");
    waitForJob();
    QVERIFY(!QFileInfo::exists(m_dir + "/victim/planted.txt"));
}

void TestArchives::damagedArchiveLeavesNothing()
{
    writeFile(m_dir + "/broken.zip", QByteArray("PK\x03\x04", 4) + randomBytes(64 * 1024));
    QDir().mkpath(m_dir + "/out");
    m_ops->extract({m_dir + "/broken.zip"}, m_dir + "/out");
    const Outcome o = waitForJob();
    QVERIFY(!o.errors.isEmpty());
    QVERIFY(o.results.isEmpty());
    QCOMPARE(QDir(m_dir + "/out").entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).size(), 0);
}

void TestArchives::cancelCompressLeavesNothing()
{
    writeFile(m_dir + "/src/big.bin", randomBytes(64 << 20));
    FileJob *job = m_ops->compress({m_dir + "/src/big.bin"}, m_dir, "big.zip", Format::Zip);
    connect(job, &FileJob::progressChanged, job, [job] {
        if (job->progress().bytesDone > 0)
            job->cancel();
    }, Qt::DirectConnection);
    const Outcome o = waitForJob();
    QVERIFY(o.canceled);
    QCOMPARE(QDir(m_dir).entryList(QDir::Files | QDir::Hidden).size(), 0);
}

void TestArchives::cancelExtractLeavesNothing()
{
    // Many small files: slow enough to extract that we cancel half-way.
    QList<Entry> entries;
    for (int i = 0; i < 20000; ++i)
        entries.append({"many/file" + QByteArray::number(i) + ".txt", "x", {}});
    writeTar(m_dir + "/many.tar", entries);
    QDir().mkpath(m_dir + "/out");
    FileJob *job = m_ops->extract({m_dir + "/many.tar"}, m_dir + "/out");
    // Direct: cancel on the worker thread at the first update, not later.
    connect(job, &FileJob::progressChanged, job, [job] {
        if (job->progress().bytesDone > 0)
            job->cancel();
    }, Qt::DirectConnection);
    const Outcome o = waitForJob();
    QVERIFY(o.canceled);
    QVERIFY(o.results.isEmpty()); // stopped before finishing
    QCOMPARE(QDir(m_dir + "/out").entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).size(), 0);
}

void TestArchives::mislabelledArchiveStillExtracts()
{
    writeFile(m_dir + "/src/game.dat", "level data");
    m_ops->compress({m_dir + "/src/game.dat"}, m_dir, "game.7z", Format::SevenZip);
    QVERIFY(waitForJob().errors.isEmpty());
    QVERIFY(QFile::rename(m_dir + "/game.7z", m_dir + "/game.zip")); // a 7z named .zip
    QDir().mkpath(m_dir + "/out");
    m_ops->extract({m_dir + "/game.zip"}, m_dir + "/out");
    const Outcome o = waitForJob();
    QVERIFY2(o.errors.isEmpty(), qPrintable(o.errors.join('\n')));
    QCOMPARE(readFile(m_dir + "/out/game.dat"), QByteArray("level data"));
}

void TestArchives::plainGzipFile()
{
    // A single compressed file (not a tar): comes out under the archive's name.
    const QByteArray text = "a log file\n" + randomBytes(1024).toHex();
    struct archive *a = archive_write_new();
    archive_write_set_format_raw(a);
    archive_write_add_filter_gzip(a);
    QCOMPARE(archive_write_open_filename(a, QFile::encodeName(m_dir + "/server.log.gz").constData()), ARCHIVE_OK);
    struct archive_entry *entry = archive_entry_new();
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_size(entry, text.size());
    archive_write_header(a, entry);
    archive_write_data(a, text.constData(), size_t(text.size()));
    archive_entry_free(entry);
    archive_write_close(a);
    archive_write_free(a);

    QDir().mkpath(m_dir + "/out");
    m_ops->extract({m_dir + "/server.log.gz"}, m_dir + "/out");
    const Outcome o = waitForJob();
    QVERIFY2(o.errors.isEmpty(), qPrintable(o.errors.join('\n')));
    QCOMPARE(readFile(m_dir + "/out/server.log"), text);
}

void TestArchives::undoExtractAndCompress()
{
    writeFile(m_dir + "/src/a.txt", "a");
    m_ops->compress({m_dir + "/src/a.txt"}, m_dir, "a.zip", Format::Zip);
    QVERIFY(waitForJob().errors.isEmpty());
    m_ops->extract({m_dir + "/a.zip"}, m_dir + "/src");
    const Outcome extracted = waitForJob();
    const QString copy = extracted.results.first().target; // "a (copy).txt"
    QVERIFY(QFileInfo::exists(copy));

    QCOMPARE(m_ops->undoText(), QStringLiteral("Undo Extract"));
    m_ops->undo();
    Outcome o = waitForJob();
    QCOMPARE(o.kind, FileJob::Kind::Trash);
    QVERIFY(!QFileInfo::exists(copy));
    QVERIFY(QFileInfo::exists(m_dir + "/src/a.txt")); // the original stays
    QVERIFY(QFileInfo::exists(o.results.first().target)); // in the (sandbox) trash
    QVERIFY(o.results.first().target.startsWith(m_sandbox));

    QCOMPARE(m_ops->undoText(), QStringLiteral("Undo Compress"));
    m_ops->undo();
    o = waitForJob();
    QVERIFY(!QFileInfo::exists(m_dir + "/a.zip"));
    QVERIFY(o.results.first().target.startsWith(m_sandbox));
}

void TestArchives::archiveMimeTypes()
{
    QVERIFY(FileOperations::isArchive("application/zip"));
    QVERIFY(FileOperations::isArchive("application/x-7z-compressed"));
    QVERIFY(FileOperations::isArchive("application/x-compressed-tar"));
    QVERIFY(!FileOperations::isArchive("application/pdf"));
    QVERIFY(!FileOperations::isArchive("application/vnd.openxmlformats-officedocument.wordprocessingml.document"));
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
    qputenv("ARGONAUT_TRASH_HOME_ONLY", "1"); // never touch real drives' trash
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QDir().mkpath(sandbox.path() + "/home");
    QDir().mkpath(sandbox.path() + "/data");

    QApplication app(argc, argv);
    TestArchives test(sandbox.path());
    return QTest::qExec(&test, argc, argv);
}

#include "tst_archives.moc"
