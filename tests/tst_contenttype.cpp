// Tests for ContentType: real file headers under honest and misleading names.

#include "ContentType.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using ContentType::Verdict;

namespace {

const QByteArray Png = QByteArrayLiteral("\x89PNG\r\n\x1a\n\0\0\0\rIHDR\0\0\0\x10\0\0\0\x10\x08\x06\0\0\0");
const QByteArray Html("<!DOCTYPE html>\n<html><head><title>404 Not Found</title></head></html>\n");
const QByteArray Elf = QByteArray("\x7f" "ELF\x02\x01\x01\0", 8) + QByteArray(56, '\0');
const QByteArray DosExe = QByteArray("MZ\x90\0\x03\0\0\0", 8) + QByteArray(120, '\0');
const QByteArray Zip = QByteArray("PK\x03\x04\x14\0\0\0\x08\0", 10) + QByteArray(30, '\0');
const QByteArray SevenZip = QByteArray("7z\xbc\xaf\x27\x1c\0\x04", 8) + QByteArray(24, '\0');
const QByteArray Shell("#!/bin/sh\necho hello\n");
const QByteArray Markdown("# Notes\n\nSome *text* here.\n");
const QByteArray Python("import os\n\ndef main():\n    print(os.getcwd())\n");

} // namespace

class TestContentType : public QObject
{
    Q_OBJECT

private slots:
    void verdicts_data();
    void verdicts();
    void fixedNames();
    void cacheRemembersAndForgets();

public:
    QString m_sandbox;
};

void TestContentType::verdicts_data()
{
    QTest::addColumn<QString>("name");
    QTest::addColumn<QByteArray>("head");
    QTest::addColumn<int>("expected");
    QTest::addColumn<QString>("contentMime"); // empty: don't care

    QTest::newRow("png named png") << "photo.png" << Png << int(Verdict::Consistent) << "image/png";
    QTest::newRow("docx is a zip") << "report.docx" << Zip << int(Verdict::Consistent) << "";
    QTest::newRow("markdown") << "notes.md" << Markdown << int(Verdict::Consistent) << "";
    QTest::newRow("text is text") << "notes.md" << Python << int(Verdict::Consistent) << "";
    QTest::newRow("shell script") << "install.sh" << Shell << int(Verdict::Consistent) << "";
    QTest::newRow("windows program") << "setup.exe" << DosExe << int(Verdict::Consistent) << "";
    QTest::newRow("empty file") << "empty.pdf" << QByteArray() << int(Verdict::Consistent) << "";

    QTest::newRow("html error page as pdf") << "report.pdf" << Html << int(Verdict::Mismatch) << "text/html";
    QTest::newRow("7z named zip") << "game.zip" << SevenZip << int(Verdict::Mismatch) << "application/x-7z-compressed";
    QTest::newRow("png named jpg") << "photo.jpg" << Png << int(Verdict::Mismatch) << "image/png";

    QTest::newRow("linux program as pdf") << "invoice.pdf" << Elf << int(Verdict::Dangerous) << "";
    QTest::newRow("windows program as jpg") << "photo.jpg" << DosExe << int(Verdict::Dangerous) << "";
    QTest::newRow("script as pdf") << "invoice.pdf" << Shell << int(Verdict::Dangerous) << "";

    QTest::newRow("no extension, png inside") << "download" << Png << int(Verdict::Identified) << "image/png";
}

void TestContentType::verdicts()
{
    QFETCH(QString, name);
    QFETCH(QByteArray, head);
    QFETCH(int, expected);
    QFETCH(QString, contentMime);
    const ContentType::Result r = ContentType::inspectData(name, head);
    QVERIFY2(int(r.verdict) == expected,
             qPrintable(QStringLiteral("verdict %1, name=%2 content=%3")
                            .arg(int(r.verdict)).arg(r.nameMime, r.contentMime)));
    if (!contentMime.isEmpty())
        QCOMPARE(r.contentMime, contentMime);
    if (r.verdict >= Verdict::Mismatch)
        QVERIFY(!ContentType::describe(r).isEmpty());
}

void TestContentType::fixedNames()
{
    QCOMPARE(ContentType::fixedName("report.pdf", "text/html"), QStringLiteral("report.html"));
    QCOMPARE(ContentType::fixedName("download", "image/png"), QStringLiteral("download.png"));
    QCOMPARE(ContentType::fixedName("photo.png", "image/png"), QString()); // already right
}

void TestContentType::cacheRemembersAndForgets()
{
    ContentType::Cache &cache = ContentType::Cache::instance();
    QVERIFY2(cache.filePath().startsWith(m_sandbox), qPrintable(cache.filePath()));
    const ContentType::Result r = ContentType::inspectData("report.pdf", Html);
    cache.store("/x/report.pdf", 100, 5000, r);
    ContentType::Result found;
    QVERIFY(cache.lookup("/x/report.pdf", 100, 5000, &found));
    QCOMPARE(found.verdict, Verdict::Mismatch);
    QCOMPARE(found.contentMime, QStringLiteral("text/html"));
    QVERIFY(!cache.lookup("/x/report.pdf", 101, 5000, &found)); // file changed: ask again
    QVERIFY(!cache.lookup("/x/report.pdf", 100, 5001, &found));
    cache.save();
    QVERIFY(QFile::exists(cache.filePath()));
}

int main(int argc, char **argv)
{
    QTemporaryDir sandbox;
    qputenv("XDG_CACHE_HOME", QFile::encodeName(sandbox.path())); // never the real cache
    QCoreApplication app(argc, argv);
    TestContentType test;
    test.m_sandbox = sandbox.path();
    return QTest::qExec(&test, argc, argv);
}

#include "tst_contenttype.moc"
