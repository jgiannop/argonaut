#include "DiskUsageModel.h"

#include "Naming.h"
#include "StatTime.h"
#include "Format.h"

#include <QCollator>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFont>
#include <QLocale>
#include <QMimeDatabase>
#include <QSet>

#include <algorithm>

#if defined(Q_OS_UNIX)
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using Node = DiskUsageModel::Node;

namespace {

void attach(Node *parent, std::unique_ptr<Node> child);
void noListing(Node *) {}

struct FileSeen
{
    QString name;
    qint64 bytes;
    qint64 mtime;
};

// Keeps the biggest files as Nodes; the rest only add to `smaller`.
void keepBiggest(Node *node, std::vector<FileSeen> &big)
{
    const auto bigger = [](const FileSeen &a, const FileSeen &b) { return a.bytes > b.bytes; };
    if (big.size() > size_t(DiskUsageModel::MaxFilesPerFolder)) {
        std::nth_element(big.begin(), big.begin() + DiskUsageModel::MaxFilesPerFolder, big.end(), bigger);
        for (size_t i = DiskUsageModel::MaxFilesPerFolder; i < big.size(); ++i) {
            node->smallerBytes += big[i].bytes;
            ++node->smallerCount;
        }
        big.resize(DiskUsageModel::MaxFilesPerFolder);
    }
    for (const FileSeen &f : big) {
        auto file = std::make_unique<Node>();
        file->kind = Node::File;
        file->name = f.name;
        file->bytes = f.bytes;
        file->mtime = f.mtime;
        file->parent = node;
        node->files.push_back(std::move(file));
    }
}

// One scan on the worker thread. Measures a folder tree into Nodes.
struct Scanner
{
    const QAtomicInteger<quint64> &generation;
    quint64 mine;
    QAtomicInteger<qint64> &bytesSeen;
    QAtomicInteger<qint64> &itemsSeen;
#if defined(Q_OS_UNIX)
    dev_t device = 0;
    QSet<QPair<quint64, quint64>> hardLinks{}; // (dev, inode) of files seen with nlink > 1
#endif

    bool canceled() const { return generation.loadRelaxed() != mine; }

    // Measures what's inside `path` into `node` (the caller counts the
    // folder's own entry). `onListed(node)` is called once the folder's own
    // files are counted, before going into sub-folders; `onChild` for each
    // sub-folder once measured.
    template<typename OnChild, typename OnListed>
    void scan(const QString &path, Node *node, OnChild onChild, OnListed onListed)
    {
        struct Sub
        {
            QString name;
            qint64 mtime;
            qint64 used; // the folder's own entry: counted in its own size
        };
        std::vector<Sub> subs;
        std::vector<FileSeen> big;
#if defined(Q_OS_UNIX)
        const QByteArray native = QFile::encodeName(path);
        const int fd = ::open(native.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        DIR *dir = fd >= 0 ? ::fdopendir(fd) : nullptr;
        if (!dir) {
            if (fd >= 0)
                ::close(fd);
            node->incomplete = true;
            onListed(node);
            return;
        }
        // Read the folder first, then go into sub-folders, so only one
        // folder is open at a time however deep the tree is.
        while (dirent *entry = ::readdir(dir)) {
            if (canceled())
                break;
            const char *name = entry->d_name;
            if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0)))
                continue;
            struct stat st;
            if (::fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
                node->incomplete = true;
                continue;
            }
            ++node->items;
            itemsSeen.fetchAndAddRelaxed(1);
            const bool isDir = S_ISDIR(st.st_mode);
            if (!isDir && st.st_nlink > 1) {
                const QPair<quint64, quint64> key(quint64(st.st_dev), quint64(st.st_ino));
                if (hardLinks.contains(key))
                    continue; // already counted under another name
                hardLinks.insert(key);
            }
            const qint64 used = qint64(st.st_blocks) * 512;
            const qint64 mtime = statMtimeMs(st);
            bytesSeen.fetchAndAddRelaxed(used);
            if (isDir && st.st_dev == device) {
                subs.push_back({QFile::decodeName(name), mtime, used});
                continue;
            }
            node->bytes += used;
            if (isDir) {
                node->smallerBytes += used; // a drive mounted here: not entered
            } else if (S_ISREG(st.st_mode) && used >= DiskUsageModel::MinFileBytes) {
                big.push_back({QFile::decodeName(name), used, mtime});
            } else {
                node->smallerBytes += used;
                ++node->smallerCount;
            }
        }
        ::closedir(dir);
#else
        QDirIterator it(path, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        while (it.hasNext()) {
            if (canceled())
                break;
            it.next();
            const QFileInfo fi = it.fileInfo();
            ++node->items;
            itemsSeen.fetchAndAddRelaxed(1);
            if (fi.isDir() && !fi.isSymLink()) {
                subs.push_back({fi.fileName(), fi.lastModified().toMSecsSinceEpoch(), 0});
                continue;
            }
            const qint64 used = fi.isSymLink() ? 0 : fi.size();
            node->bytes += used;
            bytesSeen.fetchAndAddRelaxed(used);
            if (used >= DiskUsageModel::MinFileBytes) {
                big.push_back({fi.fileName(), used, fi.lastModified().toMSecsSinceEpoch()});
            } else {
                node->smallerBytes += used;
                ++node->smallerCount;
            }
        }
#endif
        keepBiggest(node, big);
        onListed(node);
        for (const Sub &sub : subs) {
            if (canceled())
                return;
            auto child = std::make_unique<Node>();
            child->name = sub.name;
            child->mtime = sub.mtime;
            child->bytes = sub.used;
            scan(path + QLatin1Char('/') + sub.name, child.get(), attach, noListing);
            onChild(node, std::move(child));
        }
    }
};

// Default for nested folders: attach the measured child to its parent.
void attach(Node *parent, std::unique_ptr<Node> child)
{
    parent->bytes += child->bytes;
    parent->items += child->items;
    parent->incomplete = parent->incomplete || child->incomplete;
    child->parent = parent;
    parent->children.push_back(std::move(child));
}

void fixParents(Node *node)
{
    for (auto &child : node->children) {
        child->parent = node;
        fixParents(child.get());
    }
    for (auto &file : node->files)
        file->parent = node;
}

QCollator &collator()
{
    static QCollator c = Naming::naturalCollator();
    return c;
}

} // namespace

DiskUsageModel::DiskUsageModel(QObject *parent)
    : QAbstractItemModel(parent)
{
    m_pool.setMaxThreadCount(1);
    m_progressTimer.setInterval(100);
    connect(&m_progressTimer, &QTimer::timeout, this, &DiskUsageModel::progress);
    const QFileIconProvider icons;
    m_folderIcon = icons.icon(QFileIconProvider::Folder);
    m_fileIcon = icons.icon(QFileIconProvider::File);
}

DiskUsageModel::~DiskUsageModel()
{
    ++m_generation;
    m_pool.clear();
    m_pool.waitForDone();
}

QString DiskUsageModel::rootPath() const
{
    return m_root ? pathOf(m_root) : QString();
}

void DiskUsageModel::setRoot(const QString &path)
{
    const QString clean = QDir::cleanPath(path);
    if (m_root && clean == rootPath())
        return;
    if (m_complete && m_tree) {
        Node *node = findNode(clean);
        if (node && node->kind == Node::Folder) {
            beginResetModel();
            m_root = node;
            endResetModel();
            emit finished();
            return;
        }
    }
    startScan(clean);
}

void DiskUsageModel::rescan()
{
    const QString path = rootPath();
    if (!path.isEmpty())
        startScan(path);
}

void DiskUsageModel::setShowFiles(bool show)
{
    if (show == m_showFiles)
        return;
    beginResetModel();
    m_showFiles = show;
    if (m_tree)
        invalidateRows(m_tree.get());
    endResetModel();
}

void DiskUsageModel::startScan(const QString &path)
{
    const quint64 generation = ++m_generation;
    beginResetModel();
    m_tree = std::make_unique<Node>();
    m_tree->name = QFileInfo(path).fileName().isEmpty() ? QDir::toNativeSeparators(path)
                                                        : QFileInfo(path).fileName();
    m_tree->mtime = QFileInfo(path).lastModified().toMSecsSinceEpoch();
    m_treePath = path;
    m_root = m_tree.get();
    m_scanning = true;
    m_complete = false;
    endResetModel();
    m_scannedBytes.storeRelaxed(0);
    m_scannedItems.storeRelaxed(0);
    m_scanTimer.start();
    m_progressTimer.start();

    m_pool.start([this, path, generation] {
        Scanner scanner{m_generation, generation, m_scannedBytes, m_scannedItems};
#if defined(Q_OS_UNIX)
        struct stat st;
        if (::stat(QFile::encodeName(path).constData(), &st) == 0)
            scanner.device = st.st_dev;
#endif
        // The folder's own files are sent first; then each sub-folder once measured.
        Node own;
        scanner.scan(
            path, &own,
            [&](Node *, std::unique_ptr<Node> child) {
                if (scanner.canceled())
                    return;
                fixParents(child.get());
                Node *raw = child.release();
                QMetaObject::invokeMethod(this, [this, generation, raw] { addTopChild(generation, raw); },
                                          Qt::QueuedConnection);
            },
            [&](Node *listed) {
                auto copy = new Node;
                copy->bytes = listed->bytes;
                copy->items = listed->items;
                copy->smallerBytes = listed->smallerBytes;
                copy->smallerCount = listed->smallerCount;
                copy->incomplete = listed->incomplete;
                copy->files = std::move(listed->files);
                QMetaObject::invokeMethod(this, [this, generation, copy] { addTopFiles(generation, copy); },
                                          Qt::QueuedConnection);
            });
        if (scanner.canceled())
            return;
        qint64 ownBytes = 0;
#if defined(Q_OS_UNIX)
        ownBytes = qint64(st.st_blocks) * 512; // the folder's own entry, like du
#endif
        const bool incomplete = own.incomplete;
        QMetaObject::invokeMethod(
            this, [this, generation, ownBytes, incomplete] { finishScan(generation, ownBytes, incomplete); },
            Qt::QueuedConnection);
    });
}

// The analysed folder's own files (and counts), right after it was listed.
void DiskUsageModel::addTopFiles(quint64 generation, Node *raw)
{
    std::unique_ptr<Node> listed(raw);
    if (generation != m_generation || !m_tree)
        return;
    beginResetModel(); // only the top row exists this early
    Node *top = m_tree.get();
    top->bytes += listed->bytes;
    top->items += listed->items;
    top->smallerBytes += listed->smallerBytes;
    top->smallerCount += listed->smallerCount;
    top->incomplete = top->incomplete || listed->incomplete;
    for (auto &file : listed->files) {
        file->parent = top;
        top->files.push_back(std::move(file));
    }
    invalidateRows(top);
    endResetModel();
}

// A top-level sub-folder, measured with everything inside it.
void DiskUsageModel::addTopChild(quint64 generation, Node *raw)
{
    std::unique_ptr<Node> child(raw);
    if (generation != m_generation || !m_tree)
        return;
    Node *top = m_tree.get();
    child->parent = top;
    Node *node = child.get();
    top->bytes += child->bytes;
    top->items += child->items;
    top->incomplete = top->incomplete || child->incomplete;
    top->children.push_back(std::move(child));
    if (!top->rowsBuilt || !isShown(top))
        return;
    auto pos = std::lower_bound(top->rows.begin(), top->rows.end(), node,
                                [this](const Node *a, const Node *b) { return lessThan(a, b); });
    const int row = int(pos - top->rows.begin());
    beginInsertRows(indexFor(top), row, row);
    top->rows.insert(pos, node);
    for (size_t i = size_t(row); i < top->rows.size(); ++i)
        top->rows[i]->row = int(i);
    endInsertRows();
    // The top row grew, so every share of it changed.
    emit dataChanged(indexFor(top), indexFor(top, ColumnCount - 1));
    emit dataChanged(index(0, 0, indexFor(top)),
                     index(int(top->rows.size()) - 1, ColumnCount - 1, indexFor(top)));
}

void DiskUsageModel::finishScan(quint64 generation, qint64 ownBytes, bool incomplete)
{
    if (generation != m_generation || !m_tree)
        return;
    Node *top = m_tree.get();
    top->bytes += ownBytes;
    top->incomplete = top->incomplete || incomplete;
    m_scanning = false;
    m_complete = true;
    m_progressTimer.stop();
    m_scanMs = m_scanTimer.elapsed();
    if (isShown(top))
        emit dataChanged(indexFor(top), indexFor(top, ColumnCount - 1));
    emit finished();
}

// ----------------------------------------------------- applying changes ---

qint64 DiskUsageModel::sizeOnDisk(const QString &path)
{
#if defined(Q_OS_UNIX)
    struct stat st;
    return ::lstat(QFile::encodeName(path).constData(), &st) == 0 ? qint64(st.st_blocks) * 512 : -1;
#else
    const QFileInfo info(path);
    return info.exists() ? (info.isDir() ? 0 : info.size()) : -1;
#endif
}

bool DiskUsageModel::removePath(const QString &path, qint64 knownBytes)
{
    const QString clean = QDir::cleanPath(path);
    Node *node = findNode(clean);
    if (!node && knownBytes >= 0 && knownBytes < MinFileBytes) {
        // A small file: it was part of its folder's "smaller files".
        Node *parent = findNode(QFileInfo(clean).absolutePath());
        if (!parent || parent->kind != Node::Folder || parent->smallerCount == 0)
            return false;
        parent->smallerBytes = qMax<qint64>(0, parent->smallerBytes - knownBytes);
        --parent->smallerCount;
        for (Node *n = parent; n; n = n->parent) {
            n->bytes -= knownBytes;
            n->items -= 1;
        }
        if (parent->smaller && parent->smallerCount == 0 && parent->rowsBuilt && isShown(parent)) {
            const int row = parent->smaller->row;
            beginRemoveRows(indexFor(parent), row, row);
            parent->rows.erase(parent->rows.begin() + row);
            endRemoveRows();
            parent->smaller.reset();
        }
        relayout();
        return true;
    }
    if (!node || !m_tree || node == m_tree.get())
        return false;
    // The shown folder (or one above it) went away: nothing sensible to show.
    for (const Node *n = m_root; n; n = n->parent) {
        if (n == node) {
            beginResetModel();
            m_tree.reset();
            m_root = nullptr;
            m_complete = false;
            endResetModel();
            return true;
        }
    }
    Node *parent = node->parent;
    const qint64 bytes = node->bytes;
    const qint64 items = node->items + 1;
    if (parent->rowsBuilt && isShown(parent)) {
        const int row = int(std::find(parent->rows.begin(), parent->rows.end(), node) - parent->rows.begin());
        if (row < int(parent->rows.size())) {
            beginRemoveRows(indexFor(parent), row, row);
            parent->rows.erase(parent->rows.begin() + row);
            for (size_t i = size_t(row); i < parent->rows.size(); ++i)
                parent->rows[i]->row = int(i);
            endRemoveRows();
        }
    }
    auto &list = node->kind == Node::File ? parent->files : parent->children;
    list.erase(std::find_if(list.begin(), list.end(), [node](const auto &p) { return p.get() == node; }));
    for (Node *n = parent; n; n = n->parent) {
        n->bytes -= bytes;
        n->items -= items;
    }
    relayout(); // sizes changed all the way up: order and every share
    return true;
}

bool DiskUsageModel::addPath(const QString &path)
{
    const QString clean = QDir::cleanPath(path);
    if (!m_complete || !m_tree || findNode(clean))
        return false;
    Node *parent = findNode(QFileInfo(clean).absolutePath());
    if (!parent || parent->kind != Node::Folder)
        return false;
    const quint64 generation = m_generation;
    m_pool.start([this, clean, generation] {
        QAtomicInteger<qint64> bytes, items; // progress isn't shown for these
        Scanner scanner{m_generation, generation, bytes, items};
        auto node = std::make_unique<Node>();
        const QFileInfo info(clean);
        node->name = info.fileName();
        node->mtime = info.lastModified().toMSecsSinceEpoch();
#if defined(Q_OS_UNIX)
        struct stat st;
        if (::lstat(QFile::encodeName(clean).constData(), &st) != 0)
            return;
        scanner.device = st.st_dev;
        node->bytes = qint64(st.st_blocks) * 512;
        const bool isDir = S_ISDIR(st.st_mode);
#else
        const bool isDir = info.isDir() && !info.isSymLink();
        node->bytes = isDir ? 0 : info.size();
#endif
        if (isDir) {
            scanner.scan(clean, node.get(), attach, noListing);
            fixParents(node.get());
        } else {
            node->kind = Node::File;
        }
        if (scanner.canceled())
            return;
        Node *raw = node.release();
        QMetaObject::invokeMethod(this, [this, generation, clean, raw] { applyAdded(generation, clean, raw); },
                                  Qt::QueuedConnection);
    });
    return true;
}

void DiskUsageModel::applyAdded(quint64 generation, const QString &path, Node *raw)
{
    std::unique_ptr<Node> node(raw);
    if (generation != m_generation || !m_tree || findNode(path))
        return;
    Node *parent = findNode(QFileInfo(path).absolutePath());
    if (!parent || parent->kind != Node::Folder)
        return;
    node->parent = parent;
    for (Node *n = parent; n; n = n->parent) {
        n->bytes += node->bytes;
        n->items += node->items + 1;
    }
    const bool isFile = node->kind == Node::File;
    if (isFile && node->bytes < MinFileBytes) {
        // Joins the "smaller files" row.
        parent->smallerBytes += node->bytes;
        ++parent->smallerCount;
        const bool newRow = parent->smallerCount == 1 && m_showFiles && parent->rowsBuilt && isShown(parent);
        if (newRow) {
            beginInsertRows(indexFor(parent), int(parent->rows.size()), int(parent->rows.size()));
            invalidateRows(parent);
            ensureRows(parent);
            endInsertRows();
        } else if (parent->smaller) {
            parent->smaller->bytes = parent->smallerBytes;
            parent->smaller->items = parent->smallerCount;
        }
    } else {
        Node *added = node.get();
        (isFile ? parent->files : parent->children).push_back(std::move(node));
        if (parent->rowsBuilt && isShown(parent) && (!isFile || m_showFiles)) {
            const int row = int(parent->rows.size());
            beginInsertRows(indexFor(parent), row, row);
            parent->rows.push_back(added);
            added->row = row;
            endInsertRows();
        } else {
            invalidateRows(parent);
        }
    }
    relayout();
    emit added(path);
}

// ------------------------------------------------------------- helpers ---

QString DiskUsageModel::pathOf(const Node *node) const
{
    if (!node || node->kind == Node::SmallerFiles)
        return {};
    QStringList parts;
    for (const Node *n = node; n && n != m_tree.get(); n = n->parent)
        parts.prepend(n->name);
    if (parts.isEmpty())
        return m_treePath;
    return (m_treePath.endsWith(QLatin1Char('/')) ? m_treePath : m_treePath + QLatin1Char('/'))
           + parts.join(QLatin1Char('/'));
}

DiskUsageModel::Node *DiskUsageModel::findNode(const QString &path) const
{
    if (!m_tree)
        return nullptr;
    if (path == m_treePath)
        return m_tree.get();
    const QString prefix = m_treePath.endsWith(QLatin1Char('/')) ? m_treePath : m_treePath + QLatin1Char('/');
    if (!path.startsWith(prefix))
        return nullptr;
    Node *node = m_tree.get();
    const QStringList parts = path.mid(prefix.size()).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (int i = 0; i < parts.size(); ++i) {
        Node *next = nullptr;
        for (const auto &child : node->children) {
            if (child->name == parts.at(i)) {
                next = child.get();
                break;
            }
        }
        if (!next && i == parts.size() - 1) { // the last part can be a file
            for (const auto &file : node->files) {
                if (file->name == parts.at(i)) {
                    next = file.get();
                    break;
                }
            }
        }
        if (!next)
            return nullptr;
        node = next;
    }
    return node;
}

bool DiskUsageModel::isShown(const Node *node) const
{
    for (const Node *n = node; n; n = n->parent) {
        if (n == m_root)
            return true;
    }
    return false;
}

bool DiskUsageModel::lessThan(const Node *a, const Node *b) const
{
    // The "smaller files" row always comes last.
    if ((a->kind == Node::SmallerFiles) != (b->kind == Node::SmallerFiles))
        return b->kind == Node::SmallerFiles;
    int c = 0;
    switch (m_sortColumn) {
    case NameColumn:
        c = collator().compare(a->name, b->name);
        break;
    case ItemsColumn:
        c = a->items < b->items ? -1 : (a->items > b->items ? 1 : 0);
        break;
    default: // Usage and Size
        c = a->bytes < b->bytes ? -1 : (a->bytes > b->bytes ? 1 : 0);
        break;
    }
    if (c == 0)
        c = collator().compare(a->name, b->name) * (m_sortOrder == Qt::AscendingOrder ? 1 : -1);
    return m_sortOrder == Qt::AscendingOrder ? c < 0 : c > 0;
}

void DiskUsageModel::ensureRows(Node *node) const
{
    if (node->rowsBuilt)
        return;
    node->rowsBuilt = true;
    node->rows.clear();
    for (const auto &child : node->children)
        node->rows.push_back(child.get());
    if (m_showFiles) {
        for (const auto &file : node->files)
            node->rows.push_back(file.get());
        if (node->smallerCount > 0) {
            if (!node->smaller) {
                node->smaller = std::make_unique<Node>();
                node->smaller->kind = Node::SmallerFiles;
                node->smaller->parent = node;
            }
            node->smaller->bytes = node->smallerBytes;
            node->smaller->items = node->smallerCount;
            node->rows.push_back(node->smaller.get());
        }
    }
    std::stable_sort(node->rows.begin(), node->rows.end(),
                     [this](const Node *a, const Node *b) { return lessThan(a, b); });
    for (size_t i = 0; i < node->rows.size(); ++i)
        node->rows[i]->row = int(i);
}

void DiskUsageModel::invalidateRows(Node *node) const
{
    node->rowsBuilt = false;
    node->rows.clear();
    node->rows.shrink_to_fit();
    for (const auto &child : node->children)
        invalidateRows(child.get());
}

void DiskUsageModel::relayout()
{
    if (!m_tree)
        return;
    emit layoutAboutToBeChanged();
    const QModelIndexList before = persistentIndexList();
    std::vector<std::pair<Node *, int>> nodes;
    nodes.reserve(size_t(before.size()));
    for (const QModelIndex &idx : before)
        nodes.emplace_back(static_cast<Node *>(idx.internalPointer()), idx.column());
    invalidateRows(m_tree.get());
    QModelIndexList after;
    after.reserve(before.size());
    for (const auto &[node, column] : nodes)
        after.append(isShown(node) ? indexFor(node, column) : QModelIndex());
    changePersistentIndexList(before, after);
    emit layoutChanged();
}

QModelIndex DiskUsageModel::indexFor(const Node *node, int column) const
{
    if (!node)
        return {};
    if (node == m_root)
        return createIndex(0, column, const_cast<Node *>(node));
    ensureRows(node->parent);
    return createIndex(node->row, column, const_cast<Node *>(node));
}

void DiskUsageModel::sort(int column, Qt::SortOrder order)
{
    if (column < 0 || column >= ColumnCount || (column == m_sortColumn && order == m_sortOrder))
        return;
    m_sortColumn = column;
    m_sortOrder = order;
    relayout();
}

QIcon DiskUsageModel::iconFor(const Node *node) const
{
    if (node->kind == Node::Folder)
        return m_folderIcon;
    if (node->kind == Node::SmallerFiles)
        return m_fileIcon;
    static const QMimeDatabase db;
    const QMimeType mime = db.mimeTypeForFile(node->name, QMimeDatabase::MatchExtension);
    auto it = m_iconByMime.constFind(mime.name());
    if (it == m_iconByMime.constEnd())
        it = m_iconByMime.insert(mime.name(),
                                 QIcon::fromTheme(mime.iconName(), QIcon::fromTheme(mime.genericIconName(), m_fileIcon)));
    return *it;
}

// --------------------------------------------------------------- model ---

QModelIndex DiskUsageModel::index(int row, int column, const QModelIndex &parent) const
{
    if (column < 0 || column >= ColumnCount || row < 0 || !m_root)
        return {};
    if (!parent.isValid())
        return row == 0 ? createIndex(0, column, m_root) : QModelIndex();
    Node *p = static_cast<Node *>(parent.internalPointer());
    ensureRows(p);
    if (row >= int(p->rows.size()))
        return {};
    return createIndex(row, column, p->rows[size_t(row)]);
}

QModelIndex DiskUsageModel::parent(const QModelIndex &child) const
{
    if (!child.isValid())
        return {};
    const Node *node = static_cast<const Node *>(child.internalPointer());
    if (node == m_root || !node->parent)
        return {};
    return indexFor(node->parent);
}

int DiskUsageModel::rowCount(const QModelIndex &parent) const
{
    if (!m_root)
        return 0;
    if (!parent.isValid())
        return 1;
    if (parent.column() != 0)
        return 0;
    Node *p = static_cast<Node *>(parent.internalPointer());
    if (p->kind != Node::Folder)
        return 0;
    ensureRows(p);
    return int(p->rows.size());
}

int DiskUsageModel::columnCount(const QModelIndex &) const
{
    return ColumnCount;
}

bool DiskUsageModel::hasChildren(const QModelIndex &parent) const
{
    if (!parent.isValid())
        return m_root != nullptr;
    if (parent.column() != 0)
        return false;
    const Node *p = static_cast<const Node *>(parent.internalPointer());
    return p->kind == Node::Folder
           && (!p->children.empty() || (m_showFiles && (!p->files.empty() || p->smallerCount > 0)));
}

qint64 DiskUsageModel::rootBytes() const
{
    return m_root ? m_root->bytes : 0;
}

qint64 DiskUsageModel::rootItems() const
{
    return m_root ? m_root->items : 0;
}

bool DiskUsageModel::rootIncomplete() const
{
    return m_root && m_root->incomplete;
}

QString DiskUsageModel::filePath(const QModelIndex &index) const
{
    return index.isValid() ? pathOf(static_cast<const Node *>(index.internalPointer())) : QString();
}

QModelIndex DiskUsageModel::indexForPath(const QString &path) const
{
    const Node *node = findNode(QDir::cleanPath(path));
    if (!node || !isShown(node) || (node->kind == Node::File && !m_showFiles))
        return {};
    return indexFor(node);
}

QVariant DiskUsageModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case NameColumn:
        return m_showFiles ? tr("Name") : tr("Folder");
    case UsageColumn:
        return tr("Usage");
    case SizeColumn:
        return tr("Size");
    case ItemsColumn:
        return tr("Contents");
    }
    return {};
}

QVariant DiskUsageModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return {};
    const Node *node = static_cast<const Node *>(index.internalPointer());
    const bool top = node == m_root;
    const double fraction = top || !node->parent || node->parent->bytes <= 0
                                ? 1.0
                                : double(node->bytes) / double(node->parent->bytes);
    switch (role) {
    case Qt::DisplayRole:
        switch (index.column()) {
        case NameColumn:
            if (node->kind == Node::SmallerFiles) {
                const bool others = !node->parent->files.empty();
                const qint64 n = node->items;
                if (n == 1)
                    return others ? tr("1 smaller file") : tr("1 file");
                return (others ? tr("%1 smaller files") : tr("%1 files")).arg(QLocale().toString(n));
            }
            return node->name;
        case UsageColumn:
            return QStringLiteral("%1%").arg(qRound(fraction * 100));
        case SizeColumn:
            return formatSize(node->bytes);
        case ItemsColumn:
            if (node->kind != Node::Folder)
                return QString(); // files have no contents; the row's name already counts them
            return node->items == 1 ? tr("1 item") : tr("%1 items").arg(QLocale().toString(node->items));
        }
        break;
    case Qt::DecorationRole:
        if (index.column() == NameColumn)
            return iconFor(node);
        break;
    case Qt::FontRole:
        if (node->kind == Node::SmallerFiles && index.column() == NameColumn) {
            QFont font;
            font.setItalic(true);
            return font;
        }
        break;
    case Qt::ToolTipRole:
        if (index.column() == NameColumn) {
            if (node->kind == Node::SmallerFiles)
                return tr("Files smaller than %1, or beyond the %2 biggest, in “%3”")
                    .arg(formatSize(MinFileBytes))
                    .arg(MaxFilesPerFolder)
                    .arg(node->parent->name);
            QString tip = QDir::toNativeSeparators(pathOf(node));
            if (node->incomplete)
                tip += QLatin1Char('\n') + tr("Some folders inside couldn't be read, so the real size may be bigger.");
            return tip;
        }
        break;
    case Qt::TextAlignmentRole:
        if (index.column() == SizeColumn || index.column() == ItemsColumn)
            return int(Qt::AlignRight | Qt::AlignVCenter);
        break;
    case PathRole:
        return pathOf(node);
    case FractionRole:
        return fraction;
    case BytesRole:
        return node->bytes;
    case ItemsRole:
        return node->items;
    case KindRole:
        return int(node->kind);
    }
    return {};
}
