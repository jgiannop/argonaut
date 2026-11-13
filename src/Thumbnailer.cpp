#include "Thumbnailer.h"

#include "FileJob.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QMimeDatabase>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>

#if defined(Q_OS_UNIX)
#include <unistd.h>
#endif

namespace {

constexpr int ProgramTimeoutMs = 15000;

QImage scaledDown(const QImage &img)
{
    if (img.width() <= Thumbnailer::MaxSize && img.height() <= Thumbnailer::MaxSize)
        return img;
    return img.scaled(Thumbnailer::MaxSize, Thumbnailer::MaxSize, Qt::KeepAspectRatio,
                      Qt::SmoothTransformation);
}

QString cacheBase()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/thumbnails/";
}

QString uriHash(const QString &path)
{
    const QByteArray uri = QUrl::fromLocalFile(path).toEncoded();
    return QString::fromLatin1(QCryptographicHash::hash(uri, QCryptographicHash::Md5).toHex());
}

QString failPathFor(const QString &path)
{
    return cacheBase() + "fail/kaftilous/" + uriHash(path) + ".png";
}

bool matchesFile(const QImage &img, const QDateTime &mtime)
{
    const QString thumbMtime = img.text("Thumb::MTime");
    return thumbMtime.isEmpty() || thumbMtime.toLongLong() == mtime.toSecsSinceEpoch();
}

enum class CacheResult { Found, Failed, Missing };

// Freedesktop thumbnail spec: <cache>/thumbnails/<size>/<md5(uri)>.png
CacheResult fromSharedCache(const QString &path, const QDateTime &mtime, QImage *out)
{
#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
    const QString hash = uriHash(path);
    for (const char *dir : {"large", "x-large", "normal", "xx-large"}) {
        QImage img;
        if (!img.load(cacheBase() + QLatin1String(dir) + '/' + hash + ".png"))
            continue;
        if (!matchesFile(img, mtime))
            continue; // stale
        *out = scaledDown(img);
        return CacheResult::Found;
    }
    QImage failed;
    if (failed.load(failPathFor(path)) && matchesFile(failed, mtime))
        return CacheResult::Failed;
#else
    Q_UNUSED(path)
    Q_UNUSED(mtime)
    Q_UNUSED(out)
#endif
    return CacheResult::Missing;
}

QImage decodeImage(const QString &path)
{
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

// Saves `img` as the spec says: PNG with the URI and mtime, private, atomic.
bool saveToCache(QImage img, const QString &target, const QString &path, const QDateTime &mtime)
{
    QDir().mkpath(QFileInfo(target).absolutePath());
    QFile::setPermissions(cacheBase(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    img.setText("Thumb::URI", QString::fromUtf8(QUrl::fromLocalFile(path).toEncoded()));
    img.setText("Thumb::MTime", QString::number(mtime.toSecsSinceEpoch()));
    img.setText("Thumb::Size", QString::number(QFileInfo(path).size()));
    img.setText("Software", "Kaftilous");
    const QString temp = target + QStringLiteral(".kaftilous-%1.tmp").arg(QCoreApplication::applicationPid());
    if (!img.save(temp, "PNG"))
        return false;
    QFile::setPermissions(temp, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    QFile::remove(target);
    return QFile::rename(temp, target);
}

// Runs one thumbnailer for `path` and returns its image (null on failure).
QImage runProgram(const Thumbnailer::Program &program, const QString &path, const QString &mimeType)
{
    QTemporaryDir work(QDir::tempPath() + "/kaftilous-thumb-XXXXXX");
    if (!work.isValid())
        return {};
    const bool sandbox = Thumbnailer::sandboxAvailable();
    // Inside the sandbox the file is bound under the name GNOME uses (Ubuntu's
    // AppArmor profiles for thumbnailers allow exactly these paths), with an
    // extension that matches its real type.
    static const QMimeDatabase db;
    QString suffix = db.mimeTypeForName(mimeType).preferredSuffix();
    if (suffix.isEmpty())
        suffix = QFileInfo(path).suffix();
    const QString input = sandbox ? "/tmp/gnome-desktop-file-to-thumbnail." + suffix : path;
    const QString output = sandbox ? QStringLiteral("/tmp/gnome-desktop-thumbnailer.png")
                                   : work.path() + "/thumbnail.png";

    QStringList args;
    for (const QString &token : QProcess::splitCommand(program.exec)) {
        QString arg;
        for (int i = 0; i < token.size(); ++i) {
            if (token.at(i) != QLatin1Char('%') || i + 1 >= token.size()) {
                arg += token.at(i);
                continue;
            }
            switch (token.at(++i).toLatin1()) {
            case 's': arg += QString::number(Thumbnailer::GenerateSize); break;
            case 'u': arg += QString::fromUtf8(QUrl::fromLocalFile(input).toEncoded()); break;
            case 'i': arg += input; break;
            case 'o': arg += output; break;
            case '%': arg += QLatin1Char('%'); break;
            default: break;
            }
        }
        args << arg;
    }
    if (args.isEmpty())
        return {};
    args[0] = program.tryExec.isEmpty() ? args[0] : program.tryExec;

    QProcess process;
    process.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardInputFile(QProcess::nullDevice());
#if defined(Q_OS_UNIX)
    process.setChildProcessModifier([] {
        if (::nice(10) == -1) {
            // not lowered: runs at normal priority
        }
    }); // don't compete with the user
#endif
    if (sandbox) {
        const QString home = QDir::homePath();
        QStringList box = {
            "--ro-bind", "/usr", "/usr",
            "--ro-bind-try", "/etc", "/etc",
            "--ro-bind-try", "/var/cache/fontconfig", "/var/cache/fontconfig",
            "--ro-bind-try", home + "/.cache/fontconfig", home + "/.cache/fontconfig",
            "--ro-bind-try", home + "/.cache/gstreamer-1.0", home + "/.cache/gstreamer-1.0",
            "--proc", "/proc", "--dev", "/dev", "--chdir", "/",
            "--setenv", "GIO_USE_VFS", "local", "--setenv", "HOME", home,
            "--unshare-all", "--die-with-parent", "--new-session",
            "--bind", work.path(), "/tmp",
            "--ro-bind", path, input,
        };
        // Merged-/usr systems: /bin, /lib… are links into /usr.
        for (const char *link : {"bin", "sbin", "lib", "lib32", "lib64", "libx32"}) {
            const QFileInfo info(QStringLiteral("/") + QLatin1String(link));
            if (info.isSymLink())
                box << "--symlink" << info.symLinkTarget().mid(1) << info.filePath();
            else if (info.isDir())
                box << "--ro-bind" << info.filePath() << info.filePath();
        }
        process.start(QStringLiteral("bwrap"), box + args);
    } else {
        process.start(args.takeFirst(), args);
    }
    if (!process.waitForStarted(5000))
        return {};
    if (!process.waitForFinished(ProgramTimeoutMs)) {
        process.kill(); // bwrap --die-with-parent takes the rest down
        process.waitForFinished(1000);
        return {};
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return {};
    return QImage(sandbox ? work.path() + "/gnome-desktop-thumbnailer.png" : output);
}

// Keys of the [Thumbnailer Entry] group. A desktop-entry file, not INI:
// QSettings would read ';' as the start of a comment and drop every type
// but the first.
QHash<QString, QString> readThumbnailerEntry(const QString &file)
{
    QHash<QString, QString> keys;
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return keys;
    bool inGroup = false;
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        if (line.startsWith(QLatin1Char('['))) {
            inGroup = line == QLatin1String("[Thumbnailer Entry]");
            continue;
        }
        const int eq = int(line.indexOf(QLatin1Char('=')));
        if (inGroup && eq > 0)
            keys.insert(line.left(eq).trimmed(), line.mid(eq + 1).trimmed());
    }
    return keys;
}

} // namespace

// ---------------------------------------------------------------- policy ---

Thumbnailer::Policy Thumbnailer::policy()
{
    const QString value = QSettings().value("thumbnails/generate", "local").toString();
    if (value == QLatin1String("always"))
        return Policy::Everywhere;
    if (value == QLatin1String("never"))
        return Policy::Never;
    return Policy::LocalOnly;
}

void Thumbnailer::setPolicy(Policy policy)
{
    QSettings().setValue("thumbnails/generate", policy == Policy::Everywhere ? "always"
                                                : policy == Policy::Never    ? "never"
                                                                             : "local");
}

bool Thumbnailer::sandboxAvailable()
{
#if defined(Q_OS_LINUX)
    if (qEnvironmentVariable("KAFTILOUS_THUMBNAIL_SANDBOX") == QLatin1String("0"))
        return false; // tests with fake thumbnailers outside /usr
    static const bool found = !QStandardPaths::findExecutable("bwrap").isEmpty();
    return found;
#else
    return false;
#endif
}

QString Thumbnailer::cachePathFor(const QString &path, const char *sizeDir)
{
    return cacheBase() + QLatin1String(sizeDir) + '/' + uriHash(path) + ".png";
}

// ------------------------------------------------------------ lifecycle ---

Thumbnailer::Thumbnailer(QObject *parent)
    : QObject(parent)
    , m_cache(96 * 1024) // ~96 MiB
{
    m_pool.setMaxThreadCount(qBound(1, QThread::idealThreadCount() / 2, 4));
    m_external.setMaxThreadCount(2);
    for (const QByteArray &fmt : QImageReader::supportedImageFormats())
        m_imageSuffixes.insert(QString::fromLatin1(fmt).toLower());
    m_imageSuffixes.insert("jpeg");

#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
    // Types other apps' thumbnailers (Nautilus, Tumbler…) commonly put in
    // the shared cache, even when we have no thumbnailer for them.
    for (const char *s : {"pdf", "epub", "djvu", "ps", "eps", "svg", "svgz", "webp", "avif",
                          "heic", "heif", "jxl", "tif", "tiff", "psd", "xcf", "kra", "ora",
                          "raw", "cr2", "cr3", "nef", "arw", "dng", "orf", "rw2", "mp4", "m4v",
                          "mkv", "webm", "avi", "mov", "wmv", "flv", "mpg", "mpeg", "ogv", "3gp",
                          "ts", "odt", "ods", "odp", "odg", "docx", "xlsx", "pptx", "doc", "xls",
                          "ppt", "ttf", "otf", "blend", "stl", "3mf", "mobi", "cbz", "cbr"})
        m_cachedSuffixes.insert(QString::fromLatin1(s));
    loadPrograms();
#endif
}

Thumbnailer::~Thumbnailer()
{
    m_pool.clear();
    m_external.clear();
    m_pool.waitForDone();
    m_external.waitForDone();
}

// /usr/share/thumbnailers/*.thumbnailer (and the other XDG data dirs; the
// first one found for a type wins, so ~/.local/share overrides the system).
void Thumbnailer::loadPrograms()
{
    const QStringList dirs = QStandardPaths::locateAll(QStandardPaths::GenericDataLocation,
                                                       QStringLiteral("thumbnailers"),
                                                       QStandardPaths::LocateDirectory);
    static const QMimeDatabase db;
    for (const QString &dir : dirs) {
        for (const QFileInfo &file : QDir(dir).entryInfoList({"*.thumbnailer"}, QDir::Files, QDir::Name)) {
            const QHash<QString, QString> entry = readThumbnailerEntry(file.absoluteFilePath());
            const QString exec = entry.value("Exec");
            QString tryExec = entry.value("TryExec");
            const QStringList mimes = entry.value("MimeType").split(QLatin1Char(';'), Qt::SkipEmptyParts);
            if (exec.isEmpty() || mimes.isEmpty())
                continue;
            if (tryExec.isEmpty())
                tryExec = QProcess::splitCommand(exec).value(0);
            if (!QFileInfo(tryExec).isAbsolute())
                tryExec = QStandardPaths::findExecutable(tryExec);
            if (tryExec.isEmpty() || !QFileInfo(tryExec).isExecutable())
                continue; // not installed
            for (const QString &mime : std::as_const(mimes)) {
                const QString name = db.mimeTypeForName(mime.trimmed()).name(); // resolves aliases
                const QString key = name.isEmpty() ? mime.trimmed() : name;
                if (!m_programs.contains(key))
                    m_programs.insert(key, {exec, tryExec});
            }
        }
    }
}

bool Thumbnailer::canGenerate(const QString &mimeType) const
{
    return !mimeType.isEmpty() && m_programs.contains(mimeType);
}

bool Thumbnailer::allowedHere(const QString &path)
{
    switch (policy()) {
    case Policy::Never:
        return false;
    case Policy::Everywhere:
        return true;
    case Policy::LocalOnly:
        break;
    }
    const QString dir = QFileInfo(path).absolutePath();
    auto it = m_localDir.constFind(dir);
    if (it == m_localDir.constEnd())
        it = m_localDir.insert(dir, !needsSafeWrites(dir) && !isNetworkFilesystem(dir));
    return *it;
}

// --------------------------------------------------------------- lookup ---

QPixmap Thumbnailer::thumbnail(const QString &path, const QDateTime &mtime, const QString &mimeType)
{
    // Cheap checks first: most files (text, logs, code…) never have one.
    const int dot = int(path.lastIndexOf(QLatin1Char('.')));
    const QString suffix = dot > path.lastIndexOf(QLatin1Char('/')) ? path.mid(dot + 1).toLower() : QString();
    const bool decodable = m_imageSuffixes.contains(suffix);
    const bool generatable = canGenerate(mimeType);
    if (!decodable && !generatable && !m_cachedSuffixes.contains(suffix))
        return {};

    if (const Entry *e = m_cache.object(path); e && e->mtime == mtime)
        return e->pixmap;
    if (m_pending.contains(path))
        return {};
    // Never our own cache folder (thumbnails of thumbnails).
    if (path.startsWith(cacheBase()))
        return {};

    m_pending.insert(path);
    m_pool.start([this, path, mtime, decodable, generatable, mimeType] {
        QImage img;
        const CacheResult cached = fromSharedCache(path, mtime, &img);
        if (cached == CacheResult::Missing && decodable)
            img = decodeImage(path);
        const bool tryProgram = img.isNull() && cached == CacheResult::Missing && generatable;
        QMetaObject::invokeMethod(
            this,
            [this, path, mtime, img, tryProgram, mimeType] {
                if (tryProgram && allowedHere(path))
                    generate(path, mtime, mimeType);
                else
                    finished(path, mtime, img);
            },
            Qt::QueuedConnection);
    });
    return {};
}

void Thumbnailer::finished(const QString &path, const QDateTime &mtime, const QImage &img)
{
    m_pending.remove(path);
    m_queued.remove(path);
    auto *entry = new Entry{img.isNull() ? QPixmap() : QPixmap::fromImage(img), mtime};
    const int cost = qMax<qsizetype>(1, img.sizeInBytes() / 1024);
    m_cache.insert(path, entry, cost);
    if (!img.isNull())
        emit thumbnailReady(path);
}

void Thumbnailer::generate(const QString &path, const QDateTime &mtime, const QString &mimeType)
{
    const Program program = m_programs.value(mimeType);
    m_queued.insert(path);
    m_external.start(
        [this, path, mtime, mimeType, program] {
            QImage made = runProgram(program, path, mimeType);
            // Store it for every app (and us, next time), or remember the failure.
            if (!made.isNull()) {
                saveToCache(made, cachePathFor(path), path, mtime);
                m_generated.fetchAndAddRelaxed(1);
            } else if (QFileInfo::exists(path)) {
                QImage marker(1, 1, QImage::Format_ARGB32);
                marker.fill(Qt::transparent);
                saveToCache(marker, failPathFor(path), path, mtime);
            }
            const QImage img = made.isNull() ? QImage() : scaledDown(made);
            QMetaObject::invokeMethod(this, [this, path, mtime, img] { finished(path, mtime, img); },
                                      Qt::QueuedConnection);
        },
        ++m_priority); // the most recently painted first
}

void Thumbnailer::cancelPending()
{
    m_external.clear(); // drops runs that haven't started
    for (const QString &path : std::as_const(m_queued))
        m_pending.remove(path); // asked for again if they're painted again
    m_queued.clear();
}
