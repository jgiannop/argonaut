#include "MediaInfo.h"

#include "Sandbox.h"

#include <QDateTime>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QRegularExpression>
#include <QStandardPaths>

namespace {

const QStringList kTags = {
    QStringLiteral("ImageSize"), QStringLiteral("Make"), QStringLiteral("Model"), QStringLiteral("LensModel"),
    QStringLiteral("DateTimeOriginal"), QStringLiteral("ExposureTime"), QStringLiteral("FNumber"), QStringLiteral("ISO"),
    QStringLiteral("FocalLength"), QStringLiteral("GPSPosition"), QStringLiteral("Duration"), QStringLiteral("AvgBitrate"),
    QStringLiteral("AudioBitrate"), QStringLiteral("SampleRate"), QStringLiteral("VideoFrameRate"), QStringLiteral("Title"),
    QStringLiteral("Artist"), QStringLiteral("Album"), QStringLiteral("Genre"), QStringLiteral("Track"),
    QStringLiteral("PageCount"), QStringLiteral("Author"), QStringLiteral("Creator")};

QString text(const QJsonObject &o, const QString &key)
{
    const QJsonValue v = o.value(key);
    if (v.isDouble())
        return QLocale::c().toString(v.toDouble(), 'g', 10);
    return v.toString().trimmed();
}

// "2024:08:12 14:33:10" (exiftool) -> the locale's date and time.
QString when(const QString &exif)
{
    QDateTime dt = QDateTime::fromString(exif.left(19), QStringLiteral("yyyy:MM:dd HH:mm:ss"));
    if (!dt.isValid())
        return exif;
    return QLocale().toString(dt, QLocale::LongFormat).remove(QRegularExpression(QStringLiteral(" [A-Z]{2,5}$")));
}

} // namespace

namespace MediaInfo {

bool interesting(const QString &mimeType)
{
    return mimeType.startsWith(QLatin1String("image/")) || mimeType.startsWith(QLatin1String("audio/"))
           || mimeType.startsWith(QLatin1String("video/")) || mimeType == QLatin1String("application/pdf");
}

Details fromExifToolJson(const QByteArray &json)
{
    Details d;
    const QJsonArray array = QJsonDocument::fromJson(json).array();
    if (array.isEmpty())
        return d;
    const QJsonObject o = array.first().toObject();
    auto add = [&d](const QString &label, const QString &value) {
        if (!value.isEmpty())
            d.append({label, value});
    };

    QString size = text(o, QStringLiteral("ImageSize"));
    size.replace(QLatin1Char('x'), QStringLiteral(" × "));
    if (!size.isEmpty())
        size += QObject::tr(" pixels");
    add(QObject::tr("Dimensions"), size);

    // "Canon" + "Canon EOS R6" -> "Canon EOS R6"
    const QString make = text(o, QStringLiteral("Make"));
    const QString model = text(o, QStringLiteral("Model"));
    add(QObject::tr("Camera"), model.startsWith(make, Qt::CaseInsensitive) ? model
                                                                          : (make + QLatin1Char(' ') + model).trimmed());
    add(QObject::tr("Lens"), text(o, QStringLiteral("LensModel")));
    const QString taken = text(o, QStringLiteral("DateTimeOriginal"));
    add(QObject::tr("Taken"), taken.isEmpty() ? QString() : when(taken));

    QStringList exposure;
    if (const QString t = text(o, QStringLiteral("ExposureTime")); !t.isEmpty())
        exposure << t + QStringLiteral(" s");
    if (const QString f = text(o, QStringLiteral("FNumber")); !f.isEmpty())
        exposure << QStringLiteral("f/") + f;
    if (const QString iso = text(o, QStringLiteral("ISO")); !iso.isEmpty())
        exposure << QStringLiteral("ISO ") + iso;
    if (const QString fl = text(o, QStringLiteral("FocalLength")); !fl.isEmpty())
        exposure << fl;
    add(QObject::tr("Exposure"), exposure.join(QStringLiteral(" · ")));
    add(QObject::tr("Location"), text(o, QStringLiteral("GPSPosition")));

    add(QObject::tr("Title"), text(o, QStringLiteral("Title")));
    add(QObject::tr("Artist"), text(o, QStringLiteral("Artist")));
    add(QObject::tr("Album"), text(o, QStringLiteral("Album")));
    add(QObject::tr("Genre"), text(o, QStringLiteral("Genre")));
    add(QObject::tr("Track"), text(o, QStringLiteral("Track")));
    QString duration = text(o, QStringLiteral("Duration"));
    duration.remove(QStringLiteral(" (approx)"));
    add(QObject::tr("Duration"), duration);
    if (QString fps = text(o, QStringLiteral("VideoFrameRate")); !fps.isEmpty())
        add(QObject::tr("Frame rate"), fps + QObject::tr(" frames per second"));
    QString bitrate = text(o, QStringLiteral("AvgBitrate"));
    if (bitrate.isEmpty())
        bitrate = text(o, QStringLiteral("AudioBitrate"));
    add(QObject::tr("Bitrate"), bitrate);
    if (const QString rate = text(o, QStringLiteral("SampleRate")); !rate.isEmpty())
        add(QObject::tr("Sample rate"), rate + QStringLiteral(" Hz"));

    add(QObject::tr("Pages"), text(o, QStringLiteral("PageCount")));
    add(QObject::tr("Author"), text(o, QStringLiteral("Author")));
    add(QObject::tr("Created with"), text(o, QStringLiteral("Creator")));
    return d;
}

Details read(const QString &path, const QString &mimeType)
{
    if (!QStandardPaths::findExecutable(QStringLiteral("exiftool")).isEmpty() && Sandbox::available()) {
        QStringList args{QStringLiteral("-json"), QStringLiteral("-charset"), QStringLiteral("filename=utf8")};
        for (const QString &t : kTags)
            args << QLatin1Char('-') + t;
        args << QStringLiteral("@FILE@");
        bool ok = false;
        const QByteArray out = Sandbox::run(QStringLiteral("exiftool"), args, path, &ok);
        if (ok) {
            const Details d = fromExifToolJson(out);
            if (!d.isEmpty())
                return d;
        }
    }
    // Without exiftool: at least an image's size (Qt reads the header only).
    Details d;
    if (mimeType.startsWith(QLatin1String("image/"))) {
        QImageReader reader(path);
        const QSize size = reader.size();
        if (size.isValid())
            d.append({QObject::tr("Dimensions"), QObject::tr("%1 × %2 pixels").arg(size.width()).arg(size.height())});
    }
    return d;
}

} // namespace MediaInfo
