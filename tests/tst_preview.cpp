// Tests for Preview (Quick Look content): each kind of file gets the right
// kind of preview, and a program in disguise is never touched.

#include "Preview.h"
#include "Thumbnailer.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QPdfWriter>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#if defined(ARGONAUT_ARCHIVES)
#include <archive.h>
#include <archive_entry.h>
#endif

using Preview::Kind;

class TestPreview : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void image();
    void text();
    void binaryIsNotText();
    void folder();
    void archive();
    void blockedNeverTouched();
    void generic();
    void pdfThroughSandbox();

private:
    QString write(const QString &name, const QByteArray &data);
    QTemporaryDir *m_dir = nullptr;
};

void TestPreview::init()
{
    delete m_dir;
    m_dir = new QTemporaryDir;
}

QString TestPreview::write(const QString &name, const QByteArray &data)
{
    const QString path = m_dir->path() + '/' + name;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size())
        qFatal("couldn't write %s", qPrintable(path));
    return path;
}

void TestPreview::image()
{
    QImage big(3000, 2000, QImage::Format_RGB32);
    big.fill(Qt::darkGreen);
    const QString path = m_dir->path() + "/photo.png";
    QVERIFY(big.save(path));
    const Preview::Result r = Preview::load(path, "image/png", ContentType::Verdict::Consistent, QSize(800, 600), nullptr);
    QCOMPARE(r.kind, Kind::Image);
    QVERIFY(r.image.width() <= 800 && r.image.height() <= 600); // decoded at the size shown, not 3000x2000
    QCOMPARE(r.image.width(), 800);
}

void TestPreview::text()
{
    const QString path = write("notes.md", "# Τίτλος\n\nSome *text* — ünïcödé.\n");
    Preview::Result r = Preview::load(path, "text/markdown", ContentType::Verdict::Consistent, QSize(800, 600), nullptr);
    QCOMPARE(r.kind, Kind::Text);
    QVERIFY(r.text.contains("Τίτλος") && r.text.contains("ünïcödé"));
    QVERIFY(!r.truncated);
    // Code and JSON are text too; long files are cut.
    const QString big = write("data.json", "{\"a\": \"" + QByteArray(Preview::MaxTextBytes * 2, 'x') + "\"}");
    r = Preview::load(big, "application/json", ContentType::Verdict::Consistent, QSize(800, 600), nullptr);
    QCOMPARE(r.kind, Kind::Text);
    QVERIFY(r.truncated);
    QCOMPARE(r.text.size(), Preview::MaxTextBytes);
    QVERIFY(Preview::isTextual("text/x-c++src"));
    QVERIFY(Preview::isTextual("application/x-shellscript")); // read, never run
    QVERIFY(!Preview::isTextual("application/pdf"));
}

void TestPreview::binaryIsNotText()
{
    const QString path = write("weird.txt", QByteArray("abc\0def\0", 8));
    const Preview::Result r = Preview::load(path, "text/plain", ContentType::Verdict::Consistent, QSize(800, 600), nullptr);
    QCOMPARE(r.kind, Kind::Generic);
}

void TestPreview::folder()
{
    write("Folder/a.txt", "a");
    write("Folder/.hidden", "h");
    QDir().mkpath(m_dir->path() + "/Folder/Sub");
    const Preview::Result r = Preview::load(m_dir->path() + "/Folder", "inode/directory",
                                            ContentType::Verdict::Consistent, QSize(800, 600), nullptr);
    QCOMPARE(r.kind, Kind::Folder);
    QCOMPARE(r.items, 3);
}

void TestPreview::archive()
{
#if !defined(ARGONAUT_ARCHIVES)
    QSKIP("built without libarchive");
#else
    const QString path = m_dir->path() + "/bundle.tar";
    struct archive *a = archive_write_new();
    archive_write_set_format_pax_restricted(a);
    QCOMPARE(archive_write_open_filename(a, QFile::encodeName(path).constData()), ARCHIVE_OK);
    auto add = [a](const char *name, const QByteArray &data, bool dir) {
        struct archive_entry *e = archive_entry_new();
        archive_entry_set_pathname(e, name);
        archive_entry_set_filetype(e, dir ? AE_IFDIR : AE_IFREG);
        archive_entry_set_perm(e, 0644);
        archive_entry_set_size(e, dir ? 0 : data.size());
        archive_write_header(a, e);
        if (!dir)
            archive_write_data(a, data.constData(), size_t(data.size()));
        archive_entry_free(e);
    };
    add("docs/", {}, true);
    add("docs/readme.txt", QByteArray(1234, 'r'), false);
    add("image.png", QByteArray(50, 'p'), false);
    archive_write_close(a);
    archive_write_free(a);
    const Preview::Result r = Preview::load(path, "application/x-tar", ContentType::Verdict::Consistent, QSize(800, 600), nullptr);
    QCOMPARE(r.kind, Kind::Archive);
    QCOMPARE(r.total, 3);
    QCOMPARE(r.entries.size(), 3);
    QCOMPARE(r.entries.at(0).path, QStringLiteral("docs"));
    QVERIFY(r.entries.at(0).isDir);
    QCOMPARE(r.entries.at(1).path, QStringLiteral("docs/readme.txt"));
    QCOMPARE(r.entries.at(1).size, 1234);
    QVERIFY(QFileInfo::exists(path) && !QFileInfo::exists(m_dir->path() + "/docs")); // nothing extracted
#endif
}

void TestPreview::blockedNeverTouched()
{
    // A program named like a document: Blocked before anything reads it.
    // Unreadable on purpose: if load() tried to open it, we'd get Generic.
    const QString path = write("invoice.pdf", QByteArray("\x7f" "ELF", 4) + QByteArray(60, '\0'));
    QFile::setPermissions(path, QFileDevice::Permissions());
    const Preview::Result r = Preview::load(path, "application/x-executable", ContentType::Verdict::Dangerous,
                                            QSize(800, 600), nullptr);
    QCOMPARE(r.kind, Kind::Blocked);
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

void TestPreview::generic()
{
    const QString path = write("data.bin", QByteArray(500, '\x01'));
    const Preview::Result r = Preview::load(path, "application/octet-stream", ContentType::Verdict::Consistent,
                                            QSize(800, 600), nullptr);
    QCOMPARE(r.kind, Kind::Generic);
}

void TestPreview::pdfThroughSandbox()
{
    if (QStandardPaths::findExecutable("bwrap").isEmpty() || QStandardPaths::findExecutable("evince-thumbnailer").isEmpty())
        QSKIP("needs bwrap and evince-thumbnailer");
    const QString path = m_dir->path() + "/report.pdf";
    {
        QPdfWriter pdf(path);
        QPainter p(&pdf);
        p.fillRect(QRect(0, 0, 3000, 3000), Qt::red);
    }
    Thumbnailer thumbnailer;
    const Preview::Result r = Preview::load(path, "application/pdf", ContentType::Verdict::Consistent, QSize(900, 900),
                                            &thumbnailer);
    QCOMPARE(r.kind, Kind::Image);
    QCOMPARE(qMax(r.image.width(), r.image.height()), 900); // the preview size, not the 256 px thumbnail
    QVERIFY(r.image.pixelColor(r.image.width() / 6, r.image.height() / 6).red() > 200);
}

int main(int argc, char **argv)
{
    QTemporaryDir sandbox;
    qputenv("HOME", QFile::encodeName(sandbox.path() + "/home"));
    qputenv("XDG_CACHE_HOME", QFile::encodeName(sandbox.path() + "/cache"));
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(sandbox.path() + "/config"));
    qputenv("XDG_DATA_HOME", QFile::encodeName(sandbox.path() + "/data"));
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    TestPreview test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_preview.moc"
