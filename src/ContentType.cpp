#include "ContentType.h"

#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>

namespace {

QAtomicInt s_reads;
constexpr int MaxCacheEntries = 200000;
constexpr quint32 CacheMagic = 0x4b464354; // "KFCT"
constexpr qint32 CacheVersion = 1;

const QMimeDatabase &db()
{
    static const QMimeDatabase database;
    return database;
}

bool sameKind(const QMimeType &a, const QMimeType &b)
{
    return a == b || a.inherits(b.name()) || b.inherits(a.name());
}

} // namespace

namespace ContentType {

// Things that run code when opened. A document, image or archive whose
// content is one of these is disguised.
bool isProgram(const QMimeType &type)
{
    static const char *const programs[] = {
        "application/x-executable",
        "application/x-pie-executable",
        "application/x-sharedlib",
        "application/x-elf",         // what a Linux program is called where the system has no type list of its own (Windows: Qt's)
        "application/x-mach-binary", // a macOS program
        "application/x-msdownload",
        "application/x-ms-dos-executable",
        "application/x-dosexec",
        "application/vnd.microsoft.portable-executable",
        "application/x-msi",
        "application/x-shellscript",
        "application/x-desktop",
        "application/vnd.appimage",
        "application/x-makeself",
        "application/x-ms-shortcut",
        "application/x-bat",
        "application/x-msdos-program",
    };
    if (!type.isValid())
        return false;
    for (const char *name : programs) {
        if (type.name() == QLatin1String(name) || type.inherits(QLatin1String(name)))
            return true;
    }
    return false;
}

Verdict judge(const QMimeType &byName, const QMimeType &byContent)
{
    // Content that says nothing definite: empty files, unknown binary data.
    if (!byContent.isValid() || byContent.isDefault()
        || byContent.name() == QLatin1String("application/x-zerosize"))
        return Verdict::Consistent;
    if (!byName.isValid() || byName.isDefault())
        return Verdict::Identified;
    if (sameKind(byName, byContent))
        return Verdict::Consistent;
    // Text is text: content sniffing can't reliably tell Markdown from
    // Python from CSV, so two text types never count as a mismatch…
    const bool nameIsText = byName.inherits(QStringLiteral("text/plain"));
    const bool contentIsText = byContent.name() == QLatin1String("text/plain")
                               || byContent.inherits(QStringLiteral("text/plain"));
    if (nameIsText && contentIsText && !isProgram(byContent))
        return Verdict::Consistent;
    // …but a program dressed up as something else always does.
    if (isProgram(byContent) && !isProgram(byName))
        return Verdict::Dangerous;
    return Verdict::Mismatch;
}

Result inspectData(const QString &fileName, const QByteArray &head)
{
    Result result;
    const QMimeType byName = db().mimeTypeForFile(fileName, QMimeDatabase::MatchExtension);
    const QMimeType byContent = db().mimeTypeForData(head);
    result.nameMime = byName.name();
    result.contentMime = head.isEmpty() ? QStringLiteral("application/x-zerosize") : byContent.name();
    result.verdict = judge(byName, head.isEmpty() ? db().mimeTypeForName(result.contentMime) : byContent);
    return result;
}

Result inspect(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        Result unreadable;
        unreadable.nameMime =
            db().mimeTypeForFile(QFileInfo(path).fileName(), QMimeDatabase::MatchExtension).name();
        unreadable.verdict = Verdict::Consistent; // nothing to say
        return unreadable;
    }
    s_reads.fetchAndAddRelaxed(1);
    return inspectData(QFileInfo(path).fileName(), file.read(HeadSize));
}

int readCount()
{
    return s_reads.loadRelaxed();
}

QString effectiveMime(const Result &result)
{
    switch (result.verdict) {
    case Verdict::Identified:
    case Verdict::Mismatch:
    case Verdict::Dangerous:
        return result.contentMime;
    default:
        return result.nameMime;
    }
}

QString describe(const Result &result)
{
    const QString named = db().mimeTypeForName(result.nameMime).comment();
    const QString content = db().mimeTypeForName(result.contentMime).comment();
    switch (result.verdict) {
    case Verdict::Mismatch:
        return QCoreApplication::translate("ContentType", "Named as %1, but the content is %2")
            .arg(named, content);
    case Verdict::Dangerous:
        return QCoreApplication::translate("ContentType",
                                           "Warning: named as %1, but this is a program (%2)")
            .arg(named, content);
    case Verdict::Identified:
        return QCoreApplication::translate("ContentType", "%1 (recognised from its content)")
            .arg(content);
    default:
        return {};
    }
}

QString fixedName(const QString &fileName, const QString &contentMime)
{
    const QString suffix = db().mimeTypeForName(contentMime).preferredSuffix();
    if (suffix.isEmpty())
        return {};
    QString base = fileName;
    const int dot = int(fileName.lastIndexOf(QLatin1Char('.')));
    if (dot > 0)
        base = fileName.left(dot);
    const QString fixed = base + QLatin1Char('.') + suffix;
    return fixed == fileName ? QString() : fixed;
}

// ---------------------------------------------------------------- cache ---

Cache &Cache::instance()
{
    static Cache *cache = new Cache; // lives until exit; saved on aboutToQuit
    return *cache;
}

Cache::Cache()
{
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(3000);
    connect(&m_saveTimer, &QTimer::timeout, this, &Cache::save);
    if (QCoreApplication::instance())
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, &Cache::save);
}

QString Cache::filePath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
           + QStringLiteral("/content-types.bin");
}

void Cache::load()
{
    m_loaded = true;
    QFile file(filePath());
    if (!file.open(QIODevice::ReadOnly))
        return;
    QDataStream in(&file);
    quint32 magic = 0;
    qint32 version = 0;
    in >> magic >> version;
    if (magic != CacheMagic || version != CacheVersion)
        return; // other format: start over
    qint32 count = 0;
    in >> count;
    m_entries.reserve(count);
    for (qint32 i = 0; i < count && in.status() == QDataStream::Ok; ++i) {
        QString path;
        Entry e;
        in >> path >> e.size >> e.mtime >> e.nameMime >> e.contentMime >> e.verdict >> e.stamp;
        m_entries.insert(path, e);
        m_nextStamp = std::max(m_nextStamp, e.stamp + 1);
    }
}

bool Cache::lookup(const QString &path, qint64 size, qint64 mtime, Result *result)
{
    if (!m_loaded)
        load();
    const auto it = m_entries.constFind(path);
    if (it == m_entries.constEnd() || it->size != size || it->mtime != mtime)
        return false;
    result->nameMime = it->nameMime;
    result->contentMime = it->contentMime;
    result->verdict = Verdict(it->verdict);
    return true;
}

void Cache::store(const QString &path, qint64 size, qint64 mtime, const Result &result)
{
    if (!m_loaded)
        load();
    m_entries.insert(path, {size, mtime, result.nameMime, result.contentMime,
                            quint8(result.verdict), m_nextStamp++});
    m_dirty = true;
    m_saveTimer.start();
}

void Cache::save()
{
    if (!m_dirty)
        return;
    // Full: keep the most recently checked files.
    if (m_entries.size() > MaxCacheEntries) {
        QList<quint32> stamps;
        stamps.reserve(m_entries.size());
        for (const Entry &e : std::as_const(m_entries))
            stamps.append(e.stamp);
        std::nth_element(stamps.begin(), stamps.begin() + (stamps.size() - MaxCacheEntries),
                         stamps.end());
        const quint32 cutoff = stamps.at(stamps.size() - MaxCacheEntries);
        m_entries.removeIf([cutoff](const auto &it) { return it.value().stamp < cutoff; });
    }
    QDir().mkpath(QFileInfo(filePath()).absolutePath());
    QSaveFile file(filePath());
    if (!file.open(QIODevice::WriteOnly))
        return;
    QDataStream out(&file);
    out << CacheMagic << CacheVersion << qint32(m_entries.size());
    for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it) {
        const Entry &e = it.value();
        out << it.key() << e.size << e.mtime << e.nameMime << e.contentMime << e.verdict << e.stamp;
    }
    if (file.commit())
        m_dirty = false;
}

} // namespace ContentType
