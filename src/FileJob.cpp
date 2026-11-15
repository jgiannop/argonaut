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

#include <filesystem>
#include <system_error>

#if defined(Q_OS_UNIX)
#include <fcntl.h>
#include <sys/stat.h>
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

bool isNetworkFilesystem(const QString &path)
{
    static const QStringList network = {"nfs", "nfs4", "cifs", "smb3", "smbfs", "fuse.sshfs",
                                        "fuse.rclone", "9p", "afs", "davfs", "fuse.gvfsd-fuse"};
    return network.contains(QString::fromLatin1(QStorageInfo(path).fileSystemType()));
}

bool needsSafeWrites(const QString &dir)
{
    if (qEnvironmentVariableIsSet("ARGONAUT_SAFE_WRITES"))
        return qEnvironmentVariableIntValue("ARGONAUT_SAFE_WRITES") != 0;

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

#if !defined(ARGONAUT_ARCHIVES)
// Built without libarchive: no archive support.
bool FileJob::archivesSupported() { return false; }
QString FileJob::archiveSuffix(ArchiveFormat) { return {}; }
bool FileJob::listArchive(const QString &, int, QList<ArchiveEntry> *, int *, QString *) { return false; }
void FileJob::runExtract() { addError(tr("Archives aren't supported in this build.")); }
void FileJob::runCompress() { addError(tr("Archives aren't supported in this build.")); }
#endif

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
    else if (m_kind == Kind::Extract)
        runExtract();
    else if (m_kind == Kind::Compress)
        runCompress();
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
        // Restores (and moves back by undo) may need their old folder again.
        if (m_kind != Kind::Copy && !QFileInfo(dir).exists())
            QDir().mkpath(dir);
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

        setCurrent(name);
        auto safe = safeByDir.constFind(dir);
        if (safe == safeByDir.constEnd())
            safe = safeByDir.insert(dir, needsSafeWrites(dir));
        m_safeWrites = *safe;
        if (m_safeWrites)
            safeDirsUsed.insert(dir);

        const bool keepGoing = transferItem(src, dir, name, sizes.at(i), true);
        itemDone();
        if (!keepGoing)
            break; // cancelled from the conflict question
    }

    // 3. Removable drives: don't report "done" while data is still in RAM.
    for (const QString &dir : std::as_const(safeDirsUsed))
        flushDrive(dir);
}

// Transfer `source` to `dir/name`, resolving a name clash first. Returns
// false only if the user cancelled the whole job.
bool FileJob::transferItem(const QString &source, const QString &dir, const QString &name,
                           qint64 bytes, bool topLevel)
{
    const QFileInfo srcInfo(source);
    const bool srcIsDir = srcInfo.isDir() && !srcInfo.isSymLink();
    QString target = QDir::cleanPath(dir + QLatin1Char('/') + name);

    if (pathExists(target)) {
        // Pasting into the folder it came from duplicates, like Nautilus.
        const bool duplicate = m_kind == Kind::Copy && topLevel
                               && QDir::cleanPath(srcInfo.absolutePath()) == dir;
        const Resolution r = duplicate ? Resolution{} : askUser(source, target);
        const QFileInfo existing(target);
        const bool targetIsDir = existing.isDir() && !existing.isSymLink();
        switch (r.action) {
        case ConflictAction::Cancel:
            cancel();
            return false;
        case ConflictAction::Skip:
            addBytes(bytes);
            return true;
        case ConflictAction::KeepBoth: {
            const QString wanted = r.newName.trimmed();
            const bool usable = !wanted.isEmpty() && !wanted.contains(QLatin1Char('/'))
                                && wanted != QLatin1String(".") && wanted != QLatin1String("..");
            target = uniqueTargetPath(dir, usable ? wanted : name, srcIsDir);
            break;
        }
        case ConflictAction::Merge:
            if (srcIsDir && targetIsDir)
                return mergeInto(source, target);
            [[fallthrough]]; // not two folders: merging means replacing
        case ConflictAction::Replace: {
            // Replacing a folder with something inside it would send the
            // source to the Trash together with the folder.
            if (QDir::cleanPath(source).startsWith(target + QLatin1Char('/'))) {
                addError(tr("“%1” wasn't replaced: the item to put there is inside it.").arg(name));
                addBytes(bytes);
                return true;
            }
            // Never destroy the existing item: it goes to the Trash.
            if (!QFile::moveToTrash(target)) {
                addError(tr("Couldn't move the existing “%1” to the Trash, so it wasn't replaced.")
                             .arg(name));
                addBytes(bytes);
                return true;
            }
            break;
        }
        }
    }

    m_createdTarget = false;
    const bool ok = moveOrCopy(source, target, bytes, dir);
    if (ok) {
        if (m_kind == Kind::Restore && topLevel)
            removeTrashInfo(source);
        if (pathExists(target)) // not for a skipped socket or device file
            addResult(source, target);
    } else if (m_createdTarget && pathExists(target)) {
        // An incomplete copy that this job started. If the name was taken by
        // another program in the meantime, what's there isn't ours to remove.
        removeTree(target);
    }
    return true;
}

// Folder onto an existing folder: transfer each child into it (asking again
// for clashes inside). Results record only what this job created, so undo
// never touches what was already there.
bool FileJob::mergeInto(const QString &sourceDir, const QString &targetDir)
{
    const QFileInfo sourceInfo(sourceDir);
    if (!sourceInfo.isReadable() || !sourceInfo.isExecutable()) {
        addError(tr("Couldn't read the folder “%1” (no permission).").arg(sourceInfo.fileName()));
        return true; // nothing was moved or copied from it
    }
    const QFileInfoList children = QDir(sourceDir).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const QFileInfo &child : children) {
        if (isCanceled())
            return false;
        if (!transferItem(child.absoluteFilePath(), targetDir, child.fileName(),
                          treeSize(child.absoluteFilePath()), false))
            return false;
    }
    if (m_kind != Kind::Copy)
        QDir().rmdir(sourceDir); // moved: drop the source folder if it's now empty
    return true;
}

bool FileJob::moveOrCopy(const QString &source, const QString &target, qint64 bytes,
                         const QString &dir)
{
    if (m_kind == Kind::Copy)
        return copyTree(source, target);
    if (QDir().rename(source, target)) {
        addBytes(bytes); // same filesystem: instant
        return true;
    }
    // Different filesystem: copy, and delete the original only once the
    // copy is complete (and, on a removable drive, really written).
    const bool ok = copyTree(source, target);
    if (ok && m_safeWrites)
        syncFilesystemOf(dir);
    if (ok && !removeTree(source))
        addError(tr("“%1” was copied but the original couldn't be removed.")
                     .arg(QFileInfo(source).fileName()));
    return ok;
}

// Worker thread: ask the GUI (conflict signal) and wait for the answer.
FileJob::Resolution FileJob::askUser(const QString &source, const QString &existing)
{
    const QFileInfo s(source);
    const QFileInfo e(existing);
    const bool folders = s.isDir() && !s.isSymLink() && e.isDir() && !e.isSymLink();
    std::optional<ConflictAction> &remembered = folders ? m_folderChoice : m_fileChoice;
    if (remembered)
        return {*remembered, true, QString()};
    if (!m_interactive)
        return {}; // KeepBoth

    {
        QMutexLocker lock(&m_askMutex);
        m_answer.reset();
    }
    emit conflict(source, existing);
    QMutexLocker lock(&m_askMutex);
    while (!m_answer && !isCanceled())
        m_answered.wait(&m_askMutex, 200);
    if (!m_answer)
        return {ConflictAction::Cancel, false, QString()};
    const Resolution answer = *m_answer;
    // A typed name only makes sense once; everything else can be remembered.
    if (answer.applyToAll && answer.newName.isEmpty())
        remembered = answer.action;
    return answer;
}

void FileJob::resolveConflict(const Resolution &resolution)
{
    QMutexLocker lock(&m_askMutex);
    m_answer = resolution;
    m_answered.wakeAll();
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

#if defined(Q_OS_UNIX)
    struct stat st;
    const QByteArray sourceBytes = QFile::encodeName(source);
    const QByteArray targetBytes = QFile::encodeName(target);
    if (::lstat(sourceBytes.constData(), &st) != 0) {
        addError(tr("Couldn't read “%1”.").arg(info.fileName()));
        return false;
    }
    if (S_ISLNK(st.st_mode)) {
        // The link's own text, unchanged: "../lib/tool.sh" stays relative.
        // (QFileInfo::symLinkTarget() gives an absolute path, which would
        // point back into the original folder.)
        QByteArray text(qMax<qint64>(st.st_size, 255) + 1, Qt::Uninitialized);
        qint64 n = ::readlink(sourceBytes.constData(), text.data(), text.size());
        while (n == text.size()) { // longer than lstat said
            text.resize(text.size() * 2);
            n = ::readlink(sourceBytes.constData(), text.data(), text.size());
        }
        if (n < 0 || (text.truncate(n), ::symlink(text.constData(), targetBytes.constData())) != 0) {
            addError(tr("Couldn't copy the link “%1”.").arg(info.fileName()));
            return false;
        }
        m_createdTarget = true;
        return true;
    }
    if (S_ISFIFO(st.st_mode)) {
        // Opening a pipe to read it would wait forever: make a new one.
        if (::mkfifo(targetBytes.constData(), st.st_mode & 07777) != 0) {
            addError(tr("Couldn't copy the pipe “%1”.").arg(info.fileName()));
            return false;
        }
        m_createdTarget = true;
        return true;
    }
    if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) {
        // Sockets and device files can't be copied (and opening one can
        // hang). A copy goes on without it; a move stops, so the original
        // is never deleted without its copy.
        addError(tr("“%1” is a socket or device file and wasn't copied.").arg(info.fileName()));
        return m_kind == Kind::Copy;
    }
#else
    if (info.isSymbolicLink()) {
        // The link as it is, like the Unix branch: QFileInfo::symLinkTarget()
        // is absolute and would point back into the original folder.
        std::error_code failed;
        std::filesystem::copy_symlink(std::filesystem::path(source.toStdWString()),
                                      std::filesystem::path(target.toStdWString()), failed);
        if (failed) {
            addError(tr("Couldn't copy the link “%1”.").arg(info.fileName()));
            return false;
        }
        m_createdTarget = true;
        return true;
    }
#endif

    if (info.isDir()) {
        // A folder we can't list would otherwise copy as an empty one.
        if (!info.isReadable() || !info.isExecutable()) {
            addError(tr("Couldn't read the folder “%1” (no permission).").arg(info.fileName()));
            return false;
        }
        if (!QDir().mkdir(target)) {
            addError(tr("Couldn't create the folder “%1”.").arg(QFileInfo(target).fileName()));
            return false;
        }
        m_createdTarget = true;
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
    m_createdTarget = true;

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
