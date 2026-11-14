#pragma once

#include <QAtomicInteger>
#include <QCache>
#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QPixmap>
#include <QSet>
#include <QThreadPool>

// Loads file thumbnails in background threads.
//  1. On Linux, reuses the shared freedesktop cache (~/.cache/thumbnails)
//     that Nautilus and other apps fill (and that we fill too, see 3).
//  2. Decodes image files itself with QImageReader.
//  3. On Linux, makes missing ones for PDFs, videos, fonts… with the
//     thumbnailers the system has installed (/usr/share/thumbnailers/
//     *.thumbnailer, the same ones Nautilus uses), and saves them in the
//     shared cache, so each file is done once, for every app.
//     - They run in a bubblewrap sandbox when `bwrap` exists (no network,
//       nothing visible but the one file), like GNOME does.
//     - At most two at a time, at low CPU priority, newest request first;
//       cancelPending() drops what hasn't started (leaving the folder).
//     - Only for what the views paint, i.e. visible files.
//     - By default only on local disks (policy()), not USB sticks or
//       network drives, where reading a video to thumbnail it is slow.
//     - Failures are remembered (~/.cache/thumbnails/fail/argonaut/) until
//       the file changes.
class Thumbnailer : public QObject
{
    Q_OBJECT

public:
    static constexpr int MaxSize = 256;      // px kept in memory: up to 128 px icons at 2x
    static constexpr int GenerateSize = 256; // px made by thumbnailers ("large" cache folder)

    enum class Policy { LocalOnly, Everywhere, Never };
    static Policy policy(); // QSettings "thumbnails/generate"
    static void setPolicy(Policy policy);

    explicit Thumbnailer(QObject *parent = nullptr);
    ~Thumbnailer() override;

    // Returns the thumbnail if ready; otherwise starts loading it (once)
    // and returns a null pixmap. thumbnailReady() fires when it arrives.
    // `path` must be a regular file; `mtime` its modification time and
    // `mimeType` its real type (the model already knows them, so this
    // never touches the disk itself).
    QPixmap thumbnail(const QString &path, const QDateTime &mtime, const QString &mimeType = {});
    // Forget queued thumbnailer runs (they'll be asked for again if needed).
    void cancelPending();

    // For tests and Properties.
    bool canGenerate(const QString &mimeType) const;
    // A large preview (Quick Look) made by the system thumbnailer for
    // `mimeType`, at up to `size` px, sandboxed, not cached. Blocks: call it
    // from a worker thread. Safe from any thread (the program list is fixed).
    QImage render(const QString &path, const QString &mimeType, int size) const;
    static bool sandboxAvailable();
    static QString cachePathFor(const QString &path, const char *sizeDir = "large");
    int generatedCount() const { return m_generated.loadRelaxed(); }

    struct Program
    {
        QString exec;    // Exec= line
        QString tryExec; // resolved executable
    };

signals:
    void thumbnailReady(const QString &path);

private:
    struct Entry {
        QPixmap pixmap; // null while pending or when there is no thumbnail
        QDateTime mtime;
    };
    void loadPrograms();
    void finished(const QString &path, const QDateTime &mtime, const QImage &img);
    void generate(const QString &path, const QDateTime &mtime, const QString &mimeType);
    bool allowedHere(const QString &path);

    QCache<QString, Entry> m_cache; // cost in KiB
    QSet<QString> m_pending;
    QSet<QString> m_queued;          // waiting for a thumbnailer (cancelable)
    QSet<QString> m_imageSuffixes;   // we can decode these ourselves
    QSet<QString> m_cachedSuffixes;  // may exist in the shared thumbnail cache
    QHash<QString, Program> m_programs; // mime type -> thumbnailer
    QHash<QString, bool> m_localDir;    // folder -> on a local disk
    QThreadPool m_pool;     // cache reads and image decoding
    QThreadPool m_external; // thumbnailer programs (2 at a time)
    int m_priority = 0;     // rises with each request: newest first
    QAtomicInteger<int> m_generated{0};
};
