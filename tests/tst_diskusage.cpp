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

QTEST_MAIN(TestDiskUsage)
#include "tst_diskusage.moc"
