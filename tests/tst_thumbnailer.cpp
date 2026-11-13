// Tests for Thumbnailer: system thumbnailers (.thumbnailer files), the
// shared cache, remembered failures, the location policy, cancelling, and a
// real sandboxed run of evince/totem when they're installed.
//
// Everything is sandboxed: XDG_CACHE_HOME (the shared thumbnail cache),
// XDG_CONFIG_HOME (settings) and XDG_DATA_DIRS (the fake thumbnailers).

#include "Thumbnailer.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QPdfWriter>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

static const char *FakeMime = "application/x-kaftilous-test";

class TestThumbnailer : public QObject
{
    Q_OBJECT

public:
    explicit TestThumbnailer(QString sandbox) : m_sandbox(std::move(sandbox)) {}

private slots:
    void initTestCase();
    void init();
    void programsFromThumbnailerFiles();
    void generatesOnceAndShares();
    void failureIsRemembered();
    void policy();
    void cancelDropsQueued();
    void dangerousAndUnknownGetNothing();
    void realPdfInSandbox();
    void realVideoInSandbox();

private:
    QString makeFile(const QString &name);
    int runs() const;
    QString m_sandbox;
    QString m_share; // fake XDG data dir with thumbnailers/
    QString m_dir;   // files to thumbnail
};

void TestThumbnailer::initTestCase()
{
    m_share = m_sandbox + "/share";
    QDir().mkpath(m_share + "/thumbnailers");
    QImage fixture(300, 200, QImage::Format_RGB32);
    fixture.fill(Qt::darkCyan);
    QVERIFY(fixture.save(m_sandbox + "/fixture.png"));
    // $1=-s $2=size $3=uri $4=output; FAIL makes it fail, SLOW makes it slow.
    QFile script(m_sandbox + "/fake-thumbnailer.sh");
    QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(QStringLiteral("#!/bin/sh\necho \"$@\" >> %1/runs.log\n"
                                "[ -f %1/SLOW ] && sleep 0.4\n"
                                "[ -f %1/FAIL ] && exit 1\n"
                                "cp %1/fixture.png \"$4\"\n").arg(m_sandbox).toUtf8());
    script.close();
    QFile::setPermissions(script.fileName(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    QFile entry(m_share + "/thumbnailers/fake.thumbnailer");
    QVERIFY(entry.open(QIODevice::WriteOnly));
    entry.write(QStringLiteral("[Thumbnailer Entry]\nTryExec=%1\nExec=%1 -s %s %u %o\nMimeType=%2;\n")
                    .arg(script.fileName(), FakeMime).toUtf8());
    entry.close();
    QFile missing(m_share + "/thumbnailers/missing.thumbnailer");
    QVERIFY(missing.open(QIODevice::WriteOnly));
    missing.write("[Thumbnailer Entry]\nTryExec=/nonexistent/thumbnailer\nExec=/nonexistent/thumbnailer %u %o\n"
                  "MimeType=application/x-not-installed;\n");
}

void TestThumbnailer::init()
{
    qputenv("XDG_DATA_DIRS", QFile::encodeName(m_share));
    qputenv("KAFTILOUS_THUMBNAIL_SANDBOX", "0"); // the fake lives outside /usr
    qunsetenv("KAFTILOUS_SAFE_WRITES");
    Thumbnailer::setPolicy(Thumbnailer::Policy::LocalOnly);
    QDir(m_sandbox + "/cache").removeRecursively();
    QFile::remove(m_sandbox + "/runs.log");
    QFile::remove(m_sandbox + "/FAIL");
    QFile::remove(m_sandbox + "/SLOW");
    m_dir = m_sandbox + "/files/" + QTest::currentTestFunction();
    QDir(m_dir).removeRecursively();
    QDir().mkpath(m_dir);
}

QString TestThumbnailer::makeFile(const QString &name)
{
    const QString path = m_dir + '/' + name;
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write("data");
    return path;
}

int TestThumbnailer::runs() const
{
    QFile log(m_sandbox + "/runs.log");
    return log.open(QIODevice::ReadOnly) ? int(log.readAll().count('\n')) : 0;
}

void TestThumbnailer::programsFromThumbnailerFiles()
{
    Thumbnailer t;
    QVERIFY(t.canGenerate(FakeMime));
    QVERIFY(!t.canGenerate("application/x-not-installed")); // TryExec missing
    QVERIFY(!t.canGenerate(QString()));
}

void TestThumbnailer::generatesOnceAndShares()
{
    const QString path = makeFile("sample.ktest");
    const QDateTime mtime = QFileInfo(path).lastModified();
    {
        Thumbnailer t;
        QSignalSpy ready(&t, &Thumbnailer::thumbnailReady);
        QVERIFY(t.thumbnail(path, mtime, FakeMime).isNull()); // not yet
        QVERIFY(ready.wait(5000));
        const QPixmap pix = t.thumbnail(path, mtime, FakeMime);
        QVERIFY(!pix.isNull());
        QVERIFY(pix.width() <= Thumbnailer::MaxSize && pix.height() <= Thumbnailer::MaxSize);
        QCOMPARE(t.generatedCount(), 1);
    }
    QCOMPARE(runs(), 1);
    // Stored per the freedesktop spec, for every app.
    const QString cached = Thumbnailer::cachePathFor(path);
    QVERIFY2(cached.startsWith(m_sandbox + "/cache/thumbnails/large/"), qPrintable(cached));
    QImage img(cached);
    QVERIFY(!img.isNull());
    QCOMPARE(img.text("Thumb::URI"), QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded));
    QCOMPARE(img.text("Thumb::MTime").toLongLong(), mtime.toSecsSinceEpoch());
    QCOMPARE(QFile::permissions(cached) & (QFileDevice::ReadGroup | QFileDevice::ReadOther), QFileDevice::Permissions());
    // A new instance (next run of the app) reads it, without running anything.
    Thumbnailer again;
    QSignalSpy ready(&again, &Thumbnailer::thumbnailReady);
    again.thumbnail(path, mtime, FakeMime);
    QVERIFY(ready.wait(5000));
    QCOMPARE(runs(), 1);
    QCOMPARE(again.generatedCount(), 0);
}

void TestThumbnailer::failureIsRemembered()
{
    QFile(m_sandbox + "/FAIL").open(QIODevice::WriteOnly);
    const QString path = makeFile("broken.ktest");
    QDateTime mtime = QFileInfo(path).lastModified();
    {
        Thumbnailer t;
        t.thumbnail(path, mtime, FakeMime);
        QTRY_COMPARE_WITH_TIMEOUT(runs(), 1, 5000);
    }
    Thumbnailer again;
    QSignalSpy ready(&again, &Thumbnailer::thumbnailReady);
    again.thumbnail(path, mtime, FakeMime);
    QTest::qWait(500);
    QCOMPARE(runs(), 1); // not tried again…
    QCOMPARE(ready.count(), 0);
    // …until the file changes.
    QFile::remove(m_sandbox + "/FAIL");
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write("changed");
    f.flush(); // or closing writes it and sets the time back to "now"
    f.setFileTime(mtime.addSecs(10), QFileDevice::FileModificationTime);
    f.close();
    mtime = QFileInfo(path).lastModified();
    Thumbnailer third;
    QSignalSpy ready3(&third, &Thumbnailer::thumbnailReady);
    third.thumbnail(path, mtime, FakeMime);
    QVERIFY(ready3.wait(5000));
    QCOMPARE(runs(), 2);
}

void TestThumbnailer::policy()
{
    const QString path = makeFile("p.ktest");
    const QDateTime mtime = QFileInfo(path).lastModified();
    Thumbnailer::setPolicy(Thumbnailer::Policy::Never);
    {
        Thumbnailer t;
        t.thumbnail(path, mtime, FakeMime);
        QTest::qWait(400);
        QCOMPARE(runs(), 0);
    }
    Thumbnailer::setPolicy(Thumbnailer::Policy::LocalOnly);
    qputenv("KAFTILOUS_SAFE_WRITES", "1"); // pretend this is a USB stick
    {
        Thumbnailer t;
        t.thumbnail(path, mtime, FakeMime);
        QTest::qWait(400);
        QCOMPARE(runs(), 0);
    }
    Thumbnailer::setPolicy(Thumbnailer::Policy::Everywhere);
    {
        Thumbnailer t;
        QSignalSpy ready(&t, &Thumbnailer::thumbnailReady);
        t.thumbnail(path, mtime, FakeMime);
        QVERIFY(ready.wait(5000));
        QCOMPARE(runs(), 1);
    }
}

void TestThumbnailer::cancelDropsQueued()
{
    QFile(m_sandbox + "/SLOW").open(QIODevice::WriteOnly);
    Thumbnailer t;
    for (int i = 0; i < 12; ++i) {
        const QString path = makeFile(QStringLiteral("f%1.ktest").arg(i));
        t.thumbnail(path, QFileInfo(path).lastModified(), FakeMime);
    }
    QTRY_VERIFY_WITH_TIMEOUT(runs() >= 1, 5000); // the first ones started
    t.cancelPending();                            // leaving the folder
    QTest::qWait(1500);
    QVERIFY2(runs() <= 4, qPrintable(QString::number(runs()))); // not all 12
}

void TestThumbnailer::dangerousAndUnknownGetNothing()
{
    Thumbnailer t;
    const QString path = makeFile("notes.txt");
    QVERIFY(t.thumbnail(path, QFileInfo(path).lastModified(), QString()).isNull());
    QVERIFY(t.thumbnail(path, QFileInfo(path).lastModified(), "text/plain").isNull());
    QTest::qWait(300);
    QCOMPARE(runs(), 0);
}

void TestThumbnailer::realPdfInSandbox()
{
    if (QStandardPaths::findExecutable("bwrap").isEmpty() || QStandardPaths::findExecutable("evince-thumbnailer").isEmpty())
        QSKIP("needs bwrap and evince-thumbnailer");
    qputenv("XDG_DATA_DIRS", "/usr/local/share:/usr/share");
    qunsetenv("KAFTILOUS_THUMBNAIL_SANDBOX");
    QVERIFY(Thumbnailer::sandboxAvailable());
    const QString path = m_dir + "/report.pdf";
    {
        QPdfWriter pdf(path);
        QPainter p(&pdf);
        p.fillRect(QRect(0, 0, 3000, 3000), Qt::blue);
    }
    Thumbnailer t;
    QVERIFY(t.canGenerate("application/pdf"));
    QSignalSpy ready(&t, &Thumbnailer::thumbnailReady);
    t.thumbnail(path, QFileInfo(path).lastModified(), "application/pdf");
    QVERIFY(ready.wait(15000));
    const QImage made(Thumbnailer::cachePathFor(path));
    QCOMPARE(qMax(made.width(), made.height()), Thumbnailer::GenerateSize);
    QVERIFY(made.pixelColor(made.width() / 4, made.height() / 4).blue() > 200); // really the page
}

void TestThumbnailer::realVideoInSandbox()
{
    if (QStandardPaths::findExecutable("bwrap").isEmpty() || QStandardPaths::findExecutable("totem-video-thumbnailer").isEmpty()
        || QStandardPaths::findExecutable("gst-launch-1.0").isEmpty())
        QSKIP("needs bwrap, totem-video-thumbnailer and gstreamer");
    qputenv("XDG_DATA_DIRS", "/usr/local/share:/usr/share");
    qunsetenv("KAFTILOUS_THUMBNAIL_SANDBOX");
    const QString path = m_dir + "/clip.webm";
    QProcess::execute("gst-launch-1.0", {"-q", "videotestsrc", "num-buffers=60", "pattern=smpte", "!",
                                         "video/x-raw,width=320,height=240", "!", "vp8enc", "!", "webmmux",
                                         "!", "filesink", "location=" + path});
    if (!QFileInfo(path).size())
        QSKIP("couldn't make a test video");
    Thumbnailer t;
    QSignalSpy ready(&t, &Thumbnailer::thumbnailReady);
    t.thumbnail(path, QFileInfo(path).lastModified(), "video/webm");
    QVERIFY(ready.wait(20000));
    QVERIFY(QFile::exists(Thumbnailer::cachePathFor(path)));
}

int main(int argc, char **argv)
{
    QTemporaryDir sandbox;
    qputenv("XDG_CACHE_HOME", QFile::encodeName(sandbox.path() + "/cache"));
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(sandbox.path() + "/config"));
    qputenv("XDG_DATA_HOME", QFile::encodeName(sandbox.path() + "/data"));
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    TestThumbnailer test(sandbox.path());
    return QTest::qExec(&test, argc, argv);
}

#include "tst_thumbnailer.moc"
