// Tests for the Recent and Starred places: the shared recently-used.xbel
// (read, add, remove, never break other apps' entries), the star list, and
// DirectoryModel showing both. HOME and XDG_* are sandboxed in main().

#include "DirectoryModel.h"
#include "RecentFiles.h"
#include "Starred.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QXmlStreamReader>
#include <QtTest>

namespace {

void touch(const QString &path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("x");
}

// Like GTK writes it: another app's entries, a non-local URI, a duplicate.
QByteArray gtkXbel(const QString &home)
{
    auto entry = [](const QString &href, const char *when, const char *app) {
        return QStringLiteral(
                   "  <bookmark href=\"%1\" added=\"%2\" modified=\"%2\" visited=\"%2\">\n"
                   "    <info>\n      <metadata owner=\"http://freedesktop.org\">\n"
                   "        <mime:mime-type type=\"text/plain\"/>\n        <bookmark:applications>\n"
                   "          <bookmark:application name=\"%3\" exec=\"&apos;%3 %u&apos;\" modified=\"%2\" count=\"1\"/>\n"
                   "        </bookmark:applications>\n      </metadata>\n    </info>\n  </bookmark>\n")
            .arg(href, QLatin1String(when), QLatin1String(app));
    };
    QString x = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<xbel version=\"1.0\"\n"
                "      xmlns:bookmark=\"http://www.freedesktop.org/standards/desktop-bookmarks\"\n"
                "      xmlns:mime=\"http://www.freedesktop.org/standards/shared-mime-info\"\n>\n";
    x += entry(QUrl::fromLocalFile(home + "/old.txt").toString(), "2026-01-01T10:00:00.000000Z", "gedit");
    x += entry(QUrl::fromLocalFile(home + "/Docs/new report.pdf").toString(), "2026-11-10T10:00:00.000000Z", "evince");
    x += entry("https://example.com/page", "2026-11-11T10:00:00.000000Z", "firefox");
    x += entry(QUrl::fromLocalFile(home + "/middle.png").toString(), "2026-06-01T10:00:00.000000Z", "eog");
    x += entry(QUrl::fromLocalFile(home + "/gone.txt").toString(), "2026-11-12T10:00:00.000000Z", "gedit");
    x += "</xbel>\n";
    return x.toUtf8();
}

int countBookmarks(const QString &file)
{
    QFile f(file);
    f.open(QIODevice::ReadOnly);
    QXmlStreamReader xml(&f);
    int n = 0;
    while (!xml.atEnd())
        if (xml.readNext() == QXmlStreamReader::StartElement && xml.name() == QLatin1String("bookmark"))
            ++n;
    return xml.hasError() ? -1 : n;
}

QStringList names(const DirectoryModel &m)
{
    QStringList out;
    for (int r = 0; r < m.rowCount(); ++r)
        out << m.index(r, 0).data().toString();
    return out;
}

} // namespace

class TestPlaces : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void readsNewestFirstLocalOnly();
    void addKeepsOtherApps();
    void addRespectsPrivacy();
    void brokenFileIsNeverOverwritten();
    void removeTakesOnlyThose();
    void starsPersistAndFollowRenames();
    void modelShowsRecent();
    void modelShowsStarred();

private:
    void writeXbel(const QByteArray &data);
    QString m_home;
};

void TestPlaces::init()
{
    m_home = QDir::homePath();
    QVERIFY(m_home.contains(QDir::tempPath())); // sandboxed
    QVERIFY(RecentFiles::filePath().startsWith(QDir::tempPath()));
    qunsetenv("KAFTILOUS_REMEMBER_RECENT");
    qputenv("KAFTILOUS_REMEMBER_RECENT", "1");
    QFile::remove(RecentFiles::filePath());
    Starred::instance().setStarred(Starred::instance().paths(), false);
}

void TestPlaces::writeXbel(const QByteArray &data)
{
    QDir().mkpath(QFileInfo(RecentFiles::filePath()).absolutePath());
    QFile f(RecentFiles::filePath());
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
}

void TestPlaces::readsNewestFirstLocalOnly()
{
    writeXbel(gtkXbel(m_home));
    const QList<RecentFiles::Item> items = RecentFiles::read();
    QStringList paths;
    for (const auto &i : items)
        paths << QFileInfo(i.path).fileName();
    QCOMPARE(paths, QStringList({"gone.txt", "new report.pdf", "middle.png", "old.txt"})); // no https
    QVERIFY(items.first().used > items.last().used);
}

void TestPlaces::addKeepsOtherApps()
{
    writeXbel(gtkXbel(m_home));
    touch(m_home + "/opened.txt");
    QVERIFY(RecentFiles::add(m_home + "/opened.txt", "text/plain"));
    QCOMPARE(countBookmarks(RecentFiles::filePath()), 6); // still valid XML, one more
    QCOMPARE(QFileInfo(RecentFiles::read().first().path).fileName(), QStringLiteral("opened.txt")); // newest
    QFile f(RecentFiles::filePath());
    f.open(QIODevice::ReadOnly);
    const QString text = QString::fromUtf8(f.readAll());
    f.close();
    for (const char *app : {"gedit", "evince", "firefox", "eog"})
        QVERIFY2(text.contains(QStringLiteral("name=\"%1\"").arg(app)), app); // others untouched
    QVERIFY(text.contains("https://example.com/page"));
    // Our own application entry (attribute order doesn't matter in XML).
    const QRegularExpression ours(R"(<bookmark:application [^>]*name="Kaftilous"[^>]*>)");
    const QRegularExpressionMatch m = ours.match(text);
    QVERIFY(m.hasMatch());
    QVERIFY(m.captured(0).contains("count=\"1\""));
    QVERIFY(m.captured(0).contains("exec="));
    // Opening an existing entry again: same entry, count goes up.
    QVERIFY(RecentFiles::add(m_home + "/old.txt", "text/plain"));
    QCOMPARE(countBookmarks(RecentFiles::filePath()), 6);
    QCOMPARE(QFileInfo(RecentFiles::read().first().path).fileName(), QStringLiteral("old.txt"));
    QCOMPARE(QFile::permissions(RecentFiles::filePath()) & (QFileDevice::ReadGroup | QFileDevice::ReadOther),
             QFileDevice::Permissions());
    // No file yet: one is created.
    QFile::remove(RecentFiles::filePath());
    QVERIFY(RecentFiles::add(m_home + "/opened.txt", "text/plain"));
    QCOMPARE(countBookmarks(RecentFiles::filePath()), 1);
}

void TestPlaces::addRespectsPrivacy()
{
    writeXbel(gtkXbel(m_home));
    const QByteArray before = [] { QFile f(RecentFiles::filePath()); f.open(QIODevice::ReadOnly); return f.readAll(); }();
    qputenv("KAFTILOUS_REMEMBER_RECENT", "0"); // "remember recent files" is off
    QVERIFY(!RecentFiles::add(m_home + "/old.txt", "text/plain"));
    QFile f(RecentFiles::filePath());
    f.open(QIODevice::ReadOnly);
    QCOMPARE(f.readAll(), before);
}

void TestPlaces::brokenFileIsNeverOverwritten()
{
    const QByteArray broken = "<?xml version=\"1.0\"?><xbel><bookmark href=\"file:///a\"";
    writeXbel(broken);
    QVERIFY(!RecentFiles::add(m_home + "/old.txt", "text/plain"));
    QVERIFY(!RecentFiles::remove({m_home + "/old.txt"}));
    QFile f(RecentFiles::filePath());
    f.open(QIODevice::ReadOnly);
    QCOMPARE(f.readAll(), broken);
}

void TestPlaces::removeTakesOnlyThose()
{
    writeXbel(gtkXbel(m_home));
    QVERIFY(RecentFiles::remove({m_home + "/middle.png"}));
    QCOMPARE(countBookmarks(RecentFiles::filePath()), 4);
    for (const auto &i : RecentFiles::read())
        QVERIFY(!i.path.endsWith("middle.png"));
}

void TestPlaces::starsPersistAndFollowRenames()
{
    Starred &s = Starred::instance();
    QVERIFY(s.filePath().startsWith(QDir::tempPath()));
    QSignalSpy changed(&s, &Starred::changed);
    s.setStarred({m_home + "/Projects", m_home + "/notes.txt"}, true);
    QVERIFY(s.isStarred(m_home + "/notes.txt"));
    QCOMPARE(changed.count(), 1);
    s.setStarred({m_home + "/Projects/inner.txt"}, true);
    // A folder moves: its star and the stars inside it follow.
    s.renamed(m_home + "/Projects", m_home + "/Archive/Projects");
    QVERIFY(!s.isStarred(m_home + "/Projects"));
    QVERIFY(s.isStarred(m_home + "/Archive/Projects"));
    QVERIFY(s.isStarred(m_home + "/Archive/Projects/inner.txt"));
    QVERIFY(s.isStarred(m_home + "/notes.txt"));
    QCOMPARE(s.paths().first(), m_home + "/Archive/Projects/inner.txt"); // newest first
    QFile f(s.filePath());
    QVERIFY(f.open(QIODevice::ReadOnly));
    QVERIFY(f.readAll().contains(QUrl::fromLocalFile(m_home + "/Archive/Projects").toEncoded()));
    s.setStarred({m_home + "/notes.txt"}, false);
    QVERIFY(!s.isStarred(m_home + "/notes.txt"));
}

void TestPlaces::modelShowsRecent()
{
    touch(m_home + "/old.txt");
    touch(m_home + "/Docs/new report.pdf");
    touch(m_home + "/middle.png");
    QDir().mkpath(m_home + "/Docs"); // a folder in the list is fine too
    writeXbel(gtkXbel(m_home)); // gone.txt doesn't exist
    DirectoryModel model;
    model.sort(DirectoryModel::ModifiedColumn, Qt::DescendingOrder);
    QSignalSpy loaded(&model, &DirectoryModel::loaded);
    model.setPath(DirectoryModel::recentPath());
    QVERIFY(loaded.wait(5000));
    QVERIFY(model.isVirtual() && model.place() == DirectoryModel::Place::Recent);
    QCOMPARE(names(model), QStringList({"new report.pdf", "middle.png", "old.txt"}));
    QCOMPARE(model.headerData(DirectoryModel::TypeColumn, Qt::Horizontal).toString(), QStringLiteral("Location"));
    QCOMPARE(model.headerData(DirectoryModel::ModifiedColumn, Qt::Horizontal).toString(), QStringLiteral("Last Used"));
    QCOMPARE(model.index(0, DirectoryModel::TypeColumn).data().toString(), QDir::toNativeSeparators("~/Docs"));
    // Search inside Recent filters the list.
    QSignalSpy again(&model, &DirectoryModel::loaded);
    model.setSearch("report");
    QVERIFY(again.wait(5000));
    QCOMPARE(names(model), QStringList({"new report.pdf"}));
    // Another app updates the list: it shows up.
    model.setSearch(QString());
    QVERIFY(again.wait(5000));
    touch(m_home + "/fresh.txt");
    QVERIFY(RecentFiles::add(m_home + "/fresh.txt", "text/plain"));
    QTRY_COMPARE_WITH_TIMEOUT(names(model).value(0), QStringLiteral("fresh.txt"), 5000);
}

void TestPlaces::modelShowsStarred()
{
    touch(m_home + "/a.txt");
    touch(m_home + "/b.txt");
    QDir().mkpath(m_home + "/Folder");
    DirectoryModel folder;
    QSignalSpy folderLoaded(&folder, &DirectoryModel::loaded);
    folder.setPath(m_home);
    QVERIFY(folderLoaded.wait(5000));
    QSignalSpy badges(&folder, &QAbstractItemModel::dataChanged);
    Starred::instance().setStarred({m_home + "/a.txt", m_home + "/Folder", m_home + "/missing.txt"}, true);
    QVERIFY(badges.count() >= 1); // the ★ repaints
    QVERIFY(folder.indexForPath(m_home + "/a.txt").data(DirectoryModel::StarredRole).toBool());
    QVERIFY(!folder.indexForPath(m_home + "/b.txt").data(DirectoryModel::StarredRole).toBool());

    DirectoryModel model;
    QSignalSpy loaded(&model, &DirectoryModel::loaded);
    model.setPath(DirectoryModel::starredPath());
    QVERIFY(loaded.wait(5000));
    QCOMPARE(names(model), QStringList({"Folder", "a.txt"})); // missing.txt isn't shown
    Starred::instance().setStarred({m_home + "/a.txt"}, false);
    QTRY_COMPARE_WITH_TIMEOUT(names(model), QStringList({"Folder"}), 5000);
}

int main(int argc, char **argv)
{
    QTemporaryDir sandbox;
    // Before anything reads them (Starred is created on first use).
    qputenv("HOME", QFile::encodeName(sandbox.path() + "/home"));
    qputenv("XDG_DATA_HOME", QFile::encodeName(sandbox.path() + "/data"));
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(sandbox.path() + "/config"));
    qputenv("XDG_CACHE_HOME", QFile::encodeName(sandbox.path() + "/cache"));
    QDir().mkpath(sandbox.path() + "/home");
    QDir().mkpath(sandbox.path() + "/data");
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    TestPlaces test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_places.moc"
