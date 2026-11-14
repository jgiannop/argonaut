// Tests for DirectoryModel: listing, natural folders-first sorting, hidden
// files, live updates from disk, persistent indexes and item counts.

#include "Access.h"
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

#if defined(Q_OS_UNIX)
#include <sys/stat.h>
#include <unistd.h>
#endif

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
    void accessHelpers();
    void ownersAndPermissions();
    void lockAndLinkEmblems();
    void chmodUpdatesRow();
    void selectionChangeableFolders();
    void treeExpandCollapse();
    void treeSortsEachFolder();
    void treeLiveUpdates();
    void treeOpenedFolderDeleted();
    void treeHiddenItems();
    void treeOnlyForPlainFolders();
    void treeOpenRacesRescan();
    void filterRules();
    void searchWithFilters();

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

void TestDirectoryModel::accessHelpers()
{
    using P = QFile::Permissions;
    QCOMPARE(Access::permissionString(P(QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther),
                                      false, false),
             QStringLiteral("-rw-r--r--"));
    QCOMPARE(Access::permissionString(P(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadGroup
                                        | QFile::ExeGroup | QFile::ReadOther | QFile::ExeOther),
                                      true, false),
             QStringLiteral("drwxr-xr-x"));
    QCOMPARE(Access::permissionString(Access::PermissionMask, false, true), QStringLiteral("lrwxrwxrwx"));
    QCOMPARE(Access::permissionString(P(), false, false), QStringLiteral("----------"));
#if defined(Q_OS_UNIX)
    if (::geteuid() == 0)
        QSKIP("root can write everything");
    const uint me = uint(::geteuid());
    const uint myGroup = uint(::getegid());
    const uint other = me + 12345;
    QVERIFY(Access::likelyWritable(P(QFile::WriteOwner), me, other));
    QVERIFY(!Access::likelyWritable(P(QFile::WriteGroup | QFile::WriteOther), me, myGroup)); // owner bits rule
    QVERIFY(Access::likelyWritable(P(QFile::WriteGroup), other, myGroup));
    QVERIFY(!Access::likelyWritable(P(QFile::WriteOwner | QFile::WriteOther), other, myGroup));
    QVERIFY(Access::likelyWritable(P(QFile::WriteOther), other, other + 1));
    QVERIFY(!Access::likelyWritable(P(QFile::WriteOwner), 0, 0));
    QVERIFY(!Access::userName(me).isEmpty());
    QCOMPARE(Access::userName(0), QStringLiteral("root"));
    QCOMPARE(Access::userName(Access::NoId), QString());
#endif
}

void TestDirectoryModel::ownersAndPermissions()
{
#if !defined(Q_OS_UNIX)
    QSKIP("Unix owners and permission bits");
#else
    touch(m_dir + "/plain.txt");
    QVERIFY(QFile::setPermissions(m_dir + "/plain.txt",
                                  QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther));
    QVERIFY(load(m_dir));
    const QModelIndex row = m_model->indexForPath(m_dir + "/plain.txt");
    QVERIFY(row.isValid());
    QCOMPARE(row.siblingAtColumn(DirectoryModel::PermissionsColumn).data().toString(), QStringLiteral("-rw-r--r--"));
    QCOMPARE(row.siblingAtColumn(DirectoryModel::OwnerColumn).data().toString(),
             QFileInfo(m_dir + "/plain.txt").owner());
    QCOMPARE(row.siblingAtColumn(DirectoryModel::GroupColumn).data().toString(),
             QFileInfo(m_dir + "/plain.txt").group());
    QCOMPARE(row.data(DirectoryModel::OwnerIdRole).toUInt(), uint(::geteuid()));
    QCOMPARE(m_model->headerData(DirectoryModel::OwnerColumn, Qt::Horizontal).toString(), QStringLiteral("Owner"));

    // The same mtime and size QFileInfo gives (the content cache keys on them).
    const QFileInfo fi(m_dir + "/plain.txt");
    QCOMPARE(row.data(DirectoryModel::ModifiedRole).toDateTime().toMSecsSinceEpoch(),
             fi.lastModified().toMSecsSinceEpoch());

    // Sorting by permissions: 0644 before 0664.
    touch(m_dir + "/shared.txt");
    QVERIFY(QFile::setPermissions(m_dir + "/shared.txt", QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup
                                                             | QFile::WriteGroup | QFile::ReadOther));
    QVERIFY(load(m_dir));
    m_model->sort(DirectoryModel::PermissionsColumn, Qt::DescendingOrder);
    QCOMPARE(names(*m_model), QStringList({"shared.txt", "plain.txt"}));
#endif
}

void TestDirectoryModel::lockAndLinkEmblems()
{
#if !defined(Q_OS_UNIX)
    QSKIP("Unix permission bits and symlinks");
#else
    if (::geteuid() == 0)
        QSKIP("root can write everything");
    touch(m_dir + "/mine.txt");
    touch(m_dir + "/readonly.txt");
    QVERIFY(QFile::setPermissions(m_dir + "/readonly.txt", QFile::ReadOwner | QFile::ReadGroup));
    QVERIFY(QFile::link(m_dir + "/mine.txt", m_dir + "/link.txt"));
    QVERIFY(QFile::link(m_dir + "/missing", m_dir + "/broken"));
    QDir().mkdir(m_dir + "/sub");
    QVERIFY(QFile::link(m_dir + "/sub", m_dir + "/sublink"));
    QVERIFY(load(m_dir));

    auto at = [this](const QString &name) { return m_model->indexForPath(m_dir + '/' + name); };
    QVERIFY(!at("mine.txt").data(DirectoryModel::LockedRole).toBool());
    QVERIFY(at("readonly.txt").data(DirectoryModel::LockedRole).toBool());
    QVERIFY(at("readonly.txt").data(Qt::ToolTipRole).toString().contains("Read-only"));
    QVERIFY(!at("mine.txt").data(DirectoryModel::SymLinkRole).toBool());
    QVERIFY(at("link.txt").data(DirectoryModel::SymLinkRole).toBool());
    QVERIFY(at("link.txt").data(Qt::ToolTipRole).toString().contains("Link to"));
    QVERIFY(at("broken").data(DirectoryModel::SymLinkRole).toBool());
    QVERIFY(at("broken").data(Qt::ToolTipRole).toString().contains("Broken link"));
    // A link to a folder lists as a folder, like before.
    QVERIFY(m_model->isDir(at("sublink")));
    QVERIFY(at("sublink").data(DirectoryModel::SymLinkRole).toBool());
    QVERIFY(QFile::setPermissions(m_dir + "/readonly.txt", QFile::ReadOwner | QFile::WriteOwner));
#endif
}

void TestDirectoryModel::chmodUpdatesRow()
{
#if !defined(Q_OS_LINUX)
    QSKIP("inotify IN_ATTRIB");
#else
    if (::geteuid() == 0)
        QSKIP("root can write everything");
    touch(m_dir + "/f.txt");
    QVERIFY(load(m_dir));
    QVERIFY(!m_model->indexForPath(m_dir + "/f.txt").data(DirectoryModel::LockedRole).toBool());
    QVERIFY(QFile::setPermissions(m_dir + "/f.txt", QFile::ReadOwner));
    QTRY_VERIFY_WITH_TIMEOUT(m_model->indexForPath(m_dir + "/f.txt").data(DirectoryModel::LockedRole).toBool(), 5000);
    QVERIFY(QFile::setPermissions(m_dir + "/f.txt", QFile::ReadOwner | QFile::WriteOwner));
    QTRY_VERIFY_WITH_TIMEOUT(!m_model->indexForPath(m_dir + "/f.txt").data(DirectoryModel::LockedRole).toBool(), 5000);
#endif
}

void TestDirectoryModel::selectionChangeableFolders()
{
#if !defined(Q_OS_UNIX)
    QSKIP("Unix folder permissions");
#else
    if (::geteuid() == 0)
        QSKIP("root can change everything");
    const uint me = uint(::geteuid());
    QDir().mkdir(m_dir + "/open");
    QDir().mkdir(m_dir + "/closed");
    QDir().mkdir(m_dir + "/sticky");
    QVERIFY(::chmod(QFile::encodeName(m_dir + "/closed").constData(), 0555) == 0);
    QVERIFY(::chmod(QFile::encodeName(m_dir + "/sticky").constData(), 01777) == 0);
    QHash<QString, Access::FolderAccess> cache;
    QVERIFY(Access::canChangeItemsIn(m_dir + "/open", me, &cache));
    QVERIFY(!Access::canChangeItemsIn(m_dir + "/closed", me, &cache));
    // A sticky folder you own: anything in it may change.
    QVERIFY(Access::canChangeItemsIn(m_dir + "/sticky", me + 1, &cache));
    QCOMPARE(cache.size(), 3);
    QVERIFY(cache.value(m_dir + "/sticky").sticky);
    // /tmp is sticky and owned by root: only your own items.
    QHash<QString, Access::FolderAccess> tmp;
    struct stat st;
    if (::stat("/tmp", &st) == 0 && (st.st_mode & S_ISVTX) && st.st_uid == 0 && ::access("/tmp", W_OK) == 0) {
        QVERIFY(Access::canChangeItemsIn("/tmp", me, &tmp));
        QVERIFY(!Access::canChangeItemsIn("/tmp", 0, &tmp));
    }
    QVERIFY(::chmod(QFile::encodeName(m_dir + "/closed").constData(), 0755) == 0);
#endif
}

namespace {
// "name@depth" for every row, in order.
QStringList treeRows(const DirectoryModel &model)
{
    QStringList out;
    for (int row = 0; row < model.rowCount(); ++row) {
        const QModelIndex i = model.index(row, 0);
        out << i.data().toString() + '@' + QString::number(i.data(DirectoryModel::TreeDepthRole).toInt());
    }
    return out;
}
} // namespace

void TestDirectoryModel::treeExpandCollapse()
{
    touch(m_dir + "/A/a1.txt");
    touch(m_dir + "/A/a2.txt");
    touch(m_dir + "/A/B/b1.txt");
    touch(m_dir + "/z.txt");
    m_model->setTreeEnabled(true);
    QVERIFY(load(m_dir));
    QCOMPARE(treeRows(*m_model), QStringList({"A@0", "z.txt@0"}));
    const QModelIndex a = m_model->indexForPath(m_dir + "/A");
    QVERIFY(a.data(DirectoryModel::ExpandableRole).toBool());
    QVERIFY(!a.data(DirectoryModel::ExpandedRole).toBool());

    m_model->expand(m_dir + "/A");
    QTRY_COMPARE_WITH_TIMEOUT(treeRows(*m_model), QStringList({"A@0", "B@1", "a1.txt@1", "a2.txt@1", "z.txt@0"}), 5000);
    QVERIFY(m_model->indexForPath(m_dir + "/A").data(DirectoryModel::ExpandedRole).toBool());
    QCOMPARE(m_model->filePath(m_model->index(1, 0)), m_dir + "/A/B");

    m_model->expand(m_dir + "/A/B");
    QTRY_COMPARE_WITH_TIMEOUT(treeRows(*m_model),
                              QStringList({"A@0", "B@1", "b1.txt@2", "a1.txt@1", "a2.txt@1", "z.txt@0"}), 5000);

    // Closing A closes B too.
    m_model->collapse(m_dir + "/A");
    QCOMPARE(treeRows(*m_model), QStringList({"A@0", "z.txt@0"}));
    QVERIFY(m_model->expandedFolders().isEmpty());

    // Turning the tree off closes everything and shows no depth.
    m_model->expand(m_dir + "/A");
    QTRY_COMPARE_WITH_TIMEOUT(m_model->rowCount(), 5, 5000);
    m_model->setTreeEnabled(false);
    QCOMPARE(m_model->rowCount(), 2);
    QCOMPARE(m_model->index(0, 0).data(DirectoryModel::TreeDepthRole).toInt(), -1);
    m_model->expand(m_dir + "/A"); // no tree: nothing happens
    QTest::qWait(100);
    QCOMPARE(m_model->rowCount(), 2);
}

void TestDirectoryModel::treeSortsEachFolder()
{
    touch(m_dir + "/A/a1.txt");
    touch(m_dir + "/A/a10.txt");
    touch(m_dir + "/A/a2.txt", 5000);
    QDir().mkpath(m_dir + "/A/Sub");
    touch(m_dir + "/b.txt", 10);
    touch(m_dir + "/c.txt", 100000);
    m_model->setTreeEnabled(true);
    QVERIFY(load(m_dir));
    m_model->expand(m_dir + "/A");
    QTRY_COMPARE_WITH_TIMEOUT(m_model->rowCount(), 7, 5000);
    QCOMPARE(names(*m_model), QStringList({"A", "Sub", "a1.txt", "a2.txt", "a10.txt", "b.txt", "c.txt"}));
    m_model->sort(DirectoryModel::NameColumn, Qt::DescendingOrder);
    QCOMPARE(names(*m_model), QStringList({"A", "Sub", "a10.txt", "a2.txt", "a1.txt", "c.txt", "b.txt"}));
    m_model->sort(DirectoryModel::SizeColumn, Qt::DescendingOrder);
    QCOMPARE(names(*m_model).mid(0, 3), QStringList({"A", "Sub", "a2.txt"})); // biggest in A first
    QCOMPARE(names(*m_model).mid(5), QStringList({"c.txt", "b.txt"}));
}

void TestDirectoryModel::treeLiveUpdates()
{
    touch(m_dir + "/A/a1.txt");
    touch(m_dir + "/z.txt");
    m_model->setTreeEnabled(true);
    QVERIFY(load(m_dir));
    m_model->expand(m_dir + "/A");
    QTRY_COMPARE_WITH_TIMEOUT(treeRows(*m_model), QStringList({"A@0", "a1.txt@1", "z.txt@0"}), 5000);

    touch(m_dir + "/A/a0.txt"); // inside the opened folder: it's watched
    QTRY_COMPARE_WITH_TIMEOUT(treeRows(*m_model), QStringList({"A@0", "a0.txt@1", "a1.txt@1", "z.txt@0"}), 5000);
    QVERIFY(QFile::remove(m_dir + "/A/a1.txt"));
    QTRY_COMPARE_WITH_TIMEOUT(treeRows(*m_model), QStringList({"A@0", "a0.txt@1", "z.txt@0"}), 5000);
    touch(m_dir + "/m.txt"); // in the open folder: rows of A stay
    QTRY_COMPARE_WITH_TIMEOUT(treeRows(*m_model), QStringList({"A@0", "a0.txt@1", "m.txt@0", "z.txt@0"}), 5000);

    // Closed folders aren't watched any more.
    m_model->collapse(m_dir + "/A");
    touch(m_dir + "/A/late.txt");
    QTest::qWait(500);
    QCOMPARE(treeRows(*m_model), QStringList({"A@0", "m.txt@0", "z.txt@0"}));
}

void TestDirectoryModel::treeOpenedFolderDeleted()
{
    touch(m_dir + "/A/B/b1.txt");
    touch(m_dir + "/z.txt");
    m_model->setTreeEnabled(true);
    QVERIFY(load(m_dir));
    m_model->expand(m_dir + "/A");
    QTRY_COMPARE_WITH_TIMEOUT(m_model->rowCount(), 3, 5000);
    m_model->expand(m_dir + "/A/B");
    QTRY_COMPARE_WITH_TIMEOUT(m_model->rowCount(), 4, 5000);
    QVERIFY(QDir(m_dir + "/A").removeRecursively());
    QTRY_COMPARE_WITH_TIMEOUT(treeRows(*m_model), QStringList({"z.txt@0"}), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(m_model->expandedFolders().isEmpty(), 5000);
}

void TestDirectoryModel::treeHiddenItems()
{
    touch(m_dir + "/A/.secret");
    touch(m_dir + "/A/shown.txt");
    touch(m_dir + "/.H/inside.txt");
    m_model->setTreeEnabled(true);
    QVERIFY(load(m_dir));
    m_model->expand(m_dir + "/A");
    QTRY_COMPARE_WITH_TIMEOUT(treeRows(*m_model), QStringList({"A@0", "shown.txt@1"}), 5000);
    m_model->setShowHidden(true);
    QCOMPARE(treeRows(*m_model), QStringList({".H@0", "A@0", ".secret@1", "shown.txt@1"}));
    m_model->expand(m_dir + "/.H");
    QTRY_COMPARE_WITH_TIMEOUT(m_model->rowCount(), 5, 5000);
    // Hiding again closes the hidden folder (its items would float otherwise).
    m_model->setShowHidden(false);
    QCOMPARE(treeRows(*m_model), QStringList({"A@0", "shown.txt@1"}));
    QVERIFY(!m_model->isExpanded(m_dir + "/.H"));
    QVERIFY(m_model->isExpanded(m_dir + "/A"));
}

void TestDirectoryModel::treeOnlyForPlainFolders()
{
    touch(m_dir + "/A/a1.txt");
    m_model->setTreeEnabled(true);
    QVERIFY(load(m_dir));
    QVERIFY(m_model->isTreeActive());
    m_model->expand(m_dir + "/A");
    QTRY_COMPARE_WITH_TIMEOUT(m_model->rowCount(), 2, 5000);
    // A search is flat and starts with every folder closed.
    QVERIFY(search("a1"));
    QVERIFY(!m_model->isTreeActive());
    QCOMPARE(m_model->index(0, 0).data(DirectoryModel::TreeDepthRole).toInt(), -1);
    QVERIFY(m_model->expandedFolders().isEmpty());
    QCOMPARE(names(*m_model), QStringList({"a1.txt"})); // found once, not twice
    QVERIFY(search(QString()));
    QVERIFY(m_model->isTreeActive());
    QCOMPARE(m_model->rowCount(), 1);
}

void TestDirectoryModel::treeOpenRacesRescan()
{
    for (int i = 0; i < 300; ++i)
        touch(QStringLiteral("%1/f%2.txt").arg(m_dir).arg(i));
    touch(m_dir + "/A/inside.txt");
    m_model->setTreeEnabled(true);
    QVERIFY(load(m_dir));
    // A rescan that doesn't know about A yet, and opening A at once.
    touch(m_dir + "/new.txt");
    m_model->refresh();
    QTest::qWait(250); // the rescan is running or done
    m_model->expand(m_dir + "/A");
    touch(m_dir + "/new2.txt");
    QTRY_VERIFY_WITH_TIMEOUT(m_model->indexForPath(m_dir + "/new2.txt").isValid(), 5000);
    QTest::qWait(400);
    QVERIFY(m_model->indexForPath(m_dir + "/A/inside.txt").isValid());
    QCOMPARE(m_model->index(1, 0).data().toString(), QStringLiteral("inside.txt"));
    QCOMPARE(m_model->rowCount(), 300 + 1 + 1 + 2);
}

void TestDirectoryModel::filterRules()
{
    using T = DirectoryModel::TypeFilter;
    using D = DirectoryModel::DateFilter;
    const qint64 now = QDateTime(QDate(2026, 11, 14), QTime(12, 0)).toMSecsSinceEpoch();
    const qint64 day = 24LL * 3600 * 1000;
    auto pass = [&](T t, D d, const QString &name, bool dir, qint64 mtime) {
        return DirectoryModel::passesFilters(t, d, name, dir, mtime, now);
    };
    QVERIFY(pass(T::Images, D::Any, "a.JPG", false, now));
    QVERIFY(!pass(T::Images, D::Any, "a.pdf", false, now));
    QVERIFY(pass(T::Documents, D::Any, "a.pdf", false, now));
    QVERIFY(pass(T::Documents, D::Any, "a.docx", false, now));
    QVERIFY(pass(T::Documents, D::Any, "a.odt", false, now));
    QVERIFY(pass(T::Audio, D::Any, "a.mp3", false, now));
    QVERIFY(pass(T::Video, D::Any, "a.mkv", false, now));
    QVERIFY(pass(T::Archives, D::Any, "a.tar.gz", false, now));
    QVERIFY(pass(T::Text, D::Any, "main.cpp", false, now));
    QVERIFY(pass(T::Folders, D::Any, "x", true, now));
    QVERIFY(!pass(T::Folders, D::Any, "x.txt", false, now));
    QVERIFY(!pass(T::Images, D::Any, "pics.jpg", true, now)); // a folder isn't an image
    QVERIFY(pass(T::Any, D::Today, "a", false, now - 3600 * 1000));
    QVERIFY(!pass(T::Any, D::Today, "a", false, now - day));
    QVERIFY(pass(T::Any, D::Week, "a", false, now - 6 * day));
    QVERIFY(!pass(T::Any, D::Week, "a", false, now - 8 * day));
    QVERIFY(pass(T::Any, D::Month, "a", false, now - 29 * day));
    QVERIFY(pass(T::Any, D::Year, "a", false, QDateTime(QDate(2026, 1, 2), QTime(0, 0)).toMSecsSinceEpoch()));
    QVERIFY(!pass(T::Any, D::Year, "a", false, QDateTime(QDate(2025, 12, 31), QTime(0, 0)).toMSecsSinceEpoch()));
    QVERIFY(pass(T::Any, D::OlderThanYear, "a", false, now - 400 * day));
    QVERIFY(!pass(T::Any, D::OlderThanYear, "a", false, now - 10 * day));
    QVERIFY(pass(T::Images, D::Week, "b.png", false, now - day));
    QVERIFY(!pass(T::Images, D::Week, "b.png", false, now - 20 * day));
}

void TestDirectoryModel::searchWithFilters()
{
    touch(m_dir + "/beach.jpg");
    touch(m_dir + "/trip/beach2.png");
    touch(m_dir + "/trip/notes.txt");
    touch(m_dir + "/report.pdf");
    QDir().mkpath(m_dir + "/beach-folder");
    // An old photo.
    touch(m_dir + "/old.jpg");
    QFile old(m_dir + "/old.jpg");
    QVERIFY(old.open(QIODevice::ReadWrite));
    QVERIFY(old.setFileTime(QDateTime::currentDateTime().addYears(-2), QFileDevice::FileModificationTime));
    old.close();
    QVERIFY(load(m_dir));

    // Filters alone search, in sub-folders too.
    QSignalSpy loaded(m_model, &DirectoryModel::loaded);
    m_model->setSearchFilters(DirectoryModel::TypeFilter::Images, DirectoryModel::DateFilter::Any);
    QVERIFY(loaded.wait(10000));
    QVERIFY(m_model->isSearch());
    QStringList found = names(*m_model);
    found.sort();
    QCOMPARE(found, QStringList({"beach.jpg", "beach2.png", "old.jpg"}));

    QVERIFY(search("beach")); // words and filters together
    found = names(*m_model);
    found.sort();
    QCOMPARE(found, QStringList({"beach.jpg", "beach2.png"})); // not the folder

    QSignalSpy again(m_model, &DirectoryModel::loaded);
    m_model->setSearchFilters(DirectoryModel::TypeFilter::Images, DirectoryModel::DateFilter::Week);
    QVERIFY(again.wait(10000));
    QVERIFY(search(""));
    found = names(*m_model);
    found.sort();
    QCOMPARE(found, QStringList({"beach.jpg", "beach2.png"})); // not the old one

    QSignalSpy off(m_model, &DirectoryModel::loaded);
    m_model->setSearchFilters(DirectoryModel::TypeFilter::Any, DirectoryModel::DateFilter::Any);
    QVERIFY(off.wait(10000));
    QVERIFY(!m_model->isSearch());
    QCOMPARE(m_model->rowCount(), 5); // the folder again
}

int main(int argc, char **argv)
{
    QTemporaryDir cache;
    // Never the real cache, data (stars, recent files) or settings.
    qputenv("XDG_CACHE_HOME", QFile::encodeName(cache.path() + "/cache"));
    qputenv("XDG_DATA_HOME", QFile::encodeName(cache.path() + "/data"));
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(cache.path() + "/config"));
    qputenv("HOME", QFile::encodeName(cache.path() + "/home"));
    QDir().mkpath(cache.path() + "/home");
    QDir().mkpath(cache.path() + "/data");
    QCoreApplication app(argc, argv);
    TestDirectoryModel test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_directorymodel.moc"
