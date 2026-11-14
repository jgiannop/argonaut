// Tests for media details (Properties): exiftool's JSON turned into rows,
// and a real photo read with exiftool in the sandbox when available.

#include "MediaInfo.h"
#include "Sandbox.h"

#include <QGuiApplication>
#include <QImage>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

class TestMediaInfo : public QObject
{
    Q_OBJECT

private slots:
    void parsesPhoto();
    void parsesSong();
    void realPhoto();
};

static QString valueOf(const MediaInfo::Details &d, const QString &label)
{
    for (const auto &[l, v] : d)
        if (l == label)
            return v;
    return {};
}

void TestMediaInfo::parsesPhoto()
{
    const QByteArray json = R"([{"SourceFile":"/tmp/input.jpg","ImageSize":"4000x3000","Make":"Canon","Model":"Canon EOS R6",
        "LensModel":"RF24-105mm F4 L IS USM","DateTimeOriginal":"2024:08:12 14:33:10","ExposureTime":"1/250","FNumber":5.6,
        "ISO":100,"FocalLength":"35.0 mm","GPSPosition":"37 deg 58' 12.00\" N, 23 deg 43' 48.00\" E"}])";
    const MediaInfo::Details d = MediaInfo::fromExifToolJson(json);
    QCOMPARE(valueOf(d, "Dimensions"), QStringLiteral("4000 × 3000 pixels"));
    QCOMPARE(valueOf(d, "Camera"), QStringLiteral("Canon EOS R6")); // not "Canon Canon EOS R6"
    QCOMPARE(valueOf(d, "Lens"), QStringLiteral("RF24-105mm F4 L IS USM"));
    QVERIFY(valueOf(d, "Taken").contains("2024"));
    QCOMPARE(valueOf(d, "Exposure"), QStringLiteral("1/250 s · f/5.6 · ISO 100 · 35.0 mm"));
    QVERIFY(valueOf(d, "Location").contains("37 deg"));
    QCOMPARE(d.first().first, QStringLiteral("Dimensions")); // display order
}

void TestMediaInfo::parsesSong()
{
    const QByteArray json = R"j([{"Title":"Song","Artist":"Band","Album":"LP","Duration":"0:03:25 (approx)","AudioBitrate":"320 kbps","SampleRate":44100}])j";
    const MediaInfo::Details d = MediaInfo::fromExifToolJson(json);
    QCOMPARE(valueOf(d, "Title"), QStringLiteral("Song"));
    QCOMPARE(valueOf(d, "Duration"), QStringLiteral("0:03:25"));
    QCOMPARE(valueOf(d, "Bitrate"), QStringLiteral("320 kbps"));
    QCOMPARE(valueOf(d, "Sample rate"), QStringLiteral("44100 Hz"));
    QVERIFY(MediaInfo::fromExifToolJson("not json").isEmpty());
}

void TestMediaInfo::realPhoto()
{
    if (QStandardPaths::findExecutable("exiftool").isEmpty() || !Sandbox::available())
        QSKIP("needs exiftool and bwrap");
    QTemporaryDir dir;
    const QString path = dir.path() + "/photo.jpg";
    QImage image(640, 480, QImage::Format_RGB32);
    image.fill(Qt::blue);
    QVERIFY(image.save(path));
    QCOMPARE(QProcess::execute("exiftool", {"-q", "-overwrite_original", "-Make=Canon", "-Model=Canon EOS R6",
                                            "-DateTimeOriginal=2024:08:12 14:33:10", "-ExposureTime=1/250",
                                            "-FNumber=5.6", "-ISO=100", path}),
             0);
    const MediaInfo::Details d = MediaInfo::read(path, "image/jpeg");
    QCOMPARE(valueOf(d, "Dimensions"), QStringLiteral("640 × 480 pixels"));
    QCOMPARE(valueOf(d, "Camera"), QStringLiteral("Canon EOS R6"));
    QVERIFY(valueOf(d, "Exposure").contains("f/5.6"));
}

QTEST_GUILESS_MAIN(TestMediaInfo)
#include "tst_mediainfo.moc"
