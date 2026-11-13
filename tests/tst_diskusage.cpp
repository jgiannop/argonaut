// Tests for DiskUsageModel (the Analyze view): sizes agree with `du`,
// hard links count once, symlinks aren't followed, re-rooting, removal,
// sorting and starting over mid-scan.

#include "DiskUsageModel.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include <unistd.h>

namespace {

void writeFile(const QString &path, int size)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    QVERIFY(f.write(QByteArray(size, 'k')) == size);
}

// Space used on disk by `path`, according to du (one filesystem).
qint64 du(const QString &path)
{
    QProcess p;
    p.start("du", {"-sxB1", path});
    p.waitForFinished();
    return p.readAllStandardOutput().split('\t').first().toLongLong();
}

QStringList childNames(const DiskUsageModel &m, const QModelIndex &parent)
{
    QStringList out;
    for (int r = 0; r < m.rowCount(parent); ++r)
        out << m.index(r, 0, parent).data().toString();
    return out;
}

} // namespace

class TestDiskUsage : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void sizesMatchDu();
    void hardLinksAndSymlinks();
    void reRootWithoutScanning();
    void removeSubtractsSize();
    void sortByName();
    void newRootCancelsOldScan();
    void unreadableFolder();
    void filesShownAndAddUp();
    void removeKeepsOtherRows();
    void addPathMeasuresWhatArrived();

private:
    bool scan(const QString &path);
    void makeTree();
    QTemporaryDir *m_tmp = nullptr;
    QString m_dir;
    DiskUsageModel *m_model = nullptr;
};

void TestDiskUsage::init()
{
    m_tmp = new QTemporaryDir;
    m_dir = QDir::cleanPath(m_tmp->path());
    m_model = new DiskUsageModel;
}

void TestDiskUsage::cleanup()
{
    delete m_model;
    delete m_tmp;
}

bool TestDiskUsage::scan(const QString &path)
{
    QSignalSpy done(m_model, &DiskUsageModel::finished);
    m_model->setRoot(path);
    return done.count() > 0 || done.wait(30000);
}

void TestDiskUsage::makeTree()
{
    writeFile(m_dir + "/top.bin", 10000);
    writeFile(m_dir + "/Big/a.bin", 400000);
    writeFile(m_dir + "/Big/Deeper/b.bin", 300000);
    writeFile(m_dir + "/Big/Deeper/Deepest/c.bin", 5000);
    writeFile(m_dir + "/Small/x.txt", 100);
    QDir().mkpath(m_dir + "/Empty");
    for (int i = 0; i < 50; ++i)
        writeFile(m_dir + "/Many/f" + QString::number(i), 3000);
}

void TestDiskUsage::sizesMatchDu()
{
    if (QStandardPaths::findExecutable("du").isEmpty())
        QSKIP("no du");
    makeTree();
    QVERIFY(scan(m_dir));
    QVERIFY(!m_model->isScanning());
    const QModelIndex top = m_model->index(0, 0);
    QCOMPARE(m_model->rowCount(), 1);
    QCOMPARE(m_model->rootBytes(), du(m_dir));
    QCOMPARE(m_model->rootItems(), qint64(1 + 3 + 1 + 1 + 1 + 1 + 1 + 1 + 50 + 1)); // see makeTree
    // Biggest first; every folder agrees with du.
    QCOMPARE(childNames(*m_model, top), QStringList({"Big", "Many", "Small", "Empty"}));
    for (int r = 0; r < m_model->rowCount(top); ++r) {
        const QModelIndex child = m_model->index(r, 0, top);
        QCOMPARE(child.data(DiskUsageModel::BytesRole).toLongLong(), du(m_model->filePath(child)));
    }
    const QModelIndex big = m_model->indexForPath(m_dir + "/Big/Deeper");
    QVERIFY(big.isValid());
    QCOMPARE(big.data(DiskUsageModel::BytesRole).toLongLong(), du(m_dir + "/Big/Deeper"));
    const double share = big.data(DiskUsageModel::FractionRole).toDouble();
    QVERIFY(share > 0.3 && share < 0.6); // ~300k of ~700k
    QCOMPARE(m_model->index(0, DiskUsageModel::UsageColumn).data().toString(), QStringLiteral("100%"));
}

void TestDiskUsage::hardLinksAndSymlinks()
{
    if (QStandardPaths::findExecutable("du").isEmpty())
        QSKIP("no du");
    writeFile(m_dir + "/data/original.bin", 500000);
    QVERIFY(QProcess::execute("ln", {m_dir + "/data/original.bin", m_dir + "/data/hardlink.bin"}) == 0);
    QTemporaryDir outside;
    writeFile(outside.path() + "/huge.bin", 2000000);
    QVERIFY(QFile::link(outside.path(), m_dir + "/data/link-to-outside"));
    QVERIFY(QFile::link(m_dir, m_dir + "/data/loop"));
    QVERIFY(scan(m_dir));
    QCOMPARE(m_model->rootBytes(), du(m_dir)); // du also counts hard links once
    QVERIFY(m_model->rootBytes() < 1000000);    // not 2x the file, not the outside file
}

void TestDiskUsage::reRootWithoutScanning()
{
    makeTree();
    QVERIFY(scan(m_dir));
    QSignalSpy progress(m_model, &DiskUsageModel::progress);
    QSignalSpy done(m_model, &DiskUsageModel::finished);
    m_model->setRoot(m_dir + "/Big");
    QCOMPARE(done.count(), 1); // at once: no scan
    QVERIFY(!m_model->isScanning());
    QCOMPARE(m_model->rootPath(), m_dir + "/Big");
    QCOMPARE(m_model->index(0, 0).data().toString(), QStringLiteral("Big"));
    QCOMPARE(childNames(*m_model, m_model->index(0, 0)), QStringList({"Deeper"}));
    QVERIFY(!m_model->indexForPath(m_dir + "/Small").isValid()); // not under the shown folder

    m_model->setRoot(m_dir); // back up: still inside the scan
    QCOMPARE(done.count(), 2);
    QVERIFY(!m_model->isScanning());

    m_model->setRoot(QFileInfo(m_dir).absolutePath()); // above it: scans
    QVERIFY(m_model->isScanning() || done.count() == 3);
}

void TestDiskUsage::removeSubtractsSize()
{
    makeTree();
    QVERIFY(scan(m_dir));
    const qint64 before = m_model->rootBytes();
    const QModelIndex deeper = m_model->indexForPath(m_dir + "/Big/Deeper");
    const qint64 removed = deeper.data(DiskUsageModel::BytesRole).toLongLong();
    const qint64 bigBefore = m_model->indexForPath(m_dir + "/Big").data(DiskUsageModel::BytesRole).toLongLong();
    QVERIFY(m_model->removePath(m_dir + "/Big/Deeper"));
    QCOMPARE(m_model->rootBytes(), before - removed);
    QCOMPARE(m_model->indexForPath(m_dir + "/Big").data(DiskUsageModel::BytesRole).toLongLong(),
             bigBefore - removed);
    QVERIFY(!m_model->indexForPath(m_dir + "/Big/Deeper").isValid());
    QVERIFY(!m_model->removePath(m_dir + "/nonexistent"));
}

void TestDiskUsage::sortByName()
{
    makeTree();
    QVERIFY(scan(m_dir));
    const QPersistentModelIndex small = m_model->indexForPath(m_dir + "/Small");
    m_model->sort(DiskUsageModel::NameColumn, Qt::AscendingOrder);
    QCOMPARE(childNames(*m_model, m_model->index(0, 0)), QStringList({"Big", "Empty", "Many", "Small"}));
    QVERIFY(small.isValid());
    QCOMPARE(small.data().toString(), QStringLiteral("Small")); // followed the move
    QCOMPARE(small.row(), 3);
}

void TestDiskUsage::newRootCancelsOldScan()
{
    makeTree();
    QTemporaryDir other;
    writeFile(other.path() + "/only/one.bin", 1000);
    m_model->setRoot(m_dir); // not waited for
    QVERIFY(scan(QDir::cleanPath(other.path())));
    QCOMPARE(childNames(*m_model, m_model->index(0, 0)), QStringList({"only"}));
    QTest::qWait(200); // a late result from the first scan must not show up
    QCOMPARE(childNames(*m_model, m_model->index(0, 0)), QStringList({"only"}));
}

void TestDiskUsage::unreadableFolder()
{
    if (::geteuid() == 0)
        QSKIP("root can read everything");
    writeFile(m_dir + "/locked/secret.bin", 1000);
    QVERIFY(QFile::setPermissions(m_dir + "/locked", QFileDevice::Permissions()));
    QVERIFY(scan(m_dir));
    QVERIFY(m_model->rootIncomplete());
    QVERIFY(m_model->index(0, 0).data(Qt::ToolTipRole).toString().contains("couldn't be read"));
    QFile::setPermissions(m_dir + "/locked", QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
}

void TestDiskUsage::filesShownAndAddUp()
{
    for (int i = 0; i < 12; ++i)
        writeFile(m_dir + "/Media/big" + QString::number(i) + ".bin", 300000 + i * 10000);
    for (int i = 0; i < 5; ++i)
        writeFile(m_dir + "/Media/small" + QString::number(i) + ".txt", 100);
    writeFile(m_dir + "/Media/Sub/inner.bin", 2000000);
    writeFile(m_dir + "/root-file.bin", 600000);
    QVERIFY(scan(m_dir));
    const QModelIndex top = m_model->index(0, 0);
    QCOMPARE(childNames(*m_model, top), QStringList({"Media"})); // folders only by default

    m_model->setShowFiles(true);
    const QModelIndex topAgain = m_model->index(0, 0);
    QCOMPARE(childNames(*m_model, topAgain), QStringList({"Media", "root-file.bin"}));
    const QModelIndex media = m_model->indexForPath(m_dir + "/Media");
    const QStringList rows = childNames(*m_model, media);
    // The sub-folder, the 10 biggest files (biggest first), then the rest in one row.
    QCOMPARE(rows.size(), 1 + DiskUsageModel::MaxFilesPerFolder + 1);
    QCOMPARE(rows.first(), QStringLiteral("Sub"));
    QCOMPARE(rows.at(1), QStringLiteral("big11.bin"));
    QCOMPARE(rows.last(), QStringLiteral("7 smaller files")); // 2 small-enough bigs + 5 small
    qint64 sum = 0;
    for (int r = 0; r < m_model->rowCount(media); ++r)
        sum += m_model->index(r, 0, media).data(DiskUsageModel::BytesRole).toLongLong();
    const qint64 folder = media.data(DiskUsageModel::BytesRole).toLongLong();
    QVERIFY2(folder - sum >= 0 && folder - sum <= 64 * 1024, // just the folder's own entry
             qPrintable(QString("%1 vs %2").arg(folder).arg(sum)));
    const QModelIndex smaller = m_model->index(m_model->rowCount(media) - 1, 0, media);
    QVERIFY(m_model->filePath(smaller).isEmpty()); // not a real item: no actions
    QCOMPARE(m_model->indexForPath(m_dir + "/Media/big11.bin").data(DiskUsageModel::KindRole).toInt(),
             int(DiskUsageModel::Node::File));
    // A big file can be trashed from here too.
    QVERIFY(m_model->removePath(m_dir + "/Media/big11.bin"));
    QVERIFY(!m_model->indexForPath(m_dir + "/Media/big11.bin").isValid());
}

void TestDiskUsage::removeKeepsOtherRows()
{
    makeTree();
    QVERIFY(scan(m_dir));
    const QPersistentModelIndex deepest = m_model->indexForPath(m_dir + "/Big/Deeper/Deepest");
    const QPersistentModelIndex small = m_model->indexForPath(m_dir + "/Small");
    QSignalSpy resets(m_model, &QAbstractItemModel::modelReset);
    QVERIFY(m_model->removePath(m_dir + "/Many"));
    QCOMPARE(resets.count(), 0); // no reset: the view keeps its open folders
    QVERIFY(deepest.isValid() && small.isValid());
    QCOMPARE(m_model->filePath(deepest), m_dir + "/Big/Deeper/Deepest");
    QCOMPARE(m_model->filePath(small), m_dir + "/Small");
}

void TestDiskUsage::addPathMeasuresWhatArrived()
{
    if (QStandardPaths::findExecutable("du").isEmpty())
        QSKIP("no du");
    makeTree();
    QVERIFY(scan(m_dir));
    const qint64 full = m_model->rootBytes();
    // Like Move to Trash then Undo: it leaves, then comes back.
    QTemporaryDir away;
    QVERIFY(QDir().rename(m_dir + "/Big", away.path() + "/Big"));
    QVERIFY(m_model->removePath(m_dir + "/Big"));
    QVERIFY(m_model->rootBytes() < full);
    QVERIFY(QDir().rename(away.path() + "/Big", m_dir + "/Big"));
    QSignalSpy added(m_model, &DiskUsageModel::added);
    QVERIFY(m_model->addPath(m_dir + "/Big"));
    QVERIFY(added.wait(10000));
    QCOMPARE(m_model->rootBytes(), full);
    QCOMPARE(m_model->rootBytes(), du(m_dir));
    QCOMPARE(childNames(*m_model, m_model->index(0, 0)).first(), QStringLiteral("Big")); // sorted back in
    const QModelIndex deeper = m_model->indexForPath(m_dir + "/Big/Deeper");
    QCOMPARE(deeper.data(DiskUsageModel::BytesRole).toLongLong(), du(m_dir + "/Big/Deeper"));

    // A new file arrives (e.g. a copy); small ones join the "smaller files" row.
    writeFile(m_dir + "/Small/copy.txt", 5000);
    QVERIFY(m_model->addPath(m_dir + "/Small/copy.txt"));
    QVERIFY(added.wait(10000));
    QCOMPARE(m_model->rootBytes(), du(m_dir));
    QVERIFY(!m_model->addPath("/somewhere/else"));

    // ...and moving it away again takes it out (it's measured where it went).
    QTemporaryDir elsewhere;
    QVERIFY(QFile::rename(m_dir + "/Small/copy.txt", elsewhere.path() + "/copy.txt"));
    QVERIFY(m_model->removePath(m_dir + "/Small/copy.txt",
                                DiskUsageModel::sizeOnDisk(elsewhere.path() + "/copy.txt")));
    QCOMPARE(m_model->rootBytes(), du(m_dir));
}

QTEST_MAIN(TestDiskUsage)
#include "tst_diskusage.moc"
