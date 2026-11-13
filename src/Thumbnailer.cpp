#include "Thumbnailer.h"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QStandardPaths>
#include <QUrl>

namespace {

QImage scaledDown(const QImage &img)
{
    if (img.width() <= Thumbnailer::MaxSize && img.height() <= Thumbnailer::MaxSize)
        return img;
    return img.scaled(Thumbnailer::MaxSize, Thumbnailer::MaxSize, Qt::KeepAspectRatio,
                      Qt::SmoothTransformation);
}

QImage loadThumbnail(const QString &path, const QDateTime &mtime, bool decodable)
{
#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
    // Freedesktop thumbnail spec: <cache>/thumbnails/<size>/<md5(uri)>.png
    const QByteArray uri = QUrl::fromLocalFile(path).toEncoded();
    const QString hash =
        QString::fromLatin1(QCryptographicHash::hash(uri, QCryptographicHash::Md5).toHex());
    const QString base =
        QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/thumbnails/";
    for (const char *dir : {"large", "x-large", "normal", "xx-large"}) {
        QImage img;
        if (!img.load(base + QLatin1String(dir) + '/' + hash + ".png"))
            continue;
        const QString thumbMtime = img.text("Thumb::MTime");
        if (!thumbMtime.isEmpty() && thumbMtime.toLongLong() != mtime.toSecsSinceEpoch())
            continue; // stale
        return scaledDown(img);
    }
#else
    Q_UNUSED(mtime)
#endif

    if (!decodable)
        return {};
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    if (size.isValid()) {
        if (qint64(size.width()) * size.height() > 150'000'000)
            return {}; // absurdly large, skip
        if (size.width() > Thumbnailer::MaxSize || size.height() > Thumbnailer::MaxSize)
            reader.setScaledSize(size.scaled(Thumbnailer::MaxSize, Thumbnailer::MaxSize,
                                             Qt::KeepAspectRatio));
    }
    return scaledDown(reader.read());
}

} // namespace

Thumbnailer::Thumbnailer(QObject *parent)
    : QObject(parent)
    , m_cache(64 * 1024) // ~64 MiB
{
    m_pool.setMaxThreadCount(qBound(1, QThread::idealThreadCount() / 2, 4));
    for (const QByteArray &fmt : QImageReader::supportedImageFormats())
        m_imageSuffixes.insert(QString::fromLatin1(fmt).toLower());
    m_imageSuffixes.insert("jpeg");
}

Thumbnailer::~Thumbnailer()
{
    m_pool.clear();
    m_pool.waitForDone();
}

QPixmap Thumbnailer::thumbnail(const QFileInfo &info)
{
    if (!info.isFile())
        return {};
    const QString path = info.absoluteFilePath();
    const QDateTime mtime = info.lastModified();

    if (const Entry *e = m_cache.object(path); e && e->mtime == mtime)
        return e->pixmap;
    if (m_pending.contains(path))
        return {};

    const bool decodable = m_imageSuffixes.contains(info.suffix().toLower());
#if !defined(Q_OS_LINUX) && !defined(Q_OS_FREEBSD)
    if (!decodable)
        return {};
#endif

    m_pending.insert(path);
    m_pool.start([this, path, mtime, decodable] {
        const QImage img = loadThumbnail(path, mtime, decodable);
        QMetaObject::invokeMethod(
            this,
            [this, path, mtime, img] {
                m_pending.remove(path);
                auto *entry = new Entry{img.isNull() ? QPixmap() : QPixmap::fromImage(img), mtime};
                const int cost = qMax<qsizetype>(1, img.sizeInBytes() / 1024);
                m_cache.insert(path, entry, cost);
                if (!img.isNull())
                    emit thumbnailReady(path);
            },
            Qt::QueuedConnection);
    });
    return {};
}
