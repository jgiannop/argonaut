#pragma once

#include "ContentType.h"
#include "FileJob.h"

#include <QImage>
#include <QList>
#include <QSize>
#include <QString>

class Thumbnailer;

// Quick Look content: what the preview window shows for one file, worked
// out on a worker thread (load() blocks). Nothing here runs the file:
// images are decoded by Qt, text is read, archives are listed with
// libarchive, and everything else goes through the sandboxed system
// thumbnailers, the same ones used for icons.
namespace Preview {

enum class Kind {
    Image,   // `image` (photos, PDFs, videos, fonts… as a picture)
    Text,    // `text` (the start of a text or code file)
    Archive, // `entries` (what's inside)
    Folder,  // `items` (how many entries)
    Blocked, // a program in disguise: no preview, only a warning
    Generic, // nothing to show but the icon and the details
};

struct Result
{
    Kind kind = Kind::Generic;
    QImage image;
    bool animated = false;      // play it (QMovie) instead of the still `image`
    QString text;
    bool truncated = false;     // text: only the start; archive: only the first entries
    QList<FileJob::ArchiveEntry> entries;
    int total = 0;              // archive: entries in all
    int items = -1;             // folder: entries inside
};

constexpr qint64 MaxTextBytes = 200 * 1024;
constexpr int MaxArchiveEntries = 1000;

// Whether `mimeType` should be shown as text.
bool isTextual(const QString &mimeType);
// `box` is the size (in device pixels) the picture should fit.
Result load(const QString &path, const QString &mimeType, ContentType::Verdict verdict, const QSize &box,
            const Thumbnailer *thumbnailer);

} // namespace Preview
