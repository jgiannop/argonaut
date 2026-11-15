#include "DirectoryModel.h"

#include "Access.h"
#include "ContentSearch.h"

#include "Format.h"

#include "FileJob.h"
#include "FileOperations.h"
#include "Naming.h"
#include "RecentFiles.h"
#include "Starred.h"
#include "StatTime.h"
#include "Trash.h"

#include <QCollator>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QLocale>
#include <QMimeData>
#include <QMimeDatabase>
#include <QSet>
#include <QUrl>
#include <QSocketNotifier>
#include <QStorageInfo>

#include <algorithm>
#include <functional>
#include <optional>

#if defined(Q_OS_LINUX)
#include <sys/inotify.h>
#include <unistd.h>
#endif
#if defined(Q_OS_UNIX)
#include <sys/stat.h>
#endif

namespace {

constexpr int RescanDelayMs = 200;  // coalesce bursts of change notifications
constexpr int FirstBatchMs = 15;    // big folders: show the first files quickly,
constexpr int BatchIntervalMs = 80; // then the rest in batches this often
constexpr size_t BigChange = 500;   // more added/removed than this: reset instead

#if defined(Q_OS_UNIX)
quint16 permissionBits(mode_t mode)
{
    QFile::Permissions p;
    const std::pair<mode_t, QFile::Permission> bits[] = {
        {S_IRUSR, QFile::ReadOwner}, {S_IWUSR, QFile::WriteOwner}, {S_IXUSR, QFile::ExeOwner},
        {S_IRGRP, QFile::ReadGroup}, {S_IWGRP, QFile::WriteGroup}, {S_IXGRP, QFile::ExeGroup},
        {S_IROTH, QFile::ReadOther}, {S_IWOTH, QFile::WriteOther}, {S_IXOTH, QFile::ExeOther}};
    for (const auto &[bit, flag] : bits) {
        if (mode & bit)
            p |= flag;
    }
    return quint16(p.toInt());
}

#endif

// Reads one entry. On Unix it is one lstat (plus a stat for symlinks), not
// QFileInfo: QFileInfo::lastModified() converts to local time, and glibc
// stats /etc/localtime twice per call, which tripled the system calls of a
// listing. Nothing if the item is gone.
std::optional<FileEntry> makeEntry(const QString &dir, const QString &name, const QCollator &collator)
{
    FileEntry e(name, collator.sortKey(name));
    e.setDir(dir);
#if defined(Q_OS_UNIX)
    const QByteArray native = QFile::encodeName(e.path());
    struct stat st;
    if (::lstat(native.constData(), &st) != 0)
        return std::nullopt;
    e.isSymLink = S_ISLNK(st.st_mode);
    if (e.isSymLink) {
        struct stat target;
        if (::stat(native.constData(), &target) == 0)
            st = target; // like QFileInfo: a link shows what it points to
        else
            e.brokenLink = true; // the bits below are the link's own
    }
    e.isDir = S_ISDIR(st.st_mode);
    e.size = e.isDir || S_ISLNK(st.st_mode) ? 0 : qint64(st.st_size); // broken link: 0
    e.mtime = statMtimeMs(st);
    e.permissions = permissionBits(st.st_mode);
    e.ownerId = uint(st.st_uid);
    e.groupId = uint(st.st_gid);
    // Dot-files and editor backups ("file~"); macOS also has a hidden flag.
    e.isHidden = Naming::isHiddenName(name);
#if defined(Q_OS_MACOS)
    e.isHidden = e.isHidden || (st.st_flags & UF_HIDDEN);
#endif
#else
    const QFileInfo fi(e.path());
    if (!fi.exists() && !fi.isSymLink())
        return std::nullopt;
    e.isSymLink = fi.isSymLink();
    e.brokenLink = e.isSymLink && !fi.exists();
    e.isDir = fi.isDir();
    e.size = e.isDir ? 0 : fi.size();
    e.mtime = fi.lastModified().toMSecsSinceEpoch();
    // Only the owner/group/other bits: the "User" ones cost an access() each.
    QFile::Permissions p;
    for (QFile::Permission bit : {QFile::ReadOwner, QFile::WriteOwner, QFile::ExeOwner,
                                  QFile::ReadGroup, QFile::WriteGroup, QFile::ExeGroup,
                                  QFile::ReadOther, QFile::WriteOther, QFile::ExeOther}) {
        if (fi.permission(bit))
            p |= bit;
    }
    e.permissions = quint16(p.toInt());
    // Hidden attribute, dot-files and editor backups ("file~").
    e.isHidden = fi.isHidden() || name.endsWith(QLatin1Char('~'));
#endif
    return e;
}

template<typename T>
int compare3(T a, T b)
{
    return a < b ? -1 : (b < a ? 1 : 0);
}

} // namespace

DirectoryModel::DirectoryModel(QObject *parent)
    : QAbstractTableModel(parent)
{
    m_pool.setMaxThreadCount(2); // one listing + one item-counting job
    // Stars changed: the Starred place lists again; elsewhere only the ★ badges change.
    connect(&Starred::instance(), &Starred::changed, this, [this] {
        if (m_place == Place::Starred)
            scheduleRescan();
        else if (!m_rows.empty())
            emit dataChanged(index(0, 0), index(int(m_rows.size()) - 1, 0), {StarredRole});
    });
    m_contentPool.setMaxThreadCount(1); // reading file heads: one disk stream
    connect(Extensions::Host::instance(), &Extensions::Host::folderChanged, this, [this](const QString &folder) {
        if (folder != m_extFolder || m_extFolder.isEmpty())
            return;
        if (!m_rows.empty())
            emit dataChanged(index(0, 0), index(int(m_rows.size()) - 1, 0), {BadgeRole, Qt::ToolTipRole});
        emit extensionInfoChanged();
    });
    m_contentTimer.setSingleShot(true);
    m_contentTimer.setInterval(30); // batch requests from painting
    connect(&m_contentTimer, &QTimer::timeout, this, &DirectoryModel::flushContentQueue);
    m_rescanTimer.setSingleShot(true);
    m_rescanTimer.setInterval(RescanDelayMs);
    connect(&m_rescanTimer, &QTimer::timeout, this, [this] {
        if (m_scanning)
            m_rescanPending = true; // run again once the current scan lands
        else
            startScan(false);
    });

#if defined(Q_OS_LINUX)
    m_inotifyFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (m_inotifyFd >= 0) {
        m_inotifyNotifier = new QSocketNotifier(m_inotifyFd, QSocketNotifier::Read, this);
        connect(m_inotifyNotifier, &QSocketNotifier::activated, this, [this] {
            // Drain the queue; only whether something happened matters, the
            // rescan works out what.
            alignas(inotify_event) char buffer[16384];
            bool relevant = false;
            ssize_t n;
            while ((n = ::read(m_inotifyFd, buffer, sizeof buffer)) > 0) {
                for (char *p = buffer; p < buffer + n;) {
                    const auto *event = reinterpret_cast<const inotify_event *>(p);
                    if (m_watchIds.contains(event->wd))
                        relevant = true;
                    p += sizeof(inotify_event) + event->len;
                }
            }
            if (relevant)
                folderChanged();
        });
    }
#endif
    if (m_inotifyFd < 0) {
        m_watcher = new QFileSystemWatcher(this);
        connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, [this](const QString &dir) {
            if (m_dirs.contains(QDir::cleanPath(dir)))
                folderChanged();
        });
    }
}

DirectoryModel::~DirectoryModel()
{
    if (!m_extFolder.isEmpty()) // no signal from here: the window may be half gone
        Extensions::Host::instance()->unwatch(m_extFolder);
    ++m_generation; // ask running jobs to stop
    m_pool.clear();
    m_pool.waitForDone();
    m_contentPool.clear();
    m_contentPool.waitForDone();
#if defined(Q_OS_LINUX)
    if (m_inotifyFd >= 0)
        ::close(m_inotifyFd);
#endif
}

void DirectoryModel::watch(const QStringList &dirs)
{
#if defined(Q_OS_LINUX)
    if (m_inotifyFd >= 0) {
        for (int id : std::as_const(m_watchIds))
            inotify_rm_watch(m_inotifyFd, id);
        m_watchIds.clear();
        m_watchByDir.clear();
        for (const QString &dir : dirs)
            addWatch(dir);
        return;
    }
#endif
    if (!m_watcher->directories().isEmpty())
        m_watcher->removePaths(m_watcher->directories());
    m_watcher->addPaths(dirs);
}

void DirectoryModel::addWatch(const QString &dir)
{
#if defined(Q_OS_LINUX)
    if (m_inotifyFd >= 0) {
        if (m_watchByDir.contains(dir))
            return;
        const int id = inotify_add_watch(m_inotifyFd, QFile::encodeName(dir).constData(),
                                         IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE
                                             | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF);
        if (id >= 0) {
            m_watchIds.append(id);
            m_watchByDir.insert(dir, id);
        }
        return;
    }
#endif
    m_watcher->addPath(dir);
}

void DirectoryModel::removeWatch(const QString &dir)
{
#if defined(Q_OS_LINUX)
    if (m_inotifyFd >= 0) {
        const int id = m_watchByDir.take(dir);
        if (id > 0) {
            inotify_rm_watch(m_inotifyFd, id);
            m_watchIds.removeAll(id);
        }
        return;
    }
#endif
    m_watcher->removePath(dir);
}

void DirectoryModel::folderChanged()
{
    refreshExtensions(); // a file changed: what extensions say about it may have too
    if (isVirtual())
        scheduleRescan(); // the list's file changed (or something next to it)
    else if (!QFileInfo(m_path).isDir())
        emit folderGone(m_path);
    else
        scheduleRescan();
}

// ------------------------------------------------------------- loading ---

DirectoryModel::Place DirectoryModel::placeOf(const QString &path)
{
    if (path == recentPath())
        return Place::Recent;
    if (path == starredPath())
        return Place::Starred;
    return Place::Folder;
}

void DirectoryModel::setPath(const QString &path)
{
    m_path = path;
    m_query.clear();
    m_place = placeOf(path);
    if (isVirtual()) {
        // Watch the folder of the list's file: it's rewritten atomically.
        m_trash = false;
        const QString list = m_place == Place::Recent ? RecentFiles::filePath() : Starred::instance().filePath();
        m_dirs = {QFileInfo(list).absolutePath()};
        QDir().mkpath(m_dirs.first());
        watchForExtensions(QString()); // items from anywhere: not a folder to ask about
        relist();
        return;
    }
    m_trash = Trash::isFilesDir(path);
    watchForExtensions(m_trash ? QString() : path);
    m_dirs = m_trash ? Trash::filesDirs() : QStringList{path};
    if (!m_dirs.contains(path))
        m_dirs.prepend(path);
    relist();
}

void DirectoryModel::setSearchFilters(TypeFilter type, DateFilter date)
{
    if (type == m_typeFilter && date == m_dateFilter)
        return;
    m_typeFilter = type;
    m_dateFilter = date;
    if (!m_path.isEmpty() && !isVirtual())
        relist();
}

bool DirectoryModel::passesFilters(TypeFilter type, DateFilter date, const QString &name, bool isDir, qint64 mtime, qint64 now)
{
    if (date != DateFilter::Any) {
        const QDateTime nowDt = QDateTime::fromMSecsSinceEpoch(now);
        const qint64 startOfToday = QDateTime(nowDt.date(), QTime(0, 0)).toMSecsSinceEpoch();
        const qint64 day = 24LL * 3600 * 1000;
        switch (date) {
        case DateFilter::Today:
            if (mtime < startOfToday)
                return false;
            break;
        case DateFilter::Week:
            if (mtime < now - 7 * day)
                return false;
            break;
        case DateFilter::Month:
            if (mtime < now - 30 * day)
                return false;
            break;
        case DateFilter::Year:
            if (mtime < QDateTime(QDate(nowDt.date().year(), 1, 1), QTime(0, 0)).toMSecsSinceEpoch())
                return false;
            break;
        case DateFilter::OlderThanYear:
            if (mtime >= now - 365 * day)
                return false;
            break;
        case DateFilter::Any:
            break;
        }
    }
    if (type == TypeFilter::Any)
        return true;
    if (type == TypeFilter::Folders)
        return isDir;
    if (isDir)
        return false;
    static const QMimeDatabase db;
    const QMimeType mime = db.mimeTypeForFile(name, QMimeDatabase::MatchExtension);
    const QString n = mime.name();
    switch (type) {
    case TypeFilter::Images:
        return n.startsWith(QLatin1String("image/"));
    case TypeFilter::Audio:
        return n.startsWith(QLatin1String("audio/"));
    case TypeFilter::Video:
        return n.startsWith(QLatin1String("video/"));
    case TypeFilter::Archives:
        return FileOperations::isArchive(n);
    case TypeFilter::Documents:
        return n == QLatin1String("application/pdf") || n.startsWith(QLatin1String("application/vnd.oasis.opendocument"))
               || n.startsWith(QLatin1String("application/vnd.openxmlformats-officedocument")) || n == QLatin1String("application/msword")
               || n == QLatin1String("application/vnd.ms-excel") || n == QLatin1String("application/vnd.ms-powerpoint")
               || n == QLatin1String("application/rtf") || n == QLatin1String("application/epub+zip")
               || n == QLatin1String("text/markdown") || n == QLatin1String("text/plain");
    case TypeFilter::Text:
        return mime.inherits(QStringLiteral("text/plain")) || n == QLatin1String("application/json")
               || n == QLatin1String("application/xml") || n == QLatin1String("application/x-shellscript");
    default:
        return true;
    }
}

void DirectoryModel::setSearch(const QString &query)
{
    const QString q = query.simplified();
    if (q == m_query || m_path.isEmpty())
        return;
    m_query = q;
    relist();
}

namespace {
bool isAscii(const QString &s)
{
    for (const QChar c : s) {
        if (c.unicode() >= 0x80)
            return false;
    }
    return true;
}
} // namespace

// Lower case without accents: "Θάλασσα" -> "θαλασσα", "Café" -> "cafe".
QString DirectoryModel::foldForSearch(const QString &text)
{
    if (isAscii(text))
        return text.toLower();
    const QString decomposed = text.normalized(QString::NormalizationForm_D);
    QString out;
    out.reserve(decomposed.size());
    for (const QChar c : decomposed) {
        if (c.category() != QChar::Mark_NonSpacing)
            out.append(c);
    }
    return out.toCaseFolded();
}

bool DirectoryModel::matchesSearch(const QString &name, const QStringList &foldedWords)
{
    // Fast path for the common case: no allocation for ASCII names.
    bool asciiWords = true;
    for (const QString &w : foldedWords)
        asciiWords = asciiWords && isAscii(w);
    if (asciiWords && isAscii(name)) {
        for (const QString &w : foldedWords) {
            if (!name.contains(w, Qt::CaseInsensitive))
                return false;
        }
        return true;
    }
    const QString folded = foldForSearch(name);
    for (const QString &w : foldedWords) {
        if (!folded.contains(w))
            return false;
    }
    return true;
}

void DirectoryModel::relist()
{
    // Every opened folder closes (a new folder, or a search, starts flat).
    for (const QString &folder : std::as_const(m_expanded))
        m_dirs.removeAll(folder);
    m_expanded.clear();
    ++m_generation;
    m_rescanTimer.stop();
    m_rescanPending = false;
    m_searchTruncated = false;
    const QString path = m_path;

    beginResetModel();
    m_rows.clear();
    m_hidden.clear();
    m_rowByPath.clear();
    endResetModel();
    emit headerDataChanged(Qt::Horizontal, 0, ColumnCount - 1);

    // Read every file's head on local disks; on removable and network drives
    // only what's on screen, so a big stick isn't read in full.
    m_wanted.clear();
    m_queued.clear();
    m_contentPool.clear();
    m_thorough = isVirtual() ? false // items from anywhere, USB sticks included: only what's on screen
                             : m_trash || (!needsSafeWrites(path) && !isNetworkFilesystem(path));
    if (isSearch())
        m_thorough = false; // results can be anywhere below: check only what's on screen

    m_loading = true;
    watch(m_dirs);
    startScan(true);
}

void DirectoryModel::refresh()
{
    if (!m_path.isEmpty())
        scheduleRescan();
}

void DirectoryModel::scheduleRescan()
{
    m_rescanTimer.start();
}

void DirectoryModel::startScan(bool initial)
{
    m_scanning = true;
    const quint64 generation = m_generation;
    const QStringList dirs = m_dirs;
    const bool trash = m_trash;
    const QStringList words = isSearch() ? foldForSearch(m_query).split(QLatin1Char(' '), Qt::SkipEmptyParts)
                                         : QStringList();
    const bool search = isSearch(); // words, filters or both
    const TypeFilter typeFilter = m_typeFilter;
    const DateFilter dateFilter = m_dateFilter;
    const bool filtered = typeFilter != TypeFilter::Any || dateFilter != DateFilter::Any;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // Inside files: plain text is read only on this computer's own disks
    // (reading every file of a USB stick or a server is slow).
    const bool contents = search && m_searchContents && !m_trash;
    const bool scanText = contents && !needsSafeWrites(m_path) && !isNetworkFilesystem(m_path);
    const QStringList indexWords = contents ? ContentSearch::queryWords(m_query) : QStringList();
    const bool showHidden = m_showHidden;
    const Place place = m_place;
    QList<RecentFiles::Item> listed; // Starred: read here, on the GUI thread
    if (place == Place::Starred) {
        for (const QString &p : Starred::instance().paths())
            listed.append({p, 0});
    }
    m_pool.start([this, generation, dirs, trash, initial, words, search, showHidden, place, listed, contents,
                  scanText, indexWords, filtered, typeFilter, dateFilter, now] {
        const QCollator collator = Naming::naturalCollator();
        Entries batch;
        QElapsedTimer sinceSend;
        sinceSend.start();
        int interval = FirstBatchMs;
        bool truncated = false;
        auto send = [&](bool done) {
            QMetaObject::invokeMethod(
                this,
                [this, generation, done, initial, truncated, dirs, entries = std::move(batch)]() mutable {
                    if (generation == m_generation && done)
                        m_searchTruncated = truncated;
                    if (initial)
                        applyBatch(generation, std::move(entries), done);
                    else
                        applyRescan(generation, std::move(entries), dirs);
                },
                Qt::QueuedConnection);
            batch = Entries();
            sinceSend.restart();
            interval = BatchIntervalMs;
        };

        // Recent / Starred: the listed items that still exist.
        if (place != Place::Folder) {
            const QList<RecentFiles::Item> items = place == Place::Recent ? RecentFiles::read() : listed;
            for (const RecentFiles::Item &item : items) {
                if (m_generation != generation)
                    return;
                const QFileInfo fi(item.path); // path strings only, no disk access
                if (search && !matchesSearch(fi.fileName(), words))
                    continue;
                std::optional<FileEntry> made = makeEntry(fi.absolutePath(), fi.fileName(), collator);
                if (!made)
                    continue; // gone
                FileEntry &e = *made;
                e.used = item.used;
                e.isHidden = false; // you used or starred it: show it
                batch.push_back(std::move(e));
                if (initial && sinceSend.elapsed() > interval)
                    send(false);
            }
            send(true);
            return;
        }

        // Search: breadth first, so results near the folder come first. It
        // doesn't follow symlinks or enter other drives (or /proc), and
        // skips hidden folders unless hidden files are shown.
        QStringList queue = dirs;
        int found = 0;
        QSet<QString> emitted; // content search: what the walk already found
#if defined(Q_OS_UNIX)
        struct stat rootStat;
        const bool haveRoot = ::stat(QFile::encodeName(dirs.first()).constData(), &rootStat) == 0;
#endif
        for (qsizetype q = 0; q < queue.size(); ++q) {
            const QString dir = queue.at(q);
            QDirIterator it(dir, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
            while (it.hasNext()) {
                if (m_generation != generation)
                    return; // the user went elsewhere
                it.next();
                if (search) {
                    const QFileInfo &fi = it.fileInfo();
                    const QString name = it.fileName();
                    if (Naming::isHiddenName(name) && !showHidden)
                        continue;
                    if (!trash && fi.isDir() && !fi.isSymLink()) {
                        bool sameDrive = true;
#if defined(Q_OS_UNIX)
                        struct stat st;
                        sameDrive = haveRoot
                                    && ::stat(QFile::encodeName(it.filePath()).constData(), &st) == 0
                                    && st.st_dev == rootStat.st_dev;
#endif
                        if (sameDrive)
                            queue.append(it.filePath());
                    }
                    if (!matchesSearch(name, words)) {
                        // Not by name: maybe by its text (plain text files
                        // here; indexed documents after the walk).
                        if (!scanText || fi.isDir())
                            continue;
                        std::optional<FileEntry> made = makeEntry(dir, name, collator);
                        if (!made || made->isDir || !ContentSearch::isTextFile(name, made->size)
                            || (filtered && !passesFilters(typeFilter, dateFilter, name, false, made->mtime, now))
                            || !ContentSearch::textMatches(made->path(), words, &made->snippet))
                            continue;
                        if (++found > MaxSearchResults) {
                            truncated = true;
                            break;
                        }
                        made->contentMatch = true;
                        emitted.insert(made->path());
                        batch.push_back(std::move(*made));
                        if (initial && sinceSend.elapsed() > interval)
                            send(false);
                        continue;
                    }
                    if (++found > MaxSearchResults) {
                        truncated = true;
                        break;
                    }
                }
                std::optional<FileEntry> made = makeEntry(dir, it.fileName(), collator);
                if (!made)
                    continue; // removed since the folder was read
                FileEntry &e = *made;
                if (filtered && search && !passesFilters(typeFilter, dateFilter, e.name, e.isDir, e.mtime, now)) {
                    --found;
                    continue;
                }
                if (contents)
                    emitted.insert(e.path());
                if (trash) {
                    const Trash::Info info = Trash::info(e.path());
                    e.originalPath = info.originalPath;
                    e.deleted = info.deletionDate.isValid() ? info.deletionDate.toMSecsSinceEpoch()
                                                            : e.mtime;
                    e.isHidden = false; // everything in the Trash is shown
                }
                batch.push_back(std::move(e));
                // Initial load of a big folder: show it progressively.
                if (initial && sinceSend.elapsed() > interval)
                    send(false);
            }
            if (truncated)
                break;
        }

        // Documents GNOME's search index found by their text (PDF, office…),
        // with the walk's rules: under the folder, not in hidden folders.
        if (contents && !truncated && !indexWords.isEmpty()) {
            const QString root = dirs.first();
            const QString prefix = root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/');
            const QHash<QString, QString> hits =
                ContentSearch::indexMatches(root, indexWords, [&] { return m_generation != generation; });
            for (auto it = hits.constBegin(); it != hits.constEnd(); ++it) {
                if (m_generation != generation)
                    return;
                const QString &path = it.key();
                if (emitted.contains(path) || !path.startsWith(prefix))
                    continue;
                if (Naming::hasHiddenPart(path.mid(prefix.size())) && !showHidden)
                    continue;
                const qsizetype slash = path.lastIndexOf(QLatin1Char('/'));
                std::optional<FileEntry> made = makeEntry(path.left(slash), path.mid(slash + 1), collator);
                if (!made || made->isDir)
                    continue; // gone since it was indexed
                if (filtered && !passesFilters(typeFilter, dateFilter, made->name, false, made->mtime, now))
                    continue;
                if (++found > MaxSearchResults) {
                    truncated = true;
                    break;
                }
                made->contentMatch = true;
                made->snippet = it.value();
                batch.push_back(std::move(*made));
            }
        }
        send(true);
    });
}

void DirectoryModel::setSearchContents(bool on)
{
    if (m_searchContents == on)
        return;
    m_searchContents = on;
    if (isSearch())
        relist();
}

void DirectoryModel::applyBatch(quint64 generation, Entries batch, bool done)
{
    if (generation != m_generation)
        return;

    Entries shown;
    for (FileEntry &e : batch) {
        applyCachedContent(e);
        if (m_thorough && !e.isDir && e.verdict == ContentType::Verdict::NotChecked)
            wantContent(e); // just this batch's new files, not the whole folder again
        if (isShown(e))
            shown.push_back(std::move(e));
        else
            m_hidden.push_back(std::move(e));
    }

    if (!shown.empty()) {
        if (m_rows.empty()) {
            beginResetModel();
            m_rows = std::move(shown);
            sortRows();
            rebuildIndex();
            endResetModel();
        } else {
            const int first = int(m_rows.size());
            beginInsertRows(QModelIndex(), first, first + int(shown.size()) - 1);
            for (FileEntry &e : shown)
                m_rows.push_back(std::move(e));
            rebuildIndex(); // before endInsertRows: listeners may look names up
            endInsertRows();
            resort(first); // the old rows are sorted: merge the new ones in
        }
    }

    if (done) {
        m_loading = false;
        m_scanning = false;
        emit loaded(m_path);
        m_contentTimer.start(); // now read the real types
        startCounting();
        if (m_rescanPending) {
            m_rescanPending = false;
            scheduleRescan();
        }
    }
}

// Apply a fresh full listing as minimal changes, so selection, current
// item and scroll position survive.
void DirectoryModel::applyRescan(quint64 generation, Entries fresh, const QStringList &scanned)
{
    if (generation != m_generation)
        return;
    m_scanning = false;

    // A tree: only rows of the folders this listing read can be stale. A
    // folder opened meanwhile keeps its rows; one closed meanwhile is dropped.
    const bool tree = isTreeActive();
    const QSet<QString> scannedSet(scanned.cbegin(), scanned.cend());
    const auto covered = [&](const FileEntry &e) { return !tree || scannedSet.contains(e.dir); };
    if (tree) {
        const QSet<QString> listedNow(m_dirs.cbegin(), m_dirs.cend());
        fresh.erase(std::remove_if(fresh.begin(), fresh.end(),
                                   [&](const FileEntry &e) { return !listedNow.contains(e.dir); }),
                    fresh.end());
    }

    for (FileEntry &e : fresh)
        applyCachedContent(e);
    QHash<QString, size_t> freshByPath;
    freshByPath.reserve(qsizetype(fresh.size()));
    for (size_t i = 0; i < fresh.size(); ++i)
        freshByPath.insert(fresh[i].path(), i);

    size_t removed = 0;
    size_t added = 0;
    for (const FileEntry &e : m_rows) {
        if (!covered(e))
            continue;
        const auto it = freshByPath.constFind(e.path());
        if (it == freshByPath.constEnd() || !isShown(fresh[*it]))
            ++removed;
    }
    for (const FileEntry &e : fresh) {
        ensureIndex();
        if (isShown(e) && !m_rowByPath.contains(e.path()))
            ++added;
    }

    if (removed + added > BigChange) {
        // Mass change (e.g. thousands of files deleted): one reset is
        // far cheaper than thousands of row moves.
        beginResetModel();
        Entries kept; // rows of folders this listing didn't read (tree)
        for (FileEntry &e : m_rows) {
            if (!covered(e))
                kept.push_back(std::move(e));
        }
        m_rows = std::move(kept);
        m_hidden.erase(std::remove_if(m_hidden.begin(), m_hidden.end(), covered), m_hidden.end());
        for (FileEntry &e : fresh)
            (isShown(e) ? m_rows : m_hidden).push_back(std::move(e));
        sortRows();
        rebuildIndex();
        endResetModel();
    } else {
        // 1. Removals (descending, so row numbers stay valid).
        for (int row = int(m_rows.size()) - 1; row >= 0; --row) {
            if (!covered(m_rows[size_t(row)]))
                continue;
            const auto it = freshByPath.constFind(m_rows[size_t(row)].path());
            if (it != freshByPath.constEnd() && isShown(fresh[*it]))
                continue;
            beginRemoveRows(QModelIndex(), row, row);
            m_rows.erase(m_rows.begin() + row);
            rebuildIndex(); // before endRemoveRows: listeners may look names up
            endRemoveRows();
        }

        // 2. Updates in place.
        bool sortKeyChanged = false;
        for (size_t row = 0; row < m_rows.size(); ++row) {
            FileEntry &cur = m_rows[row];
            const auto found = freshByPath.constFind(cur.path());
            if (found == freshByPath.constEnd())
                continue; // a folder this listing didn't read (tree)
            const FileEntry &now = fresh[*found];
            if (cur.permissions != now.permissions || cur.ownerId != now.ownerId
                || cur.groupId != now.groupId) {
                // chmod/chown: only the emblem and columns change.
                cur.permissions = now.permissions;
                cur.ownerId = now.ownerId;
                cur.groupId = now.groupId;
                emit dataChanged(index(int(row), 0), index(int(row), ColumnCount - 1));
            }
            if (cur.size == now.size && cur.mtime == now.mtime && cur.isDir == now.isDir
                && cur.isSymLink == now.isSymLink && cur.brokenLink == now.brokenLink)
                continue;
            if (cur.isDir != now.isDir || cur.mtime != now.mtime)
                cur.childCount = -1; // recount
            cur.verdict = ContentType::Verdict::NotChecked; // contents changed: look again
            cur.contentMime.clear();
            cur.size = now.size;
            cur.mtime = now.mtime;
            cur.isDir = now.isDir;
            cur.isSymLink = now.isSymLink;
            cur.brokenLink = now.brokenLink;
            cur.mimeName.clear();
            sortKeyChanged = true;
            emit dataChanged(index(int(row), 0), index(int(row), ColumnCount - 1));
        }

        // 3. Insertions at their sorted position. In a tree with opened
        // folders the order isn't one comparison: append, then sort the tree.
        const bool treeOrder = tree && !m_expanded.isEmpty();
        bool appended = false;
        for (FileEntry &e : fresh) {
            ensureIndex();
            if (!isShown(e) || m_rowByPath.contains(e.path()))
                continue;
            if (treeOrder) {
                const int row = int(m_rows.size());
                beginInsertRows(QModelIndex(), row, row);
                m_rows.push_back(std::move(e));
                rebuildIndex();
                endInsertRows();
                appended = true;
                continue;
            }
            const auto pos = std::lower_bound(m_rows.begin(), m_rows.end(), e,
                                              [this](const FileEntry &a, const FileEntry &b) {
                                                  return lessThan(a, b);
                                              });
            const int row = int(pos - m_rows.begin());
            beginInsertRows(QModelIndex(), row, row);
            m_rows.insert(pos, std::move(e));
            rebuildIndex(); // positions shifted; before endInsertRows for listeners
            endInsertRows();
        }

        m_hidden.erase(std::remove_if(m_hidden.begin(), m_hidden.end(), covered), m_hidden.end());
        for (FileEntry &e : fresh) {
            if (!isShown(e) && !e.name.isEmpty())
                m_hidden.push_back(std::move(e));
        }

        if (appended || (sortKeyChanged && m_sortColumn != NameColumn))
            resort();
    }

    // Opened folders that are gone (deleted, renamed): forget them.
    if (tree && !m_expanded.isEmpty()) {
        ensureIndex();
        const QStringList opened = expandedFolders();
        for (const QString &folder : opened) {
            if (m_expanded.contains(folder) && !m_rowByPath.contains(folder))
                collapse(folder);
        }
    }

    if (m_thorough)
        queueAllUnchecked();
    startCounting();
    if (m_rescanPending) {
        m_rescanPending = false;
        scheduleRescan();
    }
}

// Count entries inside each sub-folder ("12 items"), in the background.
void DirectoryModel::startCounting()
{
    QStringList dirs;
    for (const Entries *list : {&m_rows, &m_hidden}) {
        for (const FileEntry &e : *list) {
            if (e.isDir && e.childCount < 0)
                dirs.append(e.path());
        }
    }
    if (dirs.isEmpty())
        return;

    const quint64 generation = m_generation;
    m_pool.start([this, generation, dirs] {
        QHash<QString, int> counts;
        QElapsedTimer sinceSend;
        sinceSend.start();
        auto send = [&] {
            QMetaObject::invokeMethod(
                this, [this, generation, c = std::move(counts)] { applyCounts(generation, c); },
                Qt::QueuedConnection);
            counts = QHash<QString, int>();
            sinceSend.restart();
        };
        for (const QString &dir : dirs) {
            if (m_generation != generation)
                return;
            int n = 0;
            QDirIterator it(dir, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
            while (it.hasNext()) {
                it.next();
                ++n;
            }
            counts.insert(dir, n);
            if (sinceSend.elapsed() > 150)
                send();
        }
        send();
    });
}

void DirectoryModel::applyCounts(quint64 generation, const QHash<QString, int> &counts)
{
    if (generation != m_generation || counts.isEmpty())
        return;
    for (auto it = counts.constBegin(); it != counts.constEnd(); ++it) {
        ensureIndex();
        const int row = m_rowByPath.value(it.key(), -1);
        if (row >= 0) {
            m_rows[size_t(row)].childCount = it.value();
            const QModelIndex cell = index(row, SizeColumn);
            emit dataChanged(cell, cell);
        } else {
            for (FileEntry &e : m_hidden) {
                if (e.path() == it.key())
                    e.childCount = it.value();
            }
        }
    }
    if (m_sortColumn == SizeColumn)
        resort();
}

// --------------------------------------------------------- hidden/sort ---

void DirectoryModel::setShowHidden(bool show)
{
    if (m_showHidden == show)
        return;
    m_showHidden = show;
    if (isSearch()) {
        relist(); // the walk skips hidden folders: search again
        return;
    }
    if (!show) { // an opened folder that is now hidden (or inside one) closes
        const QStringList opened = expandedFolders();
        for (const QString &folder : opened) {
            const int skip = int(m_path.size()) + (m_path.endsWith(QLatin1Char('/')) ? 0 : 1);
            if (Naming::hasHiddenPart(folder.mid(skip)))
                collapse(folder);
        }
    }
    beginResetModel();
    Entries all = std::move(m_rows);
    for (FileEntry &e : m_hidden)
        all.push_back(std::move(e));
    m_rows.clear();
    m_hidden.clear();
    for (FileEntry &e : all)
        (isShown(e) ? m_rows : m_hidden).push_back(std::move(e));
    sortRows();
    rebuildIndex();
    endResetModel();
}

void DirectoryModel::sort(int column, Qt::SortOrder order)
{
    if (column < 0 || column >= ColumnCount)
        return;
    if (column == m_sortColumn && order == m_sortOrder)
        return;
    m_sortColumn = column;
    m_sortOrder = order;
    resort();
}

// Folders always come first (as in Nautilus), then the sort column, then
// the natural name order as a tie-breaker.
bool DirectoryModel::lessThan(const FileEntry &a, const FileEntry &b) const
{
    if (a.isDir != b.isDir && m_place != Place::Recent) // Recent is in order of use
        return a.isDir;
    int c = 0;
    switch (m_sortColumn) {
    case SizeColumn:
        c = a.isDir ? compare3(a.childCount, b.childCount) : compare3(a.size, b.size);
        break;
    case TypeColumn:
        if (isSearch() || isVirtual())
            c = a.dir.compare(b.dir, Qt::CaseInsensitive);
        else if (m_trash)
            c = a.originalPath.compare(b.originalPath, Qt::CaseInsensitive);
        else if (!a.isDir)
            c = mimeFor(a).compare(mimeFor(b));
        break;
    case ModifiedColumn:
        c = m_trash ? compare3(a.deleted, b.deleted)
            : m_place == Place::Recent ? compare3(a.used, b.used)
                                       : compare3(a.mtime, b.mtime);
        break;
    case OwnerColumn:
        if (a.ownerId != b.ownerId)
            c = Access::userName(a.ownerId).compare(Access::userName(b.ownerId), Qt::CaseInsensitive);
        break;
    case GroupColumn:
        if (a.groupId != b.groupId)
            c = Access::groupName(a.groupId).compare(Access::groupName(b.groupId), Qt::CaseInsensitive);
        break;
    case PermissionsColumn:
        c = compare3(a.permissions, b.permissions);
        break;
    default:
        break;
    }
    if (c == 0)
        c = a.key.compare(b.key);
    if (c == 0)
        c = a.name.compare(b.name);
    if (c == 0)
        c = a.dir.compare(b.dir); // same name in two trash folders
    return m_sortOrder == Qt::AscendingOrder ? c < 0 : c > 0;
}

void DirectoryModel::sortRows()
{
    if (isTreeActive() && !m_expanded.isEmpty()) {
        sortTree();
        return;
    }
    std::stable_sort(m_rows.begin(), m_rows.end(),
                     [this](const FileEntry &a, const FileEntry &b) { return lessThan(a, b); });
}

// Tree order: the open folder's rows sorted, and each opened folder's rows
// sorted on their own, right below it (depth first).
void DirectoryModel::sortTree()
{
    QHash<QString, std::vector<size_t>> byDir;
    for (size_t i = 0; i < m_rows.size(); ++i)
        byDir[m_rows[i].dir].push_back(i);
    for (auto it = byDir.begin(); it != byDir.end(); ++it) {
        std::stable_sort(it->begin(), it->end(),
                         [this](size_t a, size_t b) { return lessThan(m_rows[a], m_rows[b]); });
    }
    Entries out;
    out.reserve(m_rows.size());
    std::vector<bool> used(m_rows.size(), false);
    std::function<void(const QString &)> lay = [&](const QString &dir) {
        const auto it = byDir.constFind(dir);
        if (it == byDir.constEnd())
            return;
        for (size_t i : *it) {
            used[i] = true;
            const QString path = m_rows[i].path();
            const bool open = m_rows[i].isDir && m_expanded.contains(path);
            out.push_back(std::move(m_rows[i]));
            if (open)
                lay(path);
        }
    };
    lay(m_path);
    for (size_t i = 0; i < m_rows.size(); ++i) { // not reachable (shouldn't happen): keep, at the end
        if (!used[i])
            out.push_back(std::move(m_rows[i]));
    }
    m_rows = std::move(out);
}

int DirectoryModel::depthOf(const FileEntry &e) const
{
    if (e.dir == m_path)
        return 0;
    const int skip = int(m_path.size()) + (m_path.endsWith(QLatin1Char('/')) ? 0 : 1);
    return int(QStringView(e.dir).mid(skip).count(QLatin1Char('/'))) + 1;
}

void DirectoryModel::refreshExtensions()
{
    if (!m_extFolder.isEmpty())
        Extensions::Host::instance()->refresh(m_extFolder);
}

void DirectoryModel::watchForExtensions(const QString &folder)
{
    if (folder == m_extFolder)
        return;
    Extensions::Host *host = Extensions::Host::instance();
    const QString old = m_extFolder;
    m_extFolder = folder;
    if (!folder.isEmpty())
        host->watch(folder); // before letting go of the old one: programs keep running
    if (!old.isEmpty())
        host->unwatch(old);
    emit extensionInfoChanged();
}

// ---------------------------------------------------------------- tree ---

void DirectoryModel::setTreeEnabled(bool enabled)
{
    if (m_treeEnabled == enabled)
        return;
    if (!enabled) {
        const QStringList opened = expandedFolders();
        for (const QString &folder : opened)
            collapse(folder);
    }
    m_treeEnabled = enabled;
    if (!m_rows.empty()) // arrows and indentation appear or go
        emit dataChanged(index(0, 0), index(int(m_rows.size()) - 1, 0));
}

void DirectoryModel::expand(const QString &folder)
{
    if (!isTreeActive() || m_expanded.contains(folder))
        return;
    ensureIndex();
    const int row = m_rowByPath.value(folder, -1);
    if (row < 0 || !m_rows[size_t(row)].isDir)
        return;
    m_expanded.insert(folder);
    m_dirs.append(folder);
    addWatch(folder);
    emit dataChanged(index(row, 0), index(row, 0), {ExpandedRole});
    startChildScan(folder);
}

void DirectoryModel::forgetExpanded(const QString &folder)
{
    const QString prefix = folder + QLatin1Char('/');
    const QStringList opened = expandedFolders();
    for (const QString &p : opened) {
        if (p == folder || p.startsWith(prefix)) {
            m_expanded.remove(p);
            m_dirs.removeAll(p);
            removeWatch(p);
        }
    }
}

void DirectoryModel::collapse(const QString &folder)
{
    if (!m_expanded.contains(folder))
        return;
    forgetExpanded(folder);
    const QString prefix = folder + QLatin1Char('/');
    // Its rows (and those of folders opened inside it) are the block right below it.
    ensureIndex();
    const int row = m_rowByPath.value(folder, -1);
    if (row >= 0) {
        const int first = row + 1;
        int last = first;
        while (last < int(m_rows.size()) && m_rows[size_t(last)].path().startsWith(prefix))
            ++last;
        if (last > first) {
            beginRemoveRows(QModelIndex(), first, last - 1);
            m_rows.erase(m_rows.begin() + first, m_rows.begin() + last);
            rebuildIndex(); // before endRemoveRows: listeners may look names up
            endRemoveRows();
        }
        emit dataChanged(index(row, 0), index(row, 0), {ExpandedRole});
    }
    // Anything left elsewhere (it shouldn't be) and its hidden items.
    for (int i = int(m_rows.size()) - 1; i >= 0; --i) {
        if (m_rows[size_t(i)].path().startsWith(prefix)) {
            beginRemoveRows(QModelIndex(), i, i);
            m_rows.erase(m_rows.begin() + i);
            rebuildIndex();
            endRemoveRows();
        }
    }
    m_hidden.erase(std::remove_if(m_hidden.begin(), m_hidden.end(),
                                  [&](const FileEntry &e) { return e.path().startsWith(prefix); }),
                   m_hidden.end());
}

// Lists one opened folder (not the whole tree again).
void DirectoryModel::startChildScan(const QString &dir)
{
    const quint64 generation = m_generation;
    m_pool.start([this, generation, dir] {
        const QCollator collator = Naming::naturalCollator();
        Entries entries;
        QDirIterator it(dir, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        while (it.hasNext()) {
            if (m_generation != generation)
                return; // the user went elsewhere
            it.next();
            if (std::optional<FileEntry> made = makeEntry(dir, it.fileName(), collator))
                entries.push_back(std::move(*made));
        }
        QMetaObject::invokeMethod(
            this,
            [this, generation, dir, entries = std::move(entries)]() mutable {
                applyChildren(generation, dir, std::move(entries));
            },
            Qt::QueuedConnection);
    });
}

void DirectoryModel::applyChildren(quint64 generation, const QString &dir, Entries entries)
{
    if (generation != m_generation || !m_expanded.contains(dir))
        return; // another folder now, or closed again meanwhile
    ensureIndex();
    const int parent = m_rowByPath.value(dir, -1);
    if (parent < 0)
        return;
    const QString prefix = dir + QLatin1Char('/');
    Entries shown;
    for (FileEntry &e : entries) {
        if (m_rowByPath.contains(e.path()))
            continue; // a rescan was quicker
        m_hidden.erase(std::remove_if(m_hidden.begin(), m_hidden.end(),
                                      [&](const FileEntry &h) { return h.path() == e.path(); }),
                       m_hidden.end());
        applyCachedContent(e);
        if (m_thorough && !e.isDir && e.verdict == ContentType::Verdict::NotChecked)
            wantContent(e);
        (isShown(e) ? shown : m_hidden).push_back(std::move(e));
    }
    if (!shown.empty()) {
        std::stable_sort(shown.begin(), shown.end(),
                         [this](const FileEntry &a, const FileEntry &b) { return lessThan(a, b); });
        const bool raced = parent + 1 < int(m_rows.size()) && m_rows[size_t(parent) + 1].path().startsWith(prefix);
        const int first = raced ? int(m_rows.size()) : parent + 1;
        beginInsertRows(QModelIndex(), first, first + int(shown.size()) - 1);
        m_rows.insert(m_rows.begin() + first, std::make_move_iterator(shown.begin()),
                      std::make_move_iterator(shown.end()));
        rebuildIndex(); // before endInsertRows: listeners may look names up
        endInsertRows();
        if (raced)
            resort(); // some rows were already there: put everything in tree order
    }
    m_contentTimer.start();
    startCounting();
}

void DirectoryModel::resort(int sortedPrefix)
{
    if (isTreeActive() && !m_expanded.isEmpty())
        sortedPrefix = -1; // the merge assumes one flat order
    emit layoutAboutToBeChanged({}, QAbstractItemModel::VerticalSortHint);
    const QModelIndexList before = persistentIndexList();
    QStringList paths;
    paths.reserve(before.size());
    for (const QModelIndex &idx : before)
        paths.append(idx.row() < int(m_rows.size()) ? m_rows[size_t(idx.row())].path() : QString());

    const auto less = [this](const FileEntry &a, const FileEntry &b) { return lessThan(a, b); };
    if (sortedPrefix >= 0 && sortedPrefix <= int(m_rows.size())) {
        const auto middle = m_rows.begin() + sortedPrefix;
        std::stable_sort(middle, m_rows.end(), less);
        std::inplace_merge(m_rows.begin(), middle, m_rows.end(), less);
    } else {
        sortRows();
    }
    rebuildIndex();

    if (!before.isEmpty()) { // nothing to remap while a folder is still loading
        QModelIndexList after;
        after.reserve(before.size());
        ensureIndex();
        for (int i = 0; i < before.size(); ++i) {
            const int row = m_rowByPath.value(paths.at(i), -1);
            after.append(row < 0 ? QModelIndex() : index(row, before.at(i).column()));
        }
        changePersistentIndexList(before, after);
    }
    emit layoutChanged({}, QAbstractItemModel::VerticalSortHint);
}

void DirectoryModel::rebuildIndex()
{
    m_indexStale = true;
}

void DirectoryModel::ensureIndex() const
{
    if (!m_indexStale)
        return;
    m_indexStale = false;
    m_rowByPath.clear();
    m_rowByPath.reserve(qsizetype(m_rows.size()));
    for (size_t i = 0; i < m_rows.size(); ++i)
        m_rowByPath.insert(m_rows[i].path(), int(i));
}

// -------------------------------------------------------------- access ---

QModelIndex DirectoryModel::indexForPath(const QString &path, int column) const
{
    ensureIndex();
    const int row = m_rowByPath.value(QDir::cleanPath(path), -1);
    return row < 0 ? QModelIndex() : index(row, column);
}

QString DirectoryModel::filePath(const QModelIndex &index) const
{
    return index.isValid() ? m_rows[size_t(index.row())].path() : QString();
}

QString DirectoryModel::fileName(const QModelIndex &index) const
{
    return index.isValid() ? m_rows[size_t(index.row())].name : QString();
}

bool DirectoryModel::isDir(const QModelIndex &index) const
{
    return index.isValid() && m_rows[size_t(index.row())].isDir;
}

qint64 DirectoryModel::size(const QModelIndex &index) const
{
    return index.isValid() ? m_rows[size_t(index.row())].size : 0;
}

int DirectoryModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

int DirectoryModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

Qt::ItemFlags DirectoryModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemNeverHasChildren
           | Qt::ItemIsDragEnabled;
}

QStringList DirectoryModel::mimeTypes() const
{
    return {QStringLiteral("text/uri-list")};
}

QMimeData *DirectoryModel::mimeData(const QModelIndexList &indexes) const
{
    QList<QUrl> urls;
    QStringList paths;
    QSet<int> rows;
    for (const QModelIndex &index : indexes) {
        if (!index.isValid() || rows.contains(index.row()))
            continue; // the table gives one index per column
        rows.insert(index.row());
        const QString path = filePath(index);
        urls.append(QUrl::fromLocalFile(path));
        paths.append(QDir::toNativeSeparators(path));
    }
    if (urls.isEmpty())
        return nullptr;
    auto *data = new QMimeData;
    data->setUrls(urls);
    data->setText(paths.join(QLatin1Char('\n')));
    return data;
}

Qt::DropActions DirectoryModel::supportedDragActions() const
{
    return Qt::CopyAction | Qt::MoveAction | Qt::LinkAction;
}

QVariant DirectoryModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case NameColumn:
        return tr("Name");
    case SizeColumn:
        return tr("Size");
    case TypeColumn:
        return isSearch() || isVirtual() ? tr("Location") : m_trash ? tr("Original Location") : tr("Type");
    case ModifiedColumn:
        return m_trash ? tr("Deleted") : m_place == Place::Recent ? tr("Last Used") : tr("Modified");
    case OwnerColumn:
        return tr("Owner");
    case GroupColumn:
        return tr("Group");
    case PermissionsColumn:
        return tr("Permissions");
    }
    return {};
}

QVariant DirectoryModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= int(m_rows.size()))
        return {};
    const FileEntry &e = m_rows[size_t(index.row())];
    if (!m_thorough && !e.isDir && e.verdict == ContentType::Verdict::NotChecked
        && (role == Qt::DecorationRole || role == Qt::DisplayRole))
        wantContent(e); // visible on a slow drive: check just this one

    switch (role) {
    case Qt::DisplayRole:
        switch (index.column()) {
        case NameColumn:
            return e.name;
        case SizeColumn:
            if (e.isDir) {
                if (e.childCount < 0)
                    return QString();
                return e.childCount == 1 ? tr("1 item") : tr("%1 items").arg(e.childCount);
            }
            return formatSize(e.size);
        case TypeColumn:
            if ((isSearch() || isVirtual()) && !m_trash) {
                if (e.contentMatch) // where it is, and the text that matched
                    return e.snippet.isEmpty() ? tr("%1 · in its text").arg(searchLocation(e))
                                               : QStringLiteral("%1 · “%2”").arg(searchLocation(e), e.snippet);
                return searchLocation(e);
            }
            if (m_trash) {
                // Folder it came from, with ~ for home (like Nautilus).
                return e.originalPath.isEmpty() ? tr("Unknown")
                                                : tildePath(QFileInfo(e.originalPath).absolutePath());
            }
            return typeFor(e);
        case ModifiedColumn: {
            const QDateTime when = QDateTime::fromMSecsSinceEpoch(
                m_trash ? e.deleted : m_place == Place::Recent && e.used > 0 ? e.used : e.mtime);
            const QLocale locale;
            if (when.date() == QDate::currentDate())
                return locale.toString(when.time(), QLocale::ShortFormat);
            return locale.toString(when.date(), QLocale::ShortFormat);
        }
        case OwnerColumn:
            return Access::userName(e.ownerId);
        case GroupColumn:
            return Access::groupName(e.groupId);
        case PermissionsColumn:
            return Access::hasOwners()
                       ? Access::permissionString(QFile::Permissions::fromInt(e.permissions), e.isDir,
                                                  e.brokenLink)
                       : QString();
        }
        break;
    case Qt::EditRole:
        if (index.column() == NameColumn)
            return e.name;
        break;
    case Qt::DecorationRole:
        if (index.column() == NameColumn)
            return iconFor(e);
        break;
    case PathRole:
        return e.path();
    case OriginalPathRole:
        return e.originalPath;
    case VerdictRole:
        return int(e.verdict);
    case NameMimeRole:
        return nameMimeFor(e);
    case StarredRole:
        return Starred::instance().isStarred(e.path());
    case SymLinkRole:
        return e.isSymLink;
    case LockedRole:
        return isLocked(e);
    case OwnerIdRole:
        return e.ownerId;
    case BadgeRole: {
        if (m_extFolder.isEmpty())
            return {};
        const Extensions::Badge badge = Extensions::Host::instance()->badge(m_extFolder, e.path());
        return badge.valid ? QVariant::fromValue(badge) : QVariant();
    }
    case TreeDepthRole:
        return isTreeActive() ? depthOf(e) : -1;
    case ExpandedRole:
        return m_expanded.contains(e.path());
    case ExpandableRole:
        return isTreeActive() && e.isDir && e.childCount != 0; // "0 items": nothing to open
    case ContentMimeRole:
        return e.contentMime;
    case Qt::ToolTipRole:
        if (index.column() == NameColumn) {
            QString tip = e.isDir ? QString() : ContentType::describe({nameMimeFor(e), e.contentMime, e.verdict});
            if ((isSearch() || isVirtual()) && !m_trash) // the grid has no Location column
                tip = tr("In %1").arg(searchLocation(e)) + (tip.isEmpty() ? QString() : QLatin1Char('\n') + tip);
            if (e.contentMatch)
                tip = (e.snippet.isEmpty() ? tr("Found in its text") : tr("Found in its text: “%1”").arg(e.snippet))
                      + QLatin1Char('\n') + tip;
            QStringList extra;
            if (e.isSymLink) { // read only when hovered: one readlink
                const QString target = QFileInfo(e.path()).symLinkTarget();
                const QString shown = QDir::toNativeSeparators(target);
                extra << (QFileInfo::exists(target) ? tr("Link to %1").arg(shown)
                                                    : tr("Broken link to %1").arg(shown));
            }
            if (!m_extFolder.isEmpty()) {
                const Extensions::Badge badge = Extensions::Host::instance()->badge(m_extFolder, e.path());
                if (badge.valid && !badge.tooltip.isEmpty())
                    extra << badge.tooltip;
            }
            if (isLocked(e))
                extra << (Access::hasOwners() ? tr("Read-only (owner: %1)").arg(Access::userName(e.ownerId))
                                              : tr("Read-only"));
            if (!extra.isEmpty())
                tip = extra.join(QLatin1Char('\n')) + (tip.isEmpty() ? QString() : QLatin1Char('\n') + tip);
            return tip.isEmpty() ? QVariant() : tip;
        }
        break;
    case IsDirRole:
        return e.isDir;
    case SizeRole:
        return e.size;
    case ModifiedRole:
        return QDateTime::fromMSecsSinceEpoch(e.mtime);
    case FileNameRole:
        return e.name;
    }
    return {};
}

// Probably not writable by you: drawn as a 🔒 emblem. In memory only.
bool DirectoryModel::isLocked(const FileEntry &e) const
{
    if (m_trash)
        return false; // nothing in the Trash is edited in place
    return !Access::likelyWritable(QFile::Permissions::fromInt(e.permissions), e.ownerId, e.groupId);
}

ContentType::Result DirectoryModel::contentResult(const QModelIndex &index)
{
    ContentType::Result result;
    result.nameMime = index.data(NameMimeRole).toString();
    result.contentMime = index.data(ContentMimeRole).toString();
    result.verdict = ContentType::Verdict(index.data(VerdictRole).toInt());
    return result;
}

// The effective type: what the content says when it was checked and
// disagrees with (or replaces) the name, else the type from the name.
const QString &DirectoryModel::mimeFor(const FileEntry &e) const
{
    switch (e.verdict) {
    case ContentType::Verdict::Identified:
    case ContentType::Verdict::Mismatch:
    case ContentType::Verdict::Dangerous:
        if (!e.contentMime.isEmpty())
            return e.contentMime;
        break;
    default:
        break;
    }
    return nameMimeFor(e);
}

const QString &DirectoryModel::nameMimeFor(const FileEntry &e) const
{
    if (!e.mimeName.isEmpty())
        return e.mimeName;
    if (e.isDir) {
        e.mimeName = QStringLiteral("inode/directory");
        return e.mimeName;
    }
    const int dot = int(e.name.indexOf(QLatin1Char('.'), 1));
    const QString pattern = (dot > 0 ? e.name.mid(dot) : e.name).toLower();
    auto it = m_mimeByName.constFind(pattern);
    if (it == m_mimeByName.constEnd()) {
        static const QMimeDatabase db;
        it = m_mimeByName.insert(pattern,
                                 db.mimeTypeForFile(e.name, QMimeDatabase::MatchExtension).name());
    }
    e.mimeName = *it;
    return e.mimeName;
}

// Folder icons per folder (Desktop, Documents… have their own); file icons
// per mime type, looked up once.
QIcon DirectoryModel::iconFor(const FileEntry &e) const
{
    const QString key = e.isDir ? QStringLiteral("dir:") + e.path()
                                : QStringLiteral("mime:") + mimeFor(e);
    auto it = m_iconCache.constFind(key);
    if (it == m_iconCache.constEnd()) {
        QIcon icon;
        if (!e.isSymLink) {
            icon = m_iconProvider.icon(QFileInfo(e.path()));
        } else {
            // Qt gives every symlink a generic "link" icon, which the cache
            // would then reuse for real files of that type. Use the target's
            // icon; the link emblem is drawn by FileItemDelegate.
            const QString target = QFileInfo(e.path()).canonicalFilePath(); // empty if broken
            icon = target.isEmpty() ? m_iconProvider.icon(QAbstractFileIconProvider::File)
                                    : m_iconProvider.icon(QFileInfo(target));
        }
        it = m_iconCache.insert(key, icon);
    }
    return *it;
}

// Where a search result is, relative to the searched folder ("Documents/Work"),
// or the folder's own name for items directly in it.
QString DirectoryModel::searchLocation(const FileEntry &e) const
{
    if (isVirtual()) { // the folder it's in, with ~ for home (like Nautilus)
        return tildePath(e.dir);
    }
    if (e.dir == m_path)
        return QFileInfo(m_path).fileName().isEmpty() ? QDir::toNativeSeparators(m_path)
                                                      : QFileInfo(m_path).fileName();
    return QDir::toNativeSeparators(QDir(m_path).relativeFilePath(e.dir));
}

QString DirectoryModel::typeFor(const FileEntry &e) const
{
    if (e.isDir)
        return tr("Folder");
    const QString &mime = mimeFor(e);
    auto it = m_typeNames.constFind(mime);
    if (it == m_typeNames.constEnd()) {
        static const QMimeDatabase db;
        it = m_typeNames.insert(mime, db.mimeTypeForName(mime).comment());
    }
    return *it;
}

// -------------------------------------------------------- content check ---

void DirectoryModel::applyCachedContent(FileEntry &e) const
{
    if (e.isDir) {
        e.verdict = ContentType::Verdict::Consistent;
        return;
    }
    if (e.verdict != ContentType::Verdict::NotChecked)
        return;
    ContentType::Result cached;
    if (ContentType::Cache::instance().lookup(e.path(), e.size, e.mtime, &cached)) {
        e.verdict = cached.verdict;
        e.contentMime = cached.contentMime;
    }
}

void DirectoryModel::wantContent(const FileEntry &e) const
{
    const QString path = e.path();
    if (m_queued.contains(path))
        return;
    m_queued.insert(path);
    m_wanted.append({path, e.size, e.mtime});
    if (!m_loading) // while listing, wait: don't compete with it
        m_contentTimer.start();
}

void DirectoryModel::queueAllUnchecked()
{
    // In view order, so what's on screen first is checked first.
    for (const Entries *list : {&m_rows, &m_hidden}) {
        for (const FileEntry &e : *list) {
            if (!e.isDir && e.verdict == ContentType::Verdict::NotChecked)
                wantContent(e);
        }
    }
}

void DirectoryModel::flushContentQueue()
{
    if (m_wanted.isEmpty())
        return;
    const QList<Wanted> work = std::exchange(m_wanted, {});
    const quint64 generation = m_generation;
    m_contentPool.start([this, work, generation] {
        QList<ContentResult> results;
        QElapsedTimer sinceSend;
        sinceSend.start();
        auto send = [&] {
            QMetaObject::invokeMethod(
                this, [this, generation, r = std::move(results)] { applyContent(generation, r); },
                Qt::QueuedConnection);
            results = QList<ContentResult>();
            sinceSend.restart();
        };
        for (const Wanted &w : work) {
            if (m_generation != generation)
                break; // another folder now: don't keep the disk busy for this one
            results.append({w.path, w.size, w.mtime, ContentType::inspect(w.path)});
            if (sinceSend.elapsed() > 100)
                send();
        }
        if (!results.isEmpty())
            send();
    });
}

void DirectoryModel::applyContent(quint64 generation, const QList<ContentResult> &results)
{
    for (const ContentResult &r : results) {
        // Remember it even if we've moved on: next visit won't read the file.
        ContentType::Cache::instance().store(r.path, r.size, r.mtime, r.result);
        if (generation != m_generation)
            continue;
        m_queued.remove(r.path);
        ensureIndex();
        const int row = m_rowByPath.value(r.path, -1);
        FileEntry *e = row >= 0 ? &m_rows[size_t(row)] : nullptr;
        if (!e) {
            for (FileEntry &hidden : m_hidden) {
                if (hidden.path() == r.path)
                    e = &hidden;
            }
        }
        if (!e || e->size != r.size || e->mtime != r.mtime)
            continue; // changed meanwhile: the rescan will queue it again
        e->verdict = r.result.verdict;
        e->contentMime = r.result.contentMime;
        if (row >= 0)
            emit dataChanged(index(row, 0), index(row, ColumnCount - 1));
    }
}
