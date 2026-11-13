#include "FileJob.h"

#include "Trash.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutexLocker>
#include <QSet>
#include <QStorageInfo>
#include <QThreadPool>

#if defined(Q_OS_UNIX)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

constexpr qint64 ChunkSize = 1 << 20; // 1 MiB
// On removable drives, push data to the device this often so progress shows
// the drive's real speed instead of filling RAM and waiting at the end.
constexpr qint64 SafeFlushInterval = 32 << 20; // 32 MiB
constexpr qint64 NotifyIntervalMs = 80;

// Write a file's data to the device itself, not just the OS cache.
void flushToDevice(QFile &file)
{
    file.flush();
#if defined(Q_OS_LINUX)
    ::fdatasync(file.handle());
#elif defined(Q_OS_UNIX)
    ::fsync(file.handle());
#endif
}

// Write everything pending for the filesystem containing `dir` to the device
// (file data and directory entries).
void syncFilesystemOf(const QString &dir)
{
#if defined(Q_OS_LINUX)
    const int fd = ::open(QFile::encodeName(dir).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        ::syncfs(fd);
        ::close(fd);
        return;
    }
    ::sync();
#elif defined(Q_OS_UNIX)
    Q_UNUSED(dir)
    ::sync();
#else
    Q_UNUSED(dir) // Windows: removable drives default to "quick removal" (write-through)
#endif
}

bool pathExists(const QString &path)
{
    const QFileInfo info(path);
    return info.exists() || info.isSymLink(); // broken symlinks count too
}

// Remove a file, symlink or whole folder (never follows symlinks).
bool removeTree(const QString &path)
{
    const QFileInfo info(path);
    if (info.isDir() && !info.isSymLink())
        return QDir(path).removeRecursively();
    return QFile::remove(path);
}

// After an item leaves ".../Trash/files", drop its ".../Trash/info/NAME.trashinfo".
void removeTrashInfo(const QString &pathInTrash)
{
    const QString info = Trash::infoPathFor(pathInTrash);
    if (!info.isEmpty())
        QFile::remove(info);
}

} // namespace

bool needsSafeWrites(const QString &dir)
{
    if (qEnvironmentVariableIsSet("KAFTILOUS_SAFE_WRITES"))
        return qEnvironmentVariableIntValue("KAFTILOUS_SAFE_WRITES") != 0;

    const QStorageInfo storage(dir);
    if (!storage.isValid())
        return false;
#if defined(Q_OS_LINUX)
    const QString root = storage.rootPath();
    // udisks mounts user drives (USB sticks, SD cards, external disks) here.
    if (root.startsWith(QLatin1String("/media/")) || root.startsWith(QLatin1String("/run/media/")))
        return true;
    // Otherwise ask the kernel: USB-attached, or marked removable.
    const QString device = QFileInfo(QString::fromLocal8Bit(storage.device())).canonicalFilePath();
    if (!device.startsWith(QLatin1String("/dev/")))
        return false; // network, tmpfs, fuse…
    const QString sysPath =
        QFileInfo("/sys/class/block/" + QFileInfo(device).fileName()).canonicalFilePath();
    if (sysPath.isEmpty())
        return false;
    if (sysPath.contains(QLatin1String("/usb")))
        return true;
    // "removable" lives on the disk, so check the partition's parent too.
    for (const QString &node : {sysPath, QFileInfo(sysPath).absolutePath()}) {
        QFile flag(node + "/removable");
        if (flag.open(QIODevice::ReadOnly) && flag.readAll().trimmed() == "1")
            return true;
    }
    return false;
#elif defined(Q_OS_MACOS)
    return storage.rootPath().startsWith(QLatin1String("/Volumes/"));
#else
    return false;
#endif
}

QString uniqueTargetPath(const QString &dir, const QString &name, bool isDir)
{
    const QString first = dir + '/' + name;
    if (!pathExists(first))
        return first;

    QString base = name;
    QString ext;
    if (!isDir) {
        const int dot = name.lastIndexOf('.');
        if (dot > 0) {
            base = name.left(dot);
            ext = name.mid(dot);
            if (base.endsWith(QLatin1String(".tar"))) {
                base.chop(4);
                ext.prepend(QLatin1String(".tar"));
            }
        }
    }
    for (int i = 1;; ++i) {
        const QString candidate = i == 1 ? QStringLiteral("%1 (copy)%2").arg(base, ext)
                                         : QStringLiteral("%1 (copy %2)%3").arg(base).arg(i).arg(ext);
        const QString path = dir + '/' + candidate;
        if (!pathExists(path))
            return path;
    }
}

FileJob::FileJob(Kind kind, QList<Transfer> transfers, QObject *parent)
    : QObject(parent)
    , m_kind(kind)
    , m_transfers(std::move(transfers))
{
    m_progress.itemsTotal = int(m_transfers.size());
}

void FileJob::start(QThreadPool *pool)
{
    m_elapsed.start();
    pool->start([this] { run(); });
}

FileJob::Progress FileJob::progress() const
{
    QMutexLocker lock(&m_mutex);
    return m_progress;
}

QList<TransferResult> FileJob::results() const
{
    QMutexLocker lock(&m_mutex);
    return m_results;
}

QStringList FileJob::errors() const
{
    QMutexLocker lock(&m_mutex);
    return m_errors;
}

QStringList FileJob::flushedDrives() const
{
    QMutexLocker lock(&m_mutex);
    return m_flushedDrives;
}

// ------------------------------------------------------- worker thread ---

void FileJob::run()
{
    m_notifyTimer.start();
    if (m_kind == Kind::Trash)
        runTrash();
    else if (m_kind == Kind::Delete)
        runDelete();
    else
        runTransfers();
    notify(true);
    emit finished();
}

void FileJob::runTrash()
{
    {
        QMutexLocker lock(&m_mutex);
        m_progress.measuring = false;
    }
    for (const Transfer &t : m_transfers) {
        if (isCanceled())
            break;
        const QString name = QFileInfo(t.source).fileName();
        setCurrent(name);
        QString pathInTrash;
        if (QFile::moveToTrash(t.source, &pathInTrash))
            addResult(t.source, pathInTrash);
        else
            addError(tr("Couldn't move “%1” to the Trash.").arg(name));
        itemDone();
    }
}

// Permanent deletion (Shift+Delete, Empty Trash). Items inside a trash
// folder also lose their .trashinfo.
void FileJob::runDelete()
{
    {
        QMutexLocker lock(&m_mutex);
        m_progress.measuring = false;
    }
    for (const Transfer &t : m_transfers) {
        if (isCanceled())
            break;
        const QFileInfo info(t.source);
        setCurrent(info.fileName());
        if (!info.exists() && !info.isSymLink()) {
            itemDone();
            continue; // already gone
        }
        if (removeTree(t.source)) {
            removeTrashInfo(t.source);
            addResult(t.source, QString());
        } else {
            addError(tr("Couldn't delete “%1”.").arg(info.fileName()));
        }
        itemDone();
    }
}

void FileJob::runTransfers()
{
    // 1. Measure, so progress can show bytes.
    QList<qint64> sizes;
    qint64 total = 0;
    for (const Transfer &t : m_transfers) {
        if (isCanceled())
            return;
        const qint64 size = treeSize(t.source);
        sizes.append(size);
        total += size;
    }
    {
        QMutexLocker lock(&m_mutex);
        m_progress.bytesTotal = total;
        m_progress.measuring = false;
    }
    notify(true);

    // 2. Transfer.
    QHash<QString, bool> safeByDir; // cache: one storage lookup per folder
    QSet<QString> safeDirsUsed;
    for (int i = 0; i < m_transfers.size(); ++i) {
        if (isCanceled())
            break;
        const Transfer &t = m_transfers.at(i);
        const QFileInfo srcInfo(t.source);
        const QString name = t.targetName.isEmpty() ? srcInfo.fileName() : t.targetName;
        if (!srcInfo.exists() && !srcInfo.isSymLink()) {
            addError(tr("“%1” no longer exists.").arg(name));
            itemDone();
            continue;
        }

        const QString src = QDir::cleanPath(srcInfo.absoluteFilePath());
        const QString dir = QDir::cleanPath(t.targetDir);
        if (m_kind == Kind::Restore && !QFileInfo(dir).exists())
            QDir().mkpath(dir); // its original folder was deleted meanwhile
        if (!QFileInfo(dir).isDir()) {
            addError(tr("The destination folder “%1” doesn't exist.")
                         .arg(QDir::toNativeSeparators(dir)));
            itemDone();
            continue;
        }
        const bool isRealDir = srcInfo.isDir() && !srcInfo.isSymLink();
        if (isRealDir && (dir == src || dir.startsWith(src + '/'))) {
            addError(tr("Can't put the folder “%1” inside itself.").arg(name));
            itemDone();
            continue;
        }
        if (m_kind != Kind::Copy && QDir::cleanPath(dir + '/' + name) == src) {
            addBytes(sizes.at(i)); // moving onto itself: nothing to do
            itemDone();
            continue;
        }

        const QString target = uniqueTargetPath(dir, name, isRealDir);
        setCurrent(name);
        auto safe = safeByDir.constFind(dir);
        if (safe == safeByDir.constEnd())
            safe = safeByDir.insert(dir, needsSafeWrites(dir));
        m_safeWrites = *safe;
        if (m_safeWrites)
            safeDirsUsed.insert(dir);

        bool ok = false;
        if (m_kind == Kind::Copy) {
            ok = copyTree(src, target);
        } else if (QDir().rename(src, target)) {
            // Same filesystem: instant.
            addBytes(sizes.at(i));
            ok = true;
        } else {
            // Different filesystem: copy, and delete the original only once
            // the copy is complete (and, on a removable drive, really written).
            ok = copyTree(src, target);
            if (ok && m_safeWrites)
                syncFilesystemOf(dir);
            if (ok && !removeTree(src))
                addError(tr("“%1” was copied but the original couldn't be removed.").arg(name));
        }

        if (ok) {
            if (m_kind == Kind::Restore)
                removeTrashInfo(src);
            addResult(src, target);
        } else if (pathExists(target)) {
            removeTree(target); // incomplete copy: it didn't exist before
        }
        itemDone();
    }

    // 3. Removable drives: don't report "done" while data is still in RAM.
    for (const QString &dir : std::as_const(safeDirsUsed))
        flushDrive(dir);
}

void FileJob::flushDrive(const QString &dir)
{
    {
        QMutexLocker lock(&m_mutex);
        m_progress.finishing = true;
    }
    notify(true);
    syncFilesystemOf(dir);

    const QStorageInfo storage(dir);
    QString name = storage.displayName();
    if (name.isEmpty())
        name = QDir::toNativeSeparators(storage.rootPath());
    QMutexLocker lock(&m_mutex);
    if (!m_flushedDrives.contains(name))
        m_flushedDrives.append(name);
}

bool FileJob::copyTree(const QString &source, const QString &target)
{
    if (isCanceled())
        return false;
    const QFileInfo info(source);

    if (info.isSymbolicLink()) {
        if (!QFile::link(info.symLinkTarget(), target)) {
            addError(tr("Couldn't copy the link “%1”.").arg(info.fileName()));
            return false;
        }
        return true;
    }

    if (info.isDir()) {
        if (!QDir().mkdir(target)) {
            addError(tr("Couldn't create the folder “%1”.").arg(QFileInfo(target).fileName()));
            return false;
        }
        const QFileInfoList entries = QDir(source).entryInfoList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        for (const QFileInfo &entry : entries) {
            if (!copyTree(entry.absoluteFilePath(), target + '/' + entry.fileName()))
                return false;
        }
        QFile::setPermissions(target, info.permissions());
        return true;
    }

    return copyFile(source, target, info);
}

bool FileJob::copyFile(const QString &source, const QString &target, const QFileInfo &info)
{
    QFile in(source);
    if (!in.open(QIODevice::ReadOnly)) {
        addError(tr("Couldn't read “%1”: %2").arg(info.fileName(), in.errorString()));
        return false;
    }
    QFile out(target);
    if (!out.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        addError(tr("Couldn't create “%1”: %2")
                     .arg(QFileInfo(target).fileName(), out.errorString()));
        return false;
    }

    QByteArray buffer(ChunkSize, Qt::Uninitialized);
    qint64 sinceFlush = 0;
    while (true) {
        if (isCanceled()) {
            out.close();
            out.remove();
            return false;
        }
        const qint64 n = in.read(buffer.data(), buffer.size());
        if (n == 0)
            break;
        if (n < 0 || out.write(buffer.constData(), n) != n) {
            addError(tr("Couldn't copy “%1”: %2")
                         .arg(info.fileName(), n < 0 ? in.errorString() : out.errorString()));
            out.close();
            out.remove();
            return false;
        }
        addBytes(n);
        sinceFlush += n;
        if (m_safeWrites && sinceFlush >= SafeFlushInterval) {
            flushToDevice(out);
            sinceFlush = 0;
        }
    }
    if (m_safeWrites && sinceFlush > 0 && info.size() >= SafeFlushInterval)
        flushToDevice(out); // big files: finish now; small ones go in the final sync

    // Flush first: writing buffered data on close() would reset the date.
    out.flush();
    out.setFileTime(info.lastModified(), QFileDevice::FileModificationTime);
    out.close();
    QFile::setPermissions(target, info.permissions());
    return true;
}

qint64 FileJob::treeSize(const QString &path)
{
    const QFileInfo info(path);
    if (info.isSymLink())
        return 0;
    if (!info.isDir())
        return info.size();
    qint64 total = 0;
    QDirIterator it(path, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        if (isCanceled())
            return total;
        it.next();
        const QFileInfo entry = it.fileInfo();
        if (entry.isFile() && !entry.isSymLink())
            total += entry.size();
    }
    return total;
}

void FileJob::addBytes(qint64 bytes)
{
    {
        QMutexLocker lock(&m_mutex);
        m_progress.bytesDone += bytes;
    }
    notify(false);
}

void FileJob::setCurrent(const QString &name)
{
    {
        QMutexLocker lock(&m_mutex);
        m_progress.current = name;
    }
    notify(false);
}

void FileJob::itemDone()
{
    {
        QMutexLocker lock(&m_mutex);
        ++m_progress.itemsDone;
    }
    notify(false);
}

void FileJob::addResult(const QString &source, const QString &target)
{
    QMutexLocker lock(&m_mutex);
    m_results.append({source, target});
}

void FileJob::addError(const QString &message)
{
    QMutexLocker lock(&m_mutex);
    m_errors.append(message);
}

void FileJob::notify(bool force)
{
    if (!force && m_notifyTimer.elapsed() < NotifyIntervalMs)
        return;
    m_notifyTimer.restart();
    emit progressChanged();
}
