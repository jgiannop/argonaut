#include "Preview.h"

#include "FileOperations.h"
#include "Thumbnailer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QMimeDatabase>
#include <QStringDecoder>

namespace Preview {

bool isTextual(const QString &mimeType)
{
    static const QMimeDatabase db;
    const QMimeType type = db.mimeTypeForName(mimeType);
    if (!type.isValid())
        return false;
    if (type.inherits(QStringLiteral("text/plain")))
        return true;
    // Text formats that don't say they're text.
    static const QStringList also = {"application/json", "application/xml", "application/javascript",
                                     "application/x-yaml", "application/toml", "application/sql",
                                     "application/x-desktop", "application/x-ipynb+json"};
    for (const QString &name : also) {
        if (type.name() == name || type.inherits(name))
            return true;
    }
    return false;
}

namespace {

Result fromImage(const QString &path, const QSize &box)
{
    Result r;
    QImageReader reader(path); // decides by content, not by name
    reader.setAutoTransform(true);
    if (!reader.canRead())
        return r;
    if (reader.supportsAnimation() && reader.imageCount() > 1) {
        r.animated = true;
        r.kind = Kind::Image;
        r.image = reader.read(); // first frame, for its size
        return r;
    }
    const QSize size = reader.size();
    if (size.isValid()) {
        if (qint64(size.width()) * size.height() > 250'000'000)
            return r; // absurdly large
        if (size.width() > box.width() || size.height() > box.height())
            reader.setScaledSize(size.scaled(box, Qt::KeepAspectRatio));
    }
    r.image = reader.read();
    if (!r.image.isNull())
        r.kind = Kind::Image;
    return r;
}

Result fromText(const QString &path)
{
    Result r;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return r;
    const QByteArray bytes = file.read(MaxTextBytes);
    if (bytes.contains('\0'))
        return r; // binary after all
    QStringDecoder utf8(QStringDecoder::Utf8);
    QString text = utf8.decode(bytes);
    if (utf8.hasError())
        text = QString::fromLocal8Bit(bytes);
    r.kind = Kind::Text;
    r.text = text;
    r.truncated = file.size() > MaxTextBytes;
    return r;
}

} // namespace

Result load(const QString &path, const QString &mimeType, ContentType::Verdict verdict, const QSize &box,
            const Thumbnailer *thumbnailer)
{
    Result r;
    const QFileInfo info(path);
    if (verdict == ContentType::Verdict::Dangerous) {
        r.kind = Kind::Blocked; // never handed to anything
        return r;
    }
    if (info.isDir()) {
        r.kind = Kind::Folder;
        r.items = int(QDir(path).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System).size());
        return r;
    }
    if (!info.isFile() || !info.isReadable())
        return r;

    if (FileOperations::isArchive(mimeType)) {
        int total = 0;
        if (FileJob::listArchive(path, MaxArchiveEntries, &r.entries, &total)) {
            r.kind = Kind::Archive;
            r.total = total;
            r.truncated = total > int(r.entries.size());
            return r;
        }
    }
    if (mimeType.startsWith(QLatin1String("image/"))) {
        r = fromImage(path, box);
        if (r.kind == Kind::Image)
            return r;
    }
    if (isTextual(mimeType)) {
        r = fromText(path);
        if (r.kind == Kind::Text)
            return r;
    }
    if (thumbnailer && thumbnailer->canGenerate(mimeType)) {
        const QImage image = thumbnailer->render(path, mimeType, qMax(box.width(), box.height()));
        if (!image.isNull()) {
            r.kind = Kind::Image;
            r.image = image;
            return r;
        }
    }
    r = Result();
    return r;
}

} // namespace Preview
