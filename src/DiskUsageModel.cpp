#include "DiskUsageModel.h"

#include "Format.h"

#include <QCollator>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QLocale>
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

// One scan on the worker thread. Measures a folder tree into Nodes.
struct Scanner
{
    const QAtomicInteger<quint64> &generation;
    quint64 mine;
    QAtomicInteger<qint64> &bytesSeen;
    QAtomicInteger<qint64> &itemsSeen;
#if defined(Q_OS_UNIX)
    dev_t device = 0;
    QSet<QPair<quint64, quint64>> hardLinks; // (dev, inode) of files seen with nlink > 1
#endif

    bool canceled() const { return generation.loadRelaxed() != mine; }

    // Measures what's inside `path` into `node` (its own entry is counted by
    // the caller). `onChild` is called for each sub-folder once measured.
    template<typename OnChild>
    void scan(const QString &path, Node *node, OnChild onChild)
    {
#if defined(Q_OS_UNIX)
        const QByteArray native = QFile::encodeName(path);
        const int fd = ::open(native.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        DIR *dir = fd >= 0 ? ::fdopendir(fd) : nullptr;
        if (!dir) {
            if (fd >= 0)
                ::close(fd);
            node->incomplete = true;
            return;
        }
        // Read the folder first, then go into sub-folders, so only one
        // folder is open at a time however deep the tree is.
        struct Sub
        {
            QString name;
            qint64 mtime;
            qint64 used; // the folder's own entry: counted in its own size
        };
        std::vector<Sub> subs;
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
            bytesSeen.fetchAndAddRelaxed(used);
            if (isDir && st.st_dev == device)
                subs.push_back({QFile::decodeName(name),
                                qint64(st.st_mtim.tv_sec) * 1000 + st.st_mtim.tv_nsec / 1000000, used});
            else
                node->bytes += used; // files, links, and mount points (not entered)
        }
        ::closedir(dir);
        for (const Sub &sub : subs) {
            if (canceled())
                return;
            auto child = std::make_unique<Node>();
            child->name = sub.name;
            child->mtime = sub.mtime;
            child->bytes = sub.used;
            scan(path + QLatin1Char('/') + sub.name, child.get(), attach);
            onChild(node, std::move(child));
        }
#else
        QDirIterator it(path, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        std::vector<QFileInfo> subs;
        while (it.hasNext()) {
            if (canceled())
                return;
            it.next();
            const QFileInfo fi = it.fileInfo();
            ++node->items;
            itemsSeen.fetchAndAddRelaxed(1);
            if (fi.isSymLink())
                continue;
            if (fi.isDir()) {
                subs.push_back(fi);
            } else {
                node->bytes += fi.size();
                bytesSeen.fetchAndAddRelaxed(fi.size());
            }
        }
        for (const QFileInfo &fi : subs) {
            if (canceled())
                return;
            auto child = std::make_unique<Node>();
            child->name = fi.fileName();
            child->mtime = fi.lastModified().toMSecsSinceEpoch();
            scan(fi.absoluteFilePath(), child.get(), attach);
            onChild(node, std::move(child));
        }
#endif
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
    for (size_t i = 0; i < node->children.size(); ++i) {
        node->children[i]->parent = node;
        node->children[i]->row = int(i);
        fixParents(node->children[i].get());
    }
}

QCollator &collator()
{
    static QCollator c = [] {
        QLocale locale;
        if (locale.language() == QLocale::C)
            locale = QLocale(QLocale::English);
        QCollator coll(locale);
        coll.setNumericMode(true);
        coll.setCaseSensitivity(Qt::CaseInsensitive);
        return coll;
    }();
    return c;
}

} // namespace

DiskUsageModel::DiskUsageModel(QObject *parent)
    : QAbstractItemModel(parent)
{
    m_pool.setMaxThreadCount(1);
    m_progressTimer.setInterval(100);
    connect(&m_progressTimer, &QTimer::timeout, this, &DiskUsageModel::progress);
    m_folderIcon = QFileIconProvider().icon(QFileIconProvider::Folder);
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
        if (Node *node = findNode(clean)) {
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
        Node own; // files directly in the folder (sub-folders are sent one by one)
        scanner.scan(path, &own, [&](Node *, std::unique_ptr<Node> child) {
            if (scanner.canceled())
                return;
            fixParents(child.get());
            Node *raw = child.release();
            QMetaObject::invokeMethod(this, [this, generation, raw] { addTopChild(generation, raw); },
                                      Qt::QueuedConnection);
        });
        if (scanner.canceled())
            return;
        qint64 ownBytes = own.bytes;
#if defined(Q_OS_UNIX)
        ownBytes += qint64(st.st_blocks) * 512; // the folder's own entry, like du
#endif
        QMetaObject::invokeMethod(
            this,
            [this, generation, ownBytes, items = own.items, incomplete = own.incomplete] {
                finishScan(generation, ownBytes, items, incomplete);
            },
            Qt::QueuedConnection);
    });
}

// A top-level sub-folder, measured with everything inside it.
void DiskUsageModel::addTopChild(quint64 generation, Node *raw)
{
    std::unique_ptr<Node> child(raw);
    if (generation != m_generation || !m_tree)
        return;
    Node *top = m_tree.get();
    child->parent = top;
    sortChildren(child.get(), true);
    const int pos = insertPosition(top, child.get());
    const bool shown = m_root == top;
    if (shown)
        beginInsertRows(indexFor(top), pos, pos);
    top->bytes += child->bytes;
    top->items += child->items;
    top->incomplete = top->incomplete || child->incomplete;
    top->children.insert(top->children.begin() + pos, std::move(child));
    for (size_t i = pos; i < top->children.size(); ++i)
        top->children[i]->row = int(i);
    if (shown) {
        endInsertRows();
        // The top row grew, so every share of it changed.
        emit dataChanged(indexFor(top), indexFor(top, ColumnCount - 1));
        if (!top->children.empty())
            emit dataChanged(index(0, 0, indexFor(top)),
                             index(int(top->children.size()) - 1, ColumnCount - 1, indexFor(top)));
    }
}

void DiskUsageModel::finishScan(quint64 generation, qint64 ownBytes, qint64 ownItems, bool incomplete)
{
    if (generation != m_generation || !m_tree)
        return;
    Node *top = m_tree.get();
    // `own` counted sub-folders as items and bytes already (their entries);
    // their contents came separately through addTopChild.
    top->bytes += ownBytes;
    top->items += ownItems;
    top->incomplete = top->incomplete || incomplete;
    m_scanning = false;
    m_complete = true;
    m_progressTimer.stop();
    m_scanMs = m_scanTimer.elapsed();
    if (m_root == top) {
        emit dataChanged(indexFor(top), indexFor(top, ColumnCount - 1));
        if (!top->children.empty())
            emit dataChanged(index(0, 0, indexFor(top)),
                             index(int(top->children.size()) - 1, ColumnCount - 1, indexFor(top)));
    }
    emit finished();
}

bool DiskUsageModel::removePath(const QString &path)
{
    Node *node = findNode(QDir::cleanPath(path));
    if (!node || !m_tree)
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
    // Is `parent` visible (the shown root or below it)?
    bool shown = false;
    for (const Node *n = parent; n; n = n->parent)
        shown = shown || n == m_root;
    const int row = node->row;
    if (shown)
        beginRemoveRows(indexFor(parent), row, row);
    parent->children.erase(parent->children.begin() + row);
    for (size_t i = row; i < parent->children.size(); ++i)
        parent->children[i]->row = int(i);
    if (shown)
        endRemoveRows();
    for (Node *n = parent; n; n = n->parent) {
        n->bytes -= bytes;
        n->items -= items;
    }
    if (shown) {
        // Sizes changed all the way up, and so did every share.
        beginResetModel(); // cheap: the tree stays, views re-read what's visible
        endResetModel();
    }
    return true;
}

// ------------------------------------------------------------- helpers ---

QString DiskUsageModel::pathOf(const Node *node) const
{
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
    for (const QString &part : parts) {
        Node *next = nullptr;
        for (const auto &child : node->children) {
            if (child->name == part) {
                next = child.get();
                break;
            }
        }
        if (!next)
            return nullptr;
        node = next;
    }
    return node;
}

bool DiskUsageModel::lessThan(const Node *a, const Node *b) const
{
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

void DiskUsageModel::sortChildren(Node *node, bool recursive)
{
    std::stable_sort(node->children.begin(), node->children.end(),
                     [this](const auto &a, const auto &b) { return lessThan(a.get(), b.get()); });
    for (size_t i = 0; i < node->children.size(); ++i) {
        node->children[i]->row = int(i);
        if (recursive)
            sortChildren(node->children[i].get(), true);
    }
}

int DiskUsageModel::insertPosition(const Node *parent, const Node *child) const
{
    const auto it = std::lower_bound(parent->children.begin(), parent->children.end(), child,
                                     [this](const auto &a, const Node *b) { return lessThan(a.get(), b); });
    return int(it - parent->children.begin());
}

QModelIndex DiskUsageModel::indexFor(const Node *node, int column) const
{
    if (!node)
        return {};
    return createIndex(node == m_root ? 0 : node->row, column, const_cast<Node *>(node));
}

void DiskUsageModel::sort(int column, Qt::SortOrder order)
{
    if (column < 0 || column >= ColumnCount || (column == m_sortColumn && order == m_sortOrder))
        return;
    m_sortColumn = column;
    m_sortOrder = order;
    if (!m_tree)
        return;
    emit layoutAboutToBeChanged();
    const QModelIndexList before = persistentIndexList();
    sortChildren(m_tree.get(), true);
    QModelIndexList after;
    after.reserve(before.size());
    for (const QModelIndex &idx : before)
        after.append(indexFor(static_cast<Node *>(idx.internalPointer()), idx.column()));
    changePersistentIndexList(before, after);
    emit layoutChanged();
}

// --------------------------------------------------------------- model ---

QModelIndex DiskUsageModel::index(int row, int column, const QModelIndex &parent) const
{
    if (column < 0 || column >= ColumnCount || row < 0 || !m_root)
        return {};
    if (!parent.isValid())
        return row == 0 ? createIndex(0, column, m_root) : QModelIndex();
    const Node *p = static_cast<const Node *>(parent.internalPointer());
    if (row >= int(p->children.size()))
        return {};
    return createIndex(row, column, p->children[size_t(row)].get());
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
    return int(static_cast<const Node *>(parent.internalPointer())->children.size());
}

int DiskUsageModel::columnCount(const QModelIndex &) const
{
    return ColumnCount;
}

bool DiskUsageModel::hasChildren(const QModelIndex &parent) const
{
    return rowCount(parent) > 0;
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
    for (const Node *n = node; n; n = n->parent) {
        if (n == m_root)
            return indexFor(node);
    }
    return {}; // not under the shown folder
}

QVariant DiskUsageModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case NameColumn:
        return tr("Folder");
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
            return node->name;
        case UsageColumn:
            return QStringLiteral("%1%").arg(qRound(fraction * 100));
        case SizeColumn:
            return formatSize(node->bytes);
        case ItemsColumn:
            return node->items == 1 ? tr("1 item") : tr("%1 items").arg(QLocale().toString(node->items));
        }
        break;
    case Qt::DecorationRole:
        if (index.column() == NameColumn)
            return m_folderIcon;
        break;
    case Qt::ToolTipRole:
        if (index.column() == NameColumn) {
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
    }
    return {};
}
