// Tests for DirectoryModel: listing, natural folders-first sorting, hidden
// files, live updates from disk, persistent indexes and item counts.

#include "DirectoryModel.h"
#include "Format.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QMimeData>
#include <QUrl>
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
    void dragData();
    void sizesLikeGnome();
    void naturalOrderInCLocale();
    void realTypesInBackground();
    void bigFolder();
    void searchMatching();
    void searchFindsInSubfolders();
    void searchHiddenAndSymlinks();
    void searchEndsAndRefreshes();
    void searchStopsAtLimit();

private:
    bool load(const QString &path);
    bool search(const QString &query);

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

bool TestDirectoryModel::search(const QString &query)
{
    QSignalSpy loaded(m_model, &DirectoryModel::loaded);
    m_model->setSearch(query);
    return loaded.wait(20000);
}

void TestDirectoryModel::searchMatching()
{
    auto words = [](const QString &q) {
        return DirectoryModel::foldForSearch(q).split(' ', Qt::SkipEmptyParts);
    };
    QVERIFY(DirectoryModel::matchesSearch("Annual REPORT 2024.pdf", words("report")));
    QVERIFY(DirectoryModel::matchesSearch("Annual REPORT 2024.pdf", words("2024 annual"))); // any order
    QVERIFY(!DirectoryModel::matchesSearch("Annual REPORT 2024.pdf", words("report 2023")));
    QVERIFY(DirectoryModel::matchesSearch("Θάλασσα.jpg", words("θαλασσα")));  // no accents typed
    QVERIFY(DirectoryModel::matchesSearch("ΘΆΛΑΣΣΑ.jpg", words("θάλασσα")));  // case
    QVERIFY(DirectoryModel::matchesSearch("Καλοκαίρι.jpg", words("καλοκαιρι")));
    QVERIFY(DirectoryModel::matchesSearch("Café menu.txt", words("cafe")));
    QVERIFY(DirectoryModel::matchesSearch("cafe menu.txt", words("Café")));
    QVERIFY(!DirectoryModel::matchesSearch("compare.png", words("compa x")));
}

void TestDirectoryModel::searchFindsInSubfolders()
{
    touch(m_dir + "/report.txt");
    touch(m_dir + "/Work/2024/Annual Report.pdf");
    touch(m_dir + "/Work/notes.txt");
    QDir().mkpath(m_dir + "/Reports"); // folders match too
    touch(m_dir + "/Reports/other.txt");
    QVERIFY(load(m_dir));
    QVERIFY(search("report"));

    QVERIFY(m_model->isSearch());
    QCOMPARE(names(*m_model), QStringList({"Reports", "Annual Report.pdf", "report.txt"}));
    QCOMPARE(m_model->headerData(DirectoryModel::TypeColumn, Qt::Horizontal).toString(),
             QStringLiteral("Location"));
    const QModelIndex deep = m_model->indexForPath(m_dir + "/Work/2024/Annual Report.pdf");
    QVERIFY(deep.isValid());
    QCOMPARE(m_model->index(deep.row(), DirectoryModel::TypeColumn).data().toString(),
             QDir::toNativeSeparators("Work/2024"));
    QVERIFY(m_model->index(deep.row(), 0).data(Qt::ToolTipRole).toString().contains("Work"));
    QVERIFY(!m_model->searchTruncated());

    QVERIFY(search("annual 2024")); // words are matched against the name only
    QCOMPARE(names(*m_model), QStringList());
    QVERIFY(search("annual report"));
    QCOMPARE(names(*m_model), QStringList({"Annual Report.pdf"}));
}

void TestDirectoryModel::searchHiddenAndSymlinks()
{
    touch(m_dir + "/visible/match.txt");
    touch(m_dir + "/.config/match.txt");
    touch(m_dir + "/.match-hidden.txt");
    // A loop: following it would never end (or find everything twice).
    QVERIFY(QFile::link(m_dir, m_dir + "/visible/loop"));
    QVERIFY(load(m_dir));
    QVERIFY(search("match"));
    QCOMPARE(names(*m_model), QStringList({"match.txt"}));

    QSignalSpy loaded(m_model, &DirectoryModel::loaded);
    m_model->setShowHidden(true); // searches again, inside hidden folders too
    QVERIFY(loaded.wait(20000));
    QCOMPARE(names(*m_model).size(), 3);
}

void TestDirectoryModel::searchEndsAndRefreshes()
{
    touch(m_dir + "/a.txt");
    touch(m_dir + "/sub/match.txt");
    touch(m_dir + "/sub/match2.txt");
    QVERIFY(load(m_dir));
    QVERIFY(search("match"));
    QCOMPARE(names(*m_model).size(), 2);

    // A result removed in a (not watched) sub-folder goes on refresh().
    QVERIFY(QFile::remove(m_dir + "/sub/match2.txt"));
    m_model->refresh();
    QTRY_COMPARE_WITH_TIMEOUT(names(*m_model), QStringList({"match.txt"}), 5000);

    QVERIFY(search(QString())); // cleared: the folder again
    QVERIFY(!m_model->isSearch());
    QCOMPARE(names(*m_model), QStringList({"sub", "a.txt"}));
    QCOMPARE(m_model->headerData(DirectoryModel::TypeColumn, Qt::Horizontal).toString(),
             QStringLiteral("Type"));

    QVERIFY(search("match"));
    QVERIFY(load(m_dir + "/sub")); // going elsewhere ends the search
    QVERIFY(!m_model->isSearch());
    QCOMPARE(names(*m_model), QStringList({"match.txt"}));
}

void TestDirectoryModel::searchStopsAtLimit()
{
    const int n = DirectoryModel::MaxSearchResults + 50;
    QDir().mkpath(m_dir + "/many");
    for (int i = 0; i < n; ++i) {
        QFile f(m_dir + "/many/hit" + QString::number(i));
        QVERIFY(f.open(QIODevice::WriteOnly));
    }
    QVERIFY(load(m_dir));
    QVERIFY(search("hit"));
    QCOMPARE(m_model->rowCount(), DirectoryModel::MaxSearchResults);
    QVERIFY(m_model->searchTruncated());
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

// Dragging out: one URL per item even when every column is selected.
void TestDirectoryModel::dragData()
{
    touch(m_dir + "/a.txt");
    touch(m_dir + "/b.txt");
    QVERIFY(load(m_dir));
    QModelIndexList cells;
    for (int row = 0; row < m_model->rowCount(); ++row)
        for (int col = 0; col < m_model->columnCount(); ++col)
            cells.append(m_model->index(row, col));
    QVERIFY(m_model->flags(cells.first()) & Qt::ItemIsDragEnabled);
    std::unique_ptr<QMimeData> data(m_model->mimeData(cells));
    QVERIFY(data);
    QCOMPARE(data->urls(), QList<QUrl>({QUrl::fromLocalFile(m_dir + "/a.txt"),
                                        QUrl::fromLocalFile(m_dir + "/b.txt")}));
    QVERIFY(data->text().contains("a.txt"));
}

// Powers of 1000 with kB/MB/GB, like Nautilus and what drives are sold as.
void TestDirectoryModel::sizesLikeGnome()
{
    QLocale::setDefault(QLocale::c());
    QCOMPARE(formatSize(0), QStringLiteral("0 bytes"));
    QCOMPARE(formatSize(1), QStringLiteral("1 byte"));
    QCOMPARE(formatSize(999), QStringLiteral("999 bytes"));
    QCOMPARE(formatSize(3000), QStringLiteral("3.0 kB"));
    QCOMPARE(formatSize(52'000'000), QStringLiteral("52.0 MB"));
    QCOMPARE(formatSize(32'000'000'000), QStringLiteral("32.0 GB")); // a "32 GB" stick
    touch(m_dir + "/f.bin", 1500);
    QVERIFY(load(m_dir));
    QCOMPARE(m_model->index(0, DirectoryModel::SizeColumn).data().toString(), QStringLiteral("1.5 kB"));
    QLocale::setDefault(QLocale::system());
}

// Even under LANG=C, numbers in names sort naturally.
void TestDirectoryModel::naturalOrderInCLocale()
{
    QLocale::setDefault(QLocale::c());
    for (const char *f : {"file10.txt", "file2.txt", "file1.txt"})
        touch(m_dir + '/' + f);
    QVERIFY(load(m_dir));
    QCOMPARE(names(*m_model), QStringList({"file1.txt", "file2.txt", "file10.txt"}));
    QLocale::setDefault(QLocale::system());
}

// Names are shown at once; the real content is checked in the background,
// remembered, and not read again on the next visit.
void TestDirectoryModel::realTypesInBackground()
{
    {
        QFile f(m_dir + "/report.pdf");
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("<!DOCTYPE html><html><body>Download failed</body></html>\n");
    }
    {
        QFile f(m_dir + "/real.png");
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArray("\x89PNG\r\n\x1a\n\0\0\0\rIHDR\0\0\0\x10\0\0\0\x10\x08\x06\0\0\0", 33));
    }
    const int readsBefore = ContentType::readCount();
    QVERIFY(load(m_dir));
    const auto verdictOf = [this](const char *name) {
        return ContentType::Verdict(
            m_model->indexForPath(m_dir + '/' + name).data(DirectoryModel::VerdictRole).toInt());
    };
    QTRY_COMPARE_WITH_TIMEOUT(verdictOf("report.pdf"), ContentType::Verdict::Mismatch, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(verdictOf("real.png"), ContentType::Verdict::Consistent, 5000);
    const QModelIndex fake = m_model->indexForPath(m_dir + "/report.pdf");
    QCOMPARE(fake.siblingAtColumn(DirectoryModel::TypeColumn).data().toString(),
             QStringLiteral("HTML document")); // the real type is shown
    QVERIFY(fake.data(Qt::ToolTipRole).toString().contains("HTML"));
    QCOMPARE(ContentType::readCount() - readsBefore, 2);

    // Coming back: answers come from the cache, no file is read.
    const int readsAfterFirst = ContentType::readCount();
    DirectoryModel again;
    QSignalSpy loaded(&again, &DirectoryModel::loaded);
    again.setPath(m_dir);
    QVERIFY(loaded.wait(5000));
    QCOMPARE(ContentType::Verdict(again.indexForPath(m_dir + "/report.pdf")
                                      .data(DirectoryModel::VerdictRole)
                                      .toInt()),
             ContentType::Verdict::Mismatch);
    QTest::qWait(200);
    QCOMPARE(ContentType::readCount(), readsAfterFirst);
}

int main(int argc, char **argv)
{
    QTemporaryDir cache;
    qputenv("XDG_CACHE_HOME", QFile::encodeName(cache.path())); // never the real cache
    QCoreApplication app(argc, argv);
    TestDirectoryModel test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_directorymodel.moc"
