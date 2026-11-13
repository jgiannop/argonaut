#pragma once

#include <QAbstractItemModel>
#include <QAtomicInteger>
#include <QElapsedTimer>
#include <QIcon>
#include <QMutex>
#include <QThreadPool>
#include <QTimer>

#include <memory>
#include <vector>

// The "Analyze" view: how much disk space each folder and sub-folder uses,
// as a tree (one top row: the analysed folder), biggest first.
//
//  - Sizes are space used on disk (allocated blocks, like `du` and GNOME's
//    Disk Usage Analyzer), counting hard-linked files once. Symlinks aren't
//    followed and other drives mounted inside aren't entered.
//  - Scanning runs on a worker thread. Each top-level sub-folder appears as
//    soon as it's measured, with its whole sub-tree.
//  - Once a folder is scanned, showing any folder inside it is instant
//    (setRoot re-roots the tree instead of scanning again).
//  - Not watched: removePath() takes out what the app itself trashed or
//    moved; rescan() starts over.
class DiskUsageModel : public QAbstractItemModel
{
    Q_OBJECT

public:
    enum Column { NameColumn, UsageColumn, SizeColumn, ItemsColumn, ColumnCount };
    enum Role {
        PathRole = Qt::UserRole + 1, // QString
        FractionRole,                // double: share of the parent folder (top row: 1)
        BytesRole,                   // qint64
        ItemsRole,                   // qint64: files and folders inside, at any depth
    };

    explicit DiskUsageModel(QObject *parent = nullptr);
    ~DiskUsageModel() override;

    // Show `path` (clean, '/' separators): re-roots an existing complete scan
    // that contains it, otherwise scans it.
    void setRoot(const QString &path);
    QString rootPath() const;
    void rescan();
    bool isScanning() const { return m_scanning; }
    qint64 rootBytes() const;
    qint64 rootItems() const;
    bool rootIncomplete() const; // some folders inside couldn't be read
    qint64 scanMs() const { return m_scanMs; }
    // While scanning: what has been measured so far.
    qint64 scannedBytes() const { return m_scannedBytes.loadRelaxed(); }
    qint64 scannedItems() const { return m_scannedItems.loadRelaxed(); }

    // Something inside was trashed/moved/deleted by the app: take it out and
    // subtract its size from the folders above. Returns false if unknown.
    bool removePath(const QString &path);

    QString filePath(const QModelIndex &index) const;
    QModelIndex indexForPath(const QString &path) const;

    QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    bool hasChildren(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

    struct Node
    {
        QString name;
        Node *parent = nullptr;
        std::vector<std::unique_ptr<Node>> children; // folders only
        qint64 bytes = 0;   // on disk, this folder and everything inside
        qint64 items = 0;   // files and folders inside, at any depth
        qint64 mtime = 0;   // ms since epoch
        bool incomplete = false; // something inside couldn't be read
        int row = 0;        // index in parent->children
    };

signals:
    void progress(); // while scanning, every ~100 ms
    void finished(); // scan complete (also after re-rooting)

private:
    void startScan(const QString &path);
    void addTopChild(quint64 generation, Node *child);
    void finishScan(quint64 generation, qint64 ownBytes, qint64 ownItems, bool incomplete);
    QString pathOf(const Node *node) const;
    Node *findNode(const QString &path) const;
    bool lessThan(const Node *a, const Node *b) const;
    void sortChildren(Node *node, bool recursive);
    int insertPosition(const Node *parent, const Node *child) const;
    QModelIndex indexFor(const Node *node, int column = 0) const;

    std::unique_ptr<Node> m_tree; // the scanned folder
    QString m_treePath;           // its path
    Node *m_root = nullptr;       // what's shown (m_tree or a folder inside)
    bool m_scanning = false;
    bool m_complete = false;
    qint64 m_scanMs = 0;
    QElapsedTimer m_scanTimer;
    int m_sortColumn = SizeColumn;
    Qt::SortOrder m_sortOrder = Qt::DescendingOrder;

    QAtomicInteger<quint64> m_generation{0};
    QAtomicInteger<qint64> m_scannedBytes{0};
    QAtomicInteger<qint64> m_scannedItems{0};
    QTimer m_progressTimer;
    QThreadPool m_pool;
    QIcon m_folderIcon;
};
