#pragma once

#include <QCache>
#include <QDateTime>
#include <QObject>
#include <QPixmap>
#include <QSet>
#include <QThreadPool>

class QFileInfo;

// Loads file thumbnails in background threads.
//  1. On Linux, reuses the shared freedesktop cache (~/.cache/thumbnails)
//     that Nautilus and other apps already filled (images, PDFs, videos…).
//  2. Otherwise decodes image files itself with QImageReader.
class Thumbnailer : public QObject
{
    Q_OBJECT

public:
    static constexpr int MaxSize = 128; // px, enough for 48px icons at 2x

    explicit Thumbnailer(QObject *parent = nullptr);
    ~Thumbnailer() override;

    // Returns the thumbnail if ready; otherwise starts loading it (once)
    // and returns a null pixmap. thumbnailReady() fires when it arrives.
    QPixmap thumbnail(const QFileInfo &info);

signals:
    void thumbnailReady(const QString &path);

private:
    struct Entry {
        QPixmap pixmap; // null while pending or when there is no thumbnail
        QDateTime mtime;
    };

    QCache<QString, Entry> m_cache; // cost in KiB
    QSet<QString> m_pending;
    QSet<QString> m_imageSuffixes;
    QThreadPool m_pool;
};
