#include "DirectoryModel.h"

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

#include <algorithm>

#if defined(Q_OS_LINUX)
#include <sys/inotify.h>
#include <unistd.h>
#endif

namespace {

constexpr int RescanDelayMs = 200;  // coalesce bursts of change notifications
constexpr int FirstBatchMs = 15;    // big folders: show the first files quickly,
constexpr int BatchIntervalMs = 80; // then the rest in batches this often
constexpr size_t BigChange = 500;   // more added/removed than this: reset instead

QCollator naturalCollator()
{
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    return collator;
}

FileEntry makeEntry(const QFileInfo &fi, const QCollator &collator)
{
    const QString name = fi.fileName();
    FileEntry e(name, collator.sortKey(name));
    e.isSymLink = fi.isSymLink();
    e.isDir = fi.isDir();
    e.size = e.isDir ? 0 : fi.size();
    e.mtime = fi.lastModified().toMSecsSinceEpoch();
    // Dot-files, Windows hidden attribute, and editor backups ("file~").
    e.isHidden = fi.isHidden() || name.endsWith(QLatin1Char('~'));
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
                    if (event->wd == m_watchId)
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
            if (QDir::cleanPath(dir) == m_path)
                folderChanged();
        });
    }
}

DirectoryModel::~DirectoryModel()
{
    ++m_generation; // ask running jobs to stop
    m_pool.clear();
    m_pool.waitForDone();
#if defined(Q_OS_LINUX)
    if (m_inotifyFd >= 0)
        ::close(m_inotifyFd);
#endif
}

void DirectoryModel::watch(const QString &path)
{
#if defined(Q_OS_LINUX)
    if (m_inotifyFd >= 0) {
        if (m_watchId >= 0)
            inotify_rm_watch(m_inotifyFd, m_watchId);
        m_watchId = inotify_add_watch(m_inotifyFd, QFile::encodeName(path).constData(),
                                      IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO
                                          | IN_CLOSE_WRITE | IN_ATTRIB | IN_DELETE_SELF
                                          | IN_MOVE_SELF);
        return;
    }
#endif
    if (!m_watcher->directories().isEmpty())
        m_watcher->removePaths(m_watcher->directories());
    m_watcher->addPath(path);
}

void DirectoryModel::folderChanged()
{
    if (!QFileInfo(m_path).isDir())
        emit folderGone(m_path);
    else
        scheduleRescan();
}

// ------------------------------------------------------------- loading ---

void DirectoryModel::setPath(const QString &path)
{
    ++m_generation;
    m_rescanTimer.stop();
    m_rescanPending = false;

    beginResetModel();
    m_path = path;
    m_rows.clear();
    m_hidden.clear();
    m_rowByName.clear();
    endResetModel();

    m_loading = true;
    watch(path);
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
    const QString path = m_path;
    m_pool.start([this, generation, path, initial] {
        const QCollator collator = naturalCollator();
        Entries batch;
        QElapsedTimer sinceSend;
        sinceSend.start();
        int interval = FirstBatchMs;
        auto send = [&](bool done) {
            QMetaObject::invokeMethod(
                this,
                [this, generation, done, initial, entries = std::move(batch)]() mutable {
                    if (initial)
                        applyBatch(generation, std::move(entries), done);
                    else
                        applyRescan(generation, std::move(entries));
                },
                Qt::QueuedConnection);
            batch = Entries();
            sinceSend.restart();
            interval = BatchIntervalMs;
        };

        QDirIterator it(path, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        while (it.hasNext()) {
            if (m_generation != generation)
                return; // the user went elsewhere
            it.next();
            batch.push_back(makeEntry(it.fileInfo(), collator));
            // Initial load of a big folder: show it progressively.
            if (initial && sinceSend.elapsed() > interval)
                send(false);
        }
        send(true);
    });
}

void DirectoryModel::applyBatch(quint64 generation, Entries batch, bool done)
{
    if (generation != m_generation)
        return;

    Entries shown;
    for (FileEntry &e : batch) {
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
            resort();
        }
    }

    if (done) {
        m_loading = false;
        m_scanning = false;
        emit loaded(m_path);
        startCounting();
        if (m_rescanPending) {
            m_rescanPending = false;
            scheduleRescan();
        }
    }
}

// Apply a fresh full listing as minimal changes, so selection, current
// item and scroll position survive.
void DirectoryModel::applyRescan(quint64 generation, Entries fresh)
{
    if (generation != m_generation)
        return;
    m_scanning = false;

    QHash<QString, size_t> freshByName;
    freshByName.reserve(qsizetype(fresh.size()));
    for (size_t i = 0; i < fresh.size(); ++i)
        freshByName.insert(fresh[i].name, i);

    size_t removed = 0;
    size_t added = 0;
    for (const FileEntry &e : m_rows) {
        const auto it = freshByName.constFind(e.name);
        if (it == freshByName.constEnd() || !isShown(fresh[*it]))
            ++removed;
    }
    for (const FileEntry &e : fresh) {
        if (isShown(e) && !m_rowByName.contains(e.name))
            ++added;
    }

    if (removed + added > BigChange) {
        // Mass change (e.g. thousands of files deleted): one reset is
        // far cheaper than thousands of row moves.
        beginResetModel();
        m_rows.clear();
        m_hidden.clear();
        for (FileEntry &e : fresh)
            (isShown(e) ? m_rows : m_hidden).push_back(std::move(e));
        sortRows();
        rebuildIndex();
        endResetModel();
    } else {
        // 1. Removals (descending, so row numbers stay valid).
        for (int row = int(m_rows.size()) - 1; row >= 0; --row) {
            const auto it = freshByName.constFind(m_rows[size_t(row)].name);
            if (it != freshByName.constEnd() && isShown(fresh[*it]))
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
            const FileEntry &now = fresh[freshByName.value(cur.name)];
            if (cur.size == now.size && cur.mtime == now.mtime && cur.isDir == now.isDir
                && cur.isSymLink == now.isSymLink)
                continue;
            if (cur.isDir != now.isDir || cur.mtime != now.mtime)
                cur.childCount = -1; // recount
            cur.size = now.size;
            cur.mtime = now.mtime;
            cur.isDir = now.isDir;
            cur.isSymLink = now.isSymLink;
            cur.mimeName.clear();
            sortKeyChanged = true;
            emit dataChanged(index(int(row), 0), index(int(row), ColumnCount - 1));
        }

        // 3. Insertions at their sorted position.
        for (FileEntry &e : fresh) {
            if (!isShown(e) || m_rowByName.contains(e.name))
                continue;
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

        m_hidden.clear();
        for (FileEntry &e : fresh) {
            if (!isShown(e) && !e.name.isEmpty())
                m_hidden.push_back(std::move(e));
        }

        if (sortKeyChanged && m_sortColumn != NameColumn)
            resort();
    }

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
                dirs.append(e.name);
        }
    }
    if (dirs.isEmpty())
        return;

    const quint64 generation = m_generation;
    const QString path = m_path;
    m_pool.start([this, generation, path, dirs] {
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
        for (const QString &name : dirs) {
            if (m_generation != generation)
                return;
            int n = 0;
            QDirIterator it(path + QLatin1Char('/') + name,
                            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
            while (it.hasNext()) {
                it.next();
                ++n;
            }
            counts.insert(name, n);
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
        const int row = m_rowByName.value(it.key(), -1);
        if (row >= 0) {
            m_rows[size_t(row)].childCount = it.value();
            const QModelIndex cell = index(row, SizeColumn);
            emit dataChanged(cell, cell);
        } else {
            for (FileEntry &e : m_hidden) {
                if (e.name == it.key())
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
    if (a.isDir != b.isDir)
        return a.isDir;
    int c = 0;
    switch (m_sortColumn) {
    case SizeColumn:
        c = a.isDir ? compare3(a.childCount, b.childCount) : compare3(a.size, b.size);
        break;
    case TypeColumn:
        if (!a.isDir)
            c = mimeFor(a).compare(mimeFor(b));
        break;
    case ModifiedColumn:
        c = compare3(a.mtime, b.mtime);
        break;
    default:
        break;
    }
    if (c == 0)
        c = a.key.compare(b.key);
    if (c == 0)
        c = a.name.compare(b.name);
    return m_sortOrder == Qt::AscendingOrder ? c < 0 : c > 0;
}

void DirectoryModel::sortRows()
{
    std::stable_sort(m_rows.begin(), m_rows.end(),
                     [this](const FileEntry &a, const FileEntry &b) { return lessThan(a, b); });
}

void DirectoryModel::resort()
{
    emit layoutAboutToBeChanged({}, QAbstractItemModel::VerticalSortHint);
    const QModelIndexList before = persistentIndexList();
    QStringList names;
    names.reserve(before.size());
    for (const QModelIndex &idx : before)
        names.append(idx.row() < int(m_rows.size()) ? m_rows[size_t(idx.row())].name : QString());

    sortRows();
    rebuildIndex();

    QModelIndexList after;
    after.reserve(before.size());
    for (int i = 0; i < before.size(); ++i) {
        const int row = m_rowByName.value(names.at(i), -1);
        after.append(row < 0 ? QModelIndex() : index(row, before.at(i).column()));
    }
    changePersistentIndexList(before, after);
    emit layoutChanged({}, QAbstractItemModel::VerticalSortHint);
}

void DirectoryModel::rebuildIndex()
{
    m_rowByName.clear();
    m_rowByName.reserve(qsizetype(m_rows.size()));
    for (size_t i = 0; i < m_rows.size(); ++i)
        m_rowByName.insert(m_rows[i].name, int(i));
}

// -------------------------------------------------------------- access ---

QString DirectoryModel::joinPath(const QString &name) const
{
    return m_path.endsWith(QLatin1Char('/')) ? m_path + name : m_path + QLatin1Char('/') + name;
}

QModelIndex DirectoryModel::indexForPath(const QString &path, int column) const
{
    const QFileInfo info(path);
    if (QDir::cleanPath(info.absolutePath()) != m_path)
        return {};
    const int row = m_rowByName.value(info.fileName(), -1);
    return row < 0 ? QModelIndex() : index(row, column);
}

QString DirectoryModel::filePath(const QModelIndex &index) const
{
    return index.isValid() ? joinPath(m_rows[size_t(index.row())].name) : QString();
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
        return tr("Type");
    case ModifiedColumn:
        return tr("Modified");
    }
    return {};
}

QVariant DirectoryModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= int(m_rows.size()))
        return {};
    const FileEntry &e = m_rows[size_t(index.row())];

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
            return QLocale().formattedDataSize(e.size);
        case TypeColumn:
            return typeFor(e);
        case ModifiedColumn: {
            const QDateTime when = QDateTime::fromMSecsSinceEpoch(e.mtime);
            const QLocale locale;
            if (when.date() == QDate::currentDate())
                return locale.toString(when.time(), QLocale::ShortFormat);
            return locale.toString(when.date(), QLocale::ShortFormat);
        }
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
        return joinPath(e.name);
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

// Mime type from the name only (no file access). Cached per name pattern:
// the part after the first dot, or the whole name ("Makefile").
const QString &DirectoryModel::mimeFor(const FileEntry &e) const
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
    const QString key = e.isDir ? QStringLiteral("dir:") + joinPath(e.name)
                                : QStringLiteral("mime:") + mimeFor(e);
    auto it = m_iconCache.constFind(key);
    if (it == m_iconCache.constEnd())
        it = m_iconCache.insert(key, m_iconProvider.icon(QFileInfo(joinPath(e.name))));
    return *it;
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
