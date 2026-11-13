// Tests for DirectoryModel: listing, natural folders-first sorting, hidden
// files, live updates from disk, persistent indexes and item counts.

#include "DirectoryModel.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

namespace {

void touch(const QString &path, qint64 size = 0)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    if (size > 0)
        f.write(QByteArray(int(size), 'x'));
}

QStringList names(const DirectoryModel &model)
{
    QStringList out;
    for (int row = 0; row < model.rowCount(); ++row)
        out.append(model.index(row, 0).data().toString());
    return out;
}

} // namespace

class TestDirectoryModel : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void naturalOrderFoldersFirst();
    void sortByColumns();
    void hiddenFiles();
    void liveUpdates();
    void persistentIndexSurvivesChanges();
    void itemCounts();
    void pathsAndColumns();
    void folderGone();
    void bigFolder();

private:
    bool load(const QString &path);

    QTemporaryDir *m_tmp = nullptr;
    QString m_dir;
    DirectoryModel *m_model = nullptr;
};

void TestDirectoryModel::init()
{
    m_tmp = new QTemporaryDir;
    QVERIFY(m_tmp->isValid());
    m_dir = QDir::cleanPath(m_tmp->path());
    m_model = new DirectoryModel;
}

void TestDirectoryModel::cleanup()
{
    delete m_model;
    delete m_tmp;
}

bool TestDirectoryModel::load(const QString &path)
{
    QSignalSpy loaded(m_model, &DirectoryModel::loaded);
    m_model->setPath(path);
    return loaded.wait(20000);
}

void TestDirectoryModel::naturalOrderFoldersFirst()
{
    for (const char *f : {"file10.txt", "file2.txt", "File1.txt", "b"})
        touch(m_dir + '/' + f);
    QDir().mkpath(m_dir + "/z dir");
    QDir().mkpath(m_dir + "/A dir");
    QVERIFY(load(m_dir));
    QCOMPARE(names(*m_model),
             QStringList({"A dir", "z dir", "b", "File1.txt", "file2.txt", "file10.txt"}));

    // Descending keeps folders first, like Nautilus.
    m_model->sort(DirectoryModel::NameColumn, Qt::DescendingOrder);
    QCOMPARE(names(*m_model),
             QStringList({"z dir", "A dir", "file10.txt", "file2.txt", "File1.txt", "b"}));
}

void TestDirectoryModel::sortByColumns()
{
    touch(m_dir + "/small.txt", 10);
    touch(m_dir + "/big.txt", 5000);
    touch(m_dir + "/medium.png", 300);
    QDir().mkpath(m_dir + "/folder");
    QVERIFY(load(m_dir));

    m_model->sort(DirectoryModel::SizeColumn, Qt::DescendingOrder);
    QCOMPARE(names(*m_model), QStringList({"folder", "big.txt", "medium.png", "small.txt"}));

    m_model->sort(DirectoryModel::TypeColumn, Qt::AscendingOrder);
    // image/png < text/plain
    QCOMPARE(names(*m_model), QStringList({"folder", "medium.png", "big.txt", "small.txt"}));
}

void TestDirectoryModel::hiddenFiles()
{
    touch(m_dir + "/visible.txt");
    touch(m_dir + "/.hidden");
    touch(m_dir + "/backup.txt~");
    QVERIFY(load(m_dir));
    QCOMPARE(names(*m_model), QStringList({"visible.txt"}));

    m_model->setShowHidden(true);
    QCOMPARE(names(*m_model), QStringList({".hidden", "backup.txt~", "visible.txt"}));
    m_model->setShowHidden(false);
    QCOMPARE(names(*m_model), QStringList({"visible.txt"}));
}

void TestDirectoryModel::liveUpdates()
{
    touch(m_dir + "/b.txt");
    QVERIFY(load(m_dir));

    touch(m_dir + "/a.txt");
    QTRY_COMPARE_WITH_TIMEOUT(names(*m_model), QStringList({"a.txt", "b.txt"}), 5000);

    QVERIFY(QFile::rename(m_dir + "/b.txt", m_dir + "/c.txt"));
    QTRY_COMPARE_WITH_TIMEOUT(names(*m_model), QStringList({"a.txt", "c.txt"}), 5000);

    QDir().mkdir(m_dir + "/new folder");
    QTRY_COMPARE_WITH_TIMEOUT(names(*m_model), QStringList({"new folder", "a.txt", "c.txt"}),
                              5000);

    QVERIFY(QFile::remove(m_dir + "/a.txt"));
    QTRY_COMPARE_WITH_TIMEOUT(names(*m_model), QStringList({"new folder", "c.txt"}), 5000);

    // Size change updates the row in place.
    touch(m_dir + "/c.txt", 2048);
    const QModelIndex c = m_model->indexForPath(m_dir + "/c.txt");
    QTRY_COMPARE_WITH_TIMEOUT(m_model->size(m_model->indexForPath(m_dir + "/c.txt")), 2048, 5000);
    Q_UNUSED(c)
}

void TestDirectoryModel::persistentIndexSurvivesChanges()
{
    touch(m_dir + "/m.txt");
    touch(m_dir + "/z.txt");
    QVERIFY(load(m_dir));
    const QPersistentModelIndex z(m_model->indexForPath(m_dir + "/z.txt"));
    QCOMPARE(z.row(), 1);

    for (const char *f : {"a.txt", "b.txt", "c.txt"})
        touch(m_dir + '/' + f);
    QTRY_COMPARE_WITH_TIMEOUT(m_model->rowCount(), 5, 5000);
    QVERIFY(z.isValid());
    QCOMPARE(z.data().toString(), QStringLiteral("z.txt"));
    QCOMPARE(z.row(), 4);

    m_model->sort(DirectoryModel::NameColumn, Qt::DescendingOrder);
    QCOMPARE(z.row(), 0);
}

void TestDirectoryModel::itemCounts()
{
    for (const char *f : {"one/a", "one/b", "one/.c", "two/x"})
        touch(m_dir + '/' + f);
    QDir().mkpath(m_dir + "/empty");
    QVERIFY(load(m_dir));
    auto sizeText = [this](const char *name) {
        return m_model->indexForPath(m_dir + '/' + name, DirectoryModel::SizeColumn)
            .data()
            .toString();
    };
    QTRY_COMPARE_WITH_TIMEOUT(sizeText("one"), QStringLiteral("3 items"), 5000);
    QCOMPARE(sizeText("two"), QStringLiteral("1 item"));
    QCOMPARE(sizeText("empty"), QStringLiteral("0 items"));
}

void TestDirectoryModel::pathsAndColumns()
{
    touch(m_dir + "/photo.png", 1500);
    QVERIFY(load(m_dir));
    const QModelIndex idx = m_model->indexForPath(m_dir + "/photo.png");
    QVERIFY(idx.isValid());
    QCOMPARE(m_model->filePath(idx), m_dir + "/photo.png");
    QCOMPARE(idx.data(DirectoryModel::PathRole).toString(), m_dir + "/photo.png");
    QVERIFY(!idx.data(DirectoryModel::IsDirRole).toBool());
    QCOMPARE(m_model->columnCount(), int(DirectoryModel::ColumnCount));
    QVERIFY(!idx.siblingAtColumn(DirectoryModel::TypeColumn).data().toString().isEmpty());
    QVERIFY(!idx.siblingAtColumn(DirectoryModel::ModifiedColumn).data().toString().isEmpty());
    QVERIFY(!m_model->indexForPath(m_dir + "/missing").isValid());
    QVERIFY(!m_model->indexForPath("/somewhere/else/photo.png").isValid());
}

void TestDirectoryModel::folderGone()
{
    QDir().mkpath(m_dir + "/inner");
    touch(m_dir + "/inner/f");
    QVERIFY(load(m_dir + "/inner"));
    QSignalSpy gone(m_model, &DirectoryModel::folderGone);
    QVERIFY(QDir(m_dir + "/inner").removeRecursively());
    QVERIFY(gone.wait(5000));
    QCOMPARE(gone.first().first().toString(), m_dir + "/inner");
}

void TestDirectoryModel::bigFolder()
{
    const int count = 20000;
    for (int i = 0; i < count; ++i)
        touch(QStringLiteral("%1/f%2.txt").arg(m_dir).arg(i));
    QElapsedTimer timer;
    timer.start();
    QVERIFY(load(m_dir));
    qInfo("listed %d files in %lld ms", count, timer.elapsed());
    QCOMPARE(m_model->rowCount(), count);
    // Natural order: f0, f1, f2 … f9, f10 …
    QCOMPARE(m_model->index(0, 0).data().toString(), QStringLiteral("f0.txt"));
    QCOMPARE(m_model->index(10, 0).data().toString(), QStringLiteral("f10.txt"));
    QCOMPARE(m_model->index(count - 1, 0).data().toString(), QStringLiteral("f19999.txt"));
}

QTEST_GUILESS_MAIN(TestDirectoryModel)
#include "tst_directorymodel.moc"
