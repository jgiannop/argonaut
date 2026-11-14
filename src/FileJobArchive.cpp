// Extract and Compress jobs, using libarchive (what GNOME's file-roller and
// Nautilus's gnome-autoar use). Built only when libarchive is available.

#include "FileJob.h"

#include <QDir>
#include <QSet>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>

#include <functional>

#include <archive.h>
#include <archive_entry.h>

namespace {

constexpr qint64 Chunk = 256 * 1024;

bool removeTree(const QString &path)
{
    const QFileInfo info(path);
    if (info.isDir() && !info.isSymLink())
        return QDir(path).removeRecursively();
    return QFile::remove(path);
}

// Entries that would land outside the folder we extract into: absolute
// paths, "..", Windows drive letters. (libarchive's SECURE_* flags also
// guard this; we check too, and count what we skip.)
bool isUnsafePath(const QString &path)
{
    if (path.isEmpty() || path.startsWith(QLatin1Char('/')) || path.startsWith(QLatin1Char('\\')))
        return true;
    if (path.size() > 1 && path.at(1) == QLatin1Char(':'))
        return true;
    const QStringList parts = QString(path).replace(QLatin1Char('\\'), QLatin1Char('/')).split(QLatin1Char('/'));
    return parts.contains(QStringLiteral(".."));
}

QString entryPath(struct archive_entry *entry)
{
    if (const char *utf8 = archive_entry_pathname_utf8(entry))
        return QString::fromUtf8(utf8);
    if (const char *local = archive_entry_pathname(entry))
        return QString::fromLocal8Bit(local);
    return {};
}

// "photos.tar.gz" -> "photos"; "a.zip" -> "a".
QString archiveBaseName(const QString &fileName)
{
    static const char *const doubles[] = {".tar.gz", ".tar.xz", ".tar.bz2", ".tar.zst", ".tar.lz",
                                          ".tar.lzma", ".tar.Z"};
    for (const char *ext : doubles) {
        if (fileName.endsWith(QLatin1String(ext), Qt::CaseInsensitive))
            return fileName.left(fileName.size() - qsizetype(qstrlen(ext)));
    }
    const int dot = int(fileName.lastIndexOf(QLatin1Char('.')));
    return dot > 0 ? fileName.left(dot) : fileName;
}

} // namespace

bool FileJob::archivesSupported()
{
    return true;
}

bool FileJob::listArchive(const QString &archive, int max, QList<ArchiveEntry> *entries, int *total,
                          QString *error)
{
    struct archive *in = archive_read_new();
    archive_read_support_filter_all(in);
    archive_read_support_format_all(in);
    archive_read_support_format_raw(in);
    if (archive_read_open_filename(in, QFile::encodeName(archive).constData(), Chunk) != ARCHIVE_OK) {
        if (error)
            *error = QString::fromLocal8Bit(archive_error_string(in));
        archive_read_free(in);
        return false;
    }
    int count = 0;
    bool ok = true;
    struct archive_entry *entry = nullptr;
    while (true) {
        const int r = archive_read_next_header(in, &entry);
        if (r == ARCHIVE_EOF)
            break;
        if (r < ARCHIVE_WARN) {
            if (error)
                *error = QString::fromLocal8Bit(archive_error_string(in));
            ok = count > 0; // a damaged tail still shows what came before
            break;
        }
        ++count;
        if (entries && entries->size() < max) {
            QString path = archive_format(in) == ARCHIVE_FORMAT_RAW
                               ? archiveBaseName(QFileInfo(archive).fileName())
                               : entryPath(entry);
            const bool dir = archive_entry_filetype(entry) == AE_IFDIR || path.endsWith(QLatin1Char('/'));
            while (path.endsWith(QLatin1Char('/')))
                path.chop(1);
            entries->append({path, archive_entry_size_is_set(entry) ? qint64(archive_entry_size(entry)) : -1, dir});
        }
        archive_read_data_skip(in);
    }
    archive_read_free(in);
    if (total)
        *total = count;
    return ok;
}

QString FileJob::archiveSuffix(ArchiveFormat format)
{
    switch (format) {
    case ArchiveFormat::Zip:
        return QStringLiteral(".zip");
    case ArchiveFormat::TarGz:
        return QStringLiteral(".tar.gz");
    case ArchiveFormat::TarXz:
        return QStringLiteral(".tar.xz");
    case ArchiveFormat::SevenZip:
        return QStringLiteral(".7z");
    }
    return {};
}

// Each archive is unpacked into a hidden temporary folder first, so a
// cancelled or broken extraction leaves nothing behind. Then, like Nautilus:
// a single top-level item is moved out as is; several go into a folder named
// after the archive. Names are never overwritten.
void FileJob::runExtract()
{
    qint64 total = 0;
    for (const Transfer &t : m_transfers)
        total += QFileInfo(t.source).size();
    {
        QMutexLocker lock(&m_mutex);
        m_progress.bytesTotal = total;
        m_progress.measuring = false;
    }
    notify(true);

    QSet<QString> safeDirsUsed;
    for (const Transfer &t : m_transfers) {
        if (isCanceled())
            break;
        const QFileInfo archiveInfo(t.source);
        const QString dest = QDir::cleanPath(t.targetDir);
        setCurrent(archiveInfo.fileName());
        m_safeWrites = needsSafeWrites(dest);
        if (m_safeWrites)
            safeDirsUsed.insert(dest);
        if (!QFileInfo(dest).isDir()) {
            addError(tr("The destination folder “%1” doesn't exist.").arg(QDir::toNativeSeparators(dest)));
            itemDone();
            continue;
        }

        const QString base = archiveBaseName(archiveInfo.fileName());
        const QString temp =
            uniqueTargetPath(dest, QStringLiteral(".") + base + QStringLiteral(".argonaut-extract"), true);
        if (!QDir().mkdir(temp)) {
            addError(tr("Couldn't create a folder in “%1”.").arg(QDir::toNativeSeparators(dest)));
            itemDone();
            continue;
        }

        struct archive *in = archive_read_new();
        archive_read_support_filter_all(in);
        archive_read_support_format_all(in);
        archive_read_support_format_raw(in); // plain .gz / .xz / .bz2 files
        struct archive *out = archive_write_disk_new();
        archive_write_disk_set_options(out, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM
                                                | ARCHIVE_EXTRACT_SECURE_SYMLINKS
                                                | ARCHIVE_EXTRACT_SECURE_NODOTDOT);
        // Not SECURE_NOABSOLUTEPATHS: we rewrite every entry to an absolute
        // path inside `temp`, after isUnsafePath() rejected absolute ones.
        archive_write_disk_set_standard_lookup(out);

        bool failed = false;
        int skipped = 0;
        qint64 lastPosition = 0;
        if (archive_read_open_filename(in, QFile::encodeName(t.source).constData(), Chunk) != ARCHIVE_OK) {
            addError(tr("“%1” isn't an archive Argonaut can open.").arg(archiveInfo.fileName()));
            failed = true;
        }
        struct archive_entry *entry = nullptr;
        while (!failed && !isCanceled()) {
            const int r = archive_read_next_header(in, &entry);
            if (r == ARCHIVE_EOF)
                break;
            if (r < ARCHIVE_WARN) {
                addError(tr("“%1” is damaged: %2")
                             .arg(archiveInfo.fileName(), QString::fromLocal8Bit(archive_error_string(in))));
                failed = true;
                break;
            }
            QString path = entryPath(entry);
            if (archive_format(in) == ARCHIVE_FORMAT_RAW)
                path = base; // a single compressed file has no name inside: use the archive's
            if (isUnsafePath(path)) {
                ++skipped;
                archive_read_data_skip(in);
                continue;
            }
            archive_entry_set_pathname(entry, QFile::encodeName(temp + QLatin1Char('/') + path).constData());
            if (const char *link = archive_entry_hardlink(entry)) {
                const QString target = QString::fromLocal8Bit(link);
                if (isUnsafePath(target)) {
                    ++skipped;
                    archive_read_data_skip(in);
                    continue;
                }
                archive_entry_set_hardlink(entry, QFile::encodeName(temp + QLatin1Char('/') + target).constData());
            }
            if (archive_write_header(out, entry) < ARCHIVE_WARN) {
                ++skipped;
                archive_read_data_skip(in);
                continue;
            }
            const void *buffer = nullptr;
            size_t size = 0;
            la_int64_t offset = 0;
            int dr;
            while ((dr = archive_read_data_block(in, &buffer, &size, &offset)) == ARCHIVE_OK) {
                if (isCanceled())
                    break;
                if (archive_write_data_block(out, buffer, size, offset) < ARCHIVE_WARN) {
                    dr = ARCHIVE_FATAL;
                    break;
                }
                const qint64 position = archive_filter_bytes(in, -1); // compressed bytes read
                addBytes(position - lastPosition);
                lastPosition = position;
            }
            if (dr < ARCHIVE_WARN && !isCanceled()) {
                const bool locked = archive_read_has_encrypted_entries(in) > 0;
                addError(locked ? tr("“%1” is protected with a password, which isn't supported yet.")
                                      .arg(archiveInfo.fileName())
                                : tr("Couldn't extract “%1” from “%2”: %3")
                                      .arg(path, archiveInfo.fileName(),
                                           QString::fromLocal8Bit(archive_error_string(in))));
                failed = true;
            }
            archive_write_finish_entry(out);
        }
        archive_read_free(in);
        archive_write_free(out); // also applies deferred folder dates/permissions

        if (failed || isCanceled()) {
            removeTree(temp);
            itemDone();
            continue;
        }

        // Place the result.
        const QFileInfoList top = QDir(temp).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot
                                                           | QDir::Hidden | QDir::System);
        QString final;
        if (top.isEmpty()) {
            addError(tr("“%1” is empty.").arg(archiveInfo.fileName()));
            removeTree(temp);
        } else if (top.size() == 1) {
            final = uniqueTargetPath(dest, top.first().fileName(), top.first().isDir());
            if (QDir().rename(top.first().absoluteFilePath(), final))
                QDir().rmdir(temp);
            else
                final.clear();
        } else {
            final = uniqueTargetPath(dest, base, true);
            if (!QDir().rename(temp, final))
                final.clear();
        }
        if (!top.isEmpty() && final.isEmpty()) {
            addError(tr("Couldn't put the extracted files in “%1”.").arg(QDir::toNativeSeparators(dest)));
            removeTree(temp);
        } else if (!final.isEmpty()) {
            addResult(t.source, final);
        }
        if (skipped > 0)
            addError(tr("%1: skipped %2 unsafe or unreadable item(s) that tried to write outside the folder.")
                         .arg(archiveInfo.fileName())
                         .arg(skipped));
        addBytes(archiveInfo.size() - lastPosition);
        itemDone();
    }
    // Removable drives: don't report "done" while data is still in RAM.
    for (const QString &dir : std::as_const(safeDirsUsed))
        flushDrive(dir);
}

// All sources into one archive, written to a hidden ".part" file and renamed
// when complete (cancel or failure leaves nothing).
void FileJob::runCompress()
{
    if (m_transfers.isEmpty())
        return;
    qint64 total = 0;
    for (const Transfer &t : m_transfers) {
        if (isCanceled())
            return;
        total += treeSize(t.source);
    }
    {
        QMutexLocker lock(&m_mutex);
        m_progress.bytesTotal = total;
        m_progress.measuring = false;
    }
    notify(true);

    const QString dir = QDir::cleanPath(m_transfers.first().targetDir);
    const QString name = m_transfers.first().targetName;
    m_safeWrites = needsSafeWrites(dir);
    const QString target = uniqueTargetPath(dir, name, false);
    const QString part = dir + QStringLiteral("/.") + QFileInfo(target).fileName() + QStringLiteral(".argonaut-part");
    setCurrent(QFileInfo(target).fileName());

    struct archive *out = archive_write_new();
    switch (m_archiveFormat) {
    case ArchiveFormat::Zip:
        archive_write_set_format_zip(out);
        archive_write_set_options(out, "zip:compression=deflate,zip:hdrcharset=UTF-8");
        break;
    case ArchiveFormat::TarGz:
        archive_write_set_format_pax_restricted(out);
        archive_write_add_filter_gzip(out);
        break;
    case ArchiveFormat::TarXz:
        archive_write_set_format_pax_restricted(out);
        archive_write_add_filter_xz(out);
        break;
    case ArchiveFormat::SevenZip:
        archive_write_set_format_7zip(out);
        break;
    }
    if (archive_write_open_filename(out, QFile::encodeName(part).constData()) != ARCHIVE_OK) {
        addError(tr("Couldn't create “%1”: %2")
                     .arg(QFileInfo(target).fileName(), QString::fromLocal8Bit(archive_error_string(out))));
        archive_write_free(out);
        return;
    }
    struct archive *disk = archive_read_disk_new();
    archive_read_disk_set_standard_lookup(disk);

    bool ok = true;
    QByteArray buffer(int(Chunk), Qt::Uninitialized);
    // Adds `path` as `inside` (its name in the archive), recursing into folders.
    std::function<void(const QString &, const QString &)> add = [&](const QString &path,
                                                                    const QString &inside) {
        if (!ok || isCanceled())
            return;
        struct archive_entry *entry = archive_entry_new();
        archive_entry_copy_sourcepath(entry, QFile::encodeName(path).constData());
        archive_read_disk_entry_from_file(disk, entry, -1, nullptr);
        archive_entry_update_pathname_utf8(entry, inside.toUtf8().constData());
        if (archive_write_header(out, entry) < ARCHIVE_WARN) {
            addError(tr("Couldn't add “%1”: %2")
                         .arg(inside, QString::fromLocal8Bit(archive_error_string(out))));
            ok = false;
        }
        const QFileInfo info(path);
        if (ok && info.isFile() && !info.isSymLink()) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) {
                addError(tr("Couldn't read “%1”.").arg(inside));
                ok = false;
            }
            while (ok && !file.atEnd() && !isCanceled()) {
                const qint64 n = file.read(buffer.data(), buffer.size());
                if (n < 0 || archive_write_data(out, buffer.constData(), size_t(n)) < 0) {
                    addError(tr("Couldn't add “%1”: %2")
                                 .arg(inside, QString::fromLocal8Bit(archive_error_string(out))));
                    ok = false;
                    break;
                }
                addBytes(n);
            }
        }
        archive_entry_free(entry);
        if (ok && info.isDir() && !info.isSymLink()) {
            const QFileInfoList children = QDir(path).entryInfoList(
                QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
            for (const QFileInfo &child : children)
                add(child.absoluteFilePath(), inside + QLatin1Char('/') + child.fileName());
        }
    };
    for (const Transfer &t : m_transfers) {
        add(QDir::cleanPath(t.source), QFileInfo(t.source).fileName());
        itemDone();
    }

    if (archive_write_close(out) != ARCHIVE_OK && ok && !isCanceled()) {
        addError(tr("Couldn't finish “%1”: %2")
                     .arg(QFileInfo(target).fileName(), QString::fromLocal8Bit(archive_error_string(out))));
        ok = false;
    }
    archive_write_free(out);
    archive_read_free(disk);

    if (!ok || isCanceled() || !QFile::rename(part, target)) {
        QFile::remove(part);
        return;
    }
    addResult(QString(), target);
    if (m_safeWrites) // a removable drive: written for real before "done"
        flushDrive(dir);
}
