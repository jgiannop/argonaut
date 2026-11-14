// Tests for searching inside files: words safe for queries, snippets,
// plain-text scanning, and DirectoryModel's search with a fake tracker3
// (ARGONAUT_TRACKER_PROGRAM) standing in for GNOME's search index.

#include "ContentSearch.h"
#include "DirectoryModel.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

namespace {

void writeFile(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(content);
}

QStringList names(const DirectoryModel &model)
{
    QStringList out;
    for (int row = 0; row < model.rowCount(); ++row)
        out << model.index(row, 0).data().toString();
    out.sort();
    return out;
}

QStringList folded(const QString &q)
{
    return DirectoryModel::foldForSearch(q).split(' ', Qt::SkipEmptyParts);
}

} // namespace

class TestContentSearch : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void queryWordsAreSafe();
    void snippets();
    void textFiles();
    void modelFindsTextInside();
    void modelUsesTheIndex();

private:
    bool search(DirectoryModel &model, const QString &query);
    void fakeTracker(const QString &script);

    QTemporaryDir *m_tmp = nullptr;
    QTemporaryDir m_binDir;
    QString m_dir;
    QString m_bin;
};

void TestContentSearch::initTestCase()
{
    QVERIFY(m_binDir.isValid());
    m_bin = QDir::cleanPath(m_binDir.path());
}

void TestContentSearch::init()
{
    m_tmp = new QTemporaryDir;
    m_dir = QDir::cleanPath(m_tmp->path());
    qunsetenv("ARGONAUT_TRACKER_PROGRAM");
}

void TestContentSearch::cleanup()
{
    delete m_tmp;
}

void TestContentSearch::fakeTracker(const QString &script)
{
    const QString path = m_bin + "/tracker3";
    writeFile(path, ("#!/bin/sh\n" + script).toUtf8());
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    qputenv("ARGONAUT_TRACKER_PROGRAM", QFile::encodeName(path));
}

bool TestContentSearch::search(DirectoryModel &model, const QString &query)
{
    QSignalSpy loaded(&model, &DirectoryModel::loaded);
    model.setSearch(query);
    return loaded.wait(20000);
}

void TestContentSearch::queryWordsAreSafe()
{
    QCOMPARE(ContentSearch::queryWords("invoice 2024"), QStringList({"invoice", "2024"}));
    // Quotes, braces and query syntax never reach the query.
    QCOMPARE(ContentSearch::queryWords("\"} DROP { x\" a"), QStringList({"DROP"}));
    QCOMPARE(ContentSearch::queryWords("θάλασσα-ταξίδι"), QStringList({"θάλασσα", "ταξίδι"}));
}

void TestContentSearch::snippets()
{
    const QString text = "first line\nThe invoice for March is attached here.\nlast";
    const qsizetype at = text.indexOf("invoice");
    QCOMPARE(ContentSearch::snippetAround(text, at, 7, 200), QStringLiteral("The invoice for March is attached here."));
    const QString shortSnippet = ContentSearch::snippetAround(text, at, 7, 20);
    QVERIFY(shortSnippet.contains("invoice"));
    QVERIFY(shortSnippet.endsWith(QChar(0x2026)));
}

void TestContentSearch::textFiles()
{
    QVERIFY(ContentSearch::isTextFile("notes.txt", 10));
    QVERIFY(ContentSearch::isTextFile("code.cpp", 10));
    QVERIFY(ContentSearch::isTextFile("data.json", 10));
    QVERIFY(!ContentSearch::isTextFile("photo.jpg", 10));
    QVERIFY(!ContentSearch::isTextFile("huge.txt", ContentSearch::MaxTextBytes + 1));
    QVERIFY(!ContentSearch::isTextFile("empty.txt", 0));

    writeFile(m_dir + "/a.txt", "Ταξίδι στη Θάλασσα\nκαι Café au lait\n");
    QString snippet;
    QVERIFY(ContentSearch::textMatches(m_dir + "/a.txt", folded("θαλασσα"), &snippet));
    QVERIFY2(snippet.contains("Θάλασσα"), qPrintable(snippet)); // the original, with its accents
    QVERIFY(ContentSearch::textMatches(m_dir + "/a.txt", folded("cafe ταξιδι"), nullptr)); // every word, anywhere
    QVERIFY(!ContentSearch::textMatches(m_dir + "/a.txt", folded("cafe missing"), nullptr));
    writeFile(m_dir + "/bin.txt", QByteArray("needle\0\0binary", 14));
    QVERIFY(!ContentSearch::textMatches(m_dir + "/bin.txt", folded("needle"), nullptr));
}

void TestContentSearch::modelFindsTextInside()
{
    fakeTracker("exit 1\n"); // no index
    writeFile(m_dir + "/notes.txt", "call the plumber about the leak\n");
    writeFile(m_dir + "/src/code.cpp", "// TODO: fix the leak in parser\n");
    writeFile(m_dir + "/leak-report.pdf", "%PDF-1.4"); // found by name
    writeFile(m_dir + "/other.txt", "nothing here\n");
    writeFile(m_dir + "/.hidden/secret.txt", "leak\n");

    DirectoryModel model;
    QSignalSpy loaded(&model, &DirectoryModel::loaded);
    model.setPath(m_dir);
    QVERIFY(loaded.wait(10000));

    QVERIFY(search(model, "leak"));
    QCOMPARE(names(model), QStringList({"leak-report.pdf"})); // names only by default

    QSignalSpy again(&model, &DirectoryModel::loaded);
    model.setSearchContents(true); // searches again
    QVERIFY(again.wait(10000));
    QCOMPARE(names(model), QStringList({"code.cpp", "leak-report.pdf", "notes.txt"}));
    const QModelIndex note = model.indexForPath(m_dir + "/notes.txt");
    QVERIFY(note.siblingAtColumn(DirectoryModel::TypeColumn).data().toString().contains("plumber about the leak"));
    QVERIFY(note.data(Qt::ToolTipRole).toString().contains("Found in its text"));
    const QModelIndex byName = model.indexForPath(m_dir + "/leak-report.pdf");
    QVERIFY(!byName.data(Qt::ToolTipRole).toString().contains("Found in its text"));

    QVERIFY(search(model, "leak parser")); // every word
    QCOMPARE(names(model), QStringList({"code.cpp"}));
}

void TestContentSearch::modelUsesTheIndex()
{
    // A fake index returning one document under the folder, one outside
    // it, one in a hidden folder and one that no longer exists. The query
    // is saved, to check its shape.
    writeFile(m_dir + "/docs/contract.pdf", "%PDF");
    writeFile(m_dir + "/.cache/c.pdf", "%PDF");
    const QString url = QUrl::fromLocalFile(m_dir).toString(QUrl::FullyEncoded);
    fakeTracker(QString("printf '%s' \"$5\" > \"%1/query.txt\"\n"
                        "printf 'Results:\\n  %2/docs/contract.pdf\\tthe notice period is three months\\n"
                        "  file:///elsewhere/x.pdf\\tnotice\\n  %2/.cache/c.pdf\\tnotice\\n  %2/gone.pdf\\tnotice\\n'\n")
                    .arg(m_bin, url));

    DirectoryModel model;
    model.setSearchContents(true);
    QSignalSpy loaded(&model, &DirectoryModel::loaded);
    model.setPath(m_dir);
    QVERIFY(loaded.wait(10000));
    QVERIFY(search(model, "notice period"));
    QCOMPARE(names(model), QStringList({"contract.pdf"}));
    const QModelIndex doc = model.indexForPath(m_dir + "/docs/contract.pdf");
    QVERIFY(doc.siblingAtColumn(DirectoryModel::TypeColumn).data().toString().contains("three months"));

    QFile saved(m_bin + "/query.txt");
    QVERIFY(saved.open(QIODevice::ReadOnly));
    const QString query = QString::fromUtf8(saved.readAll());
    QVERIFY2(query.contains("CONTAINS(LCASE(?t), \"notice\")") && query.contains("CONTAINS(LCASE(?t), \"period\")"),
             qPrintable(query));
    QVERIFY(!query.contains("fts:match")); // its index can be incomplete
    QVERIFY(query.contains("STRSTARTS(?url, \"" + url + "/\")"));
}

int main(int argc, char **argv)
{
    QTemporaryDir sandbox;
    qputenv("HOME", QFile::encodeName(sandbox.path() + "/home"));
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(sandbox.path() + "/config"));
    qputenv("XDG_DATA_HOME", QFile::encodeName(sandbox.path() + "/data"));
    qputenv("XDG_CACHE_HOME", QFile::encodeName(sandbox.path() + "/cache"));
    qputenv("ARGONAUT_TRASH_HOME_ONLY", "1");
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    TestContentSearch test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_contentsearch.moc"
