#pragma once

#include <QAbstractItemModel>
#include <QAtomicInteger>
#include <QElapsedTimer>
#include <QHash>
#include <QIcon>
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
//  - With setShowFiles(true), each folder also lists its biggest files
//    (up to MaxFilesPerFolder of at least MinFileBytes) and one row for the
//    rest, so the rows of a folder always add up to 100%.
//  - Not watched: removePath()/addPath() apply what the app itself did;
//    rescan() starts over.
class DiskUsageModel : public QAbstractItemModel
{
    Q_OBJECT

public:
    enum Column { NameColumn, UsageColumn, SizeColumn, ItemsColumn, ColumnCount };
    enum Role {
        PathRole = Qt::UserRole + 1, // QString (empty for the "smaller files" row)
        FractionRole,                // double: share of the parent folder (top row: 1)
        BytesRole,                   // qint64
        ItemsRole,                   // qint64: files and folders inside, at any depth
        KindRole,                    // int, Node::Kind
    };
    static constexpr qint64 MinFileBytes = 256 * 1024;
    static constexpr int MaxFilesPerFolder = 10;

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

    void setShowFiles(bool show);
    bool showFiles() const { return m_showFiles; }

    // Something inside was trashed/moved/deleted by the app: take it out and
    // subtract its size from the folders above. Small files aren't in the
    // tree: pass their size (measured where they went, see sizeOnDisk) so
    // the "smaller files" row can be corrected. Returns false if unknown.
    bool removePath(const QString &path, qint64 knownBytes = -1);
    static qint64 sizeOnDisk(const QString &path); // one item, not followed
    // Something new inside (restored, copied, moved, extracted): measure it
    // in the background and add it. Returns false if it isn't inside a
    // complete scan. added(path) fires when it's in.
    bool addPath(const QString &path);

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
        enum Kind : quint8 { Folder, File, SmallerFiles };
        QString name;
        Node *parent = nullptr;
        std::vector<std::unique_ptr<Node>> children; // sub-folders
        std::vector<std::unique_ptr<Node>> files;    // the biggest files directly inside
        std::unique_ptr<Node> smaller;               // row for the other files (made when shown)
        qint64 bytes = 0;        // on disk: the entry itself and everything inside
        qint64 items = 0;        // files and folders inside, at any depth
        qint64 smallerBytes = 0; // files directly inside that aren't in `files`
        qint64 smallerCount = 0;
        qint64 mtime = 0; // ms since epoch
        Kind kind = Folder;
        bool incomplete = false; // something inside couldn't be read
        // What the view shows, sorted; built when first needed.
        std::vector<Node *> rows;
        bool rowsBuilt = false;
        int row = 0; // index in parent->rows
    };

signals:
    void progress(); // while scanning, every ~100 ms
    void finished(); // scan complete (also after re-rooting)
    void added(const QString &path);

private:
    void startScan(const QString &path);
    void addTopFiles(quint64 generation, Node *listed);
    void addTopChild(quint64 generation, Node *child);
    void finishScan(quint64 generation, qint64 ownBytes, bool incomplete);
    void applyAdded(quint64 generation, const QString &path, Node *node);

    QString pathOf(const Node *node) const;
    Node *findNode(const QString &path) const;
    bool isShown(const Node *node) const; // the shown root or inside it
    bool lessThan(const Node *a, const Node *b) const;
    void ensureRows(Node *node) const;
    void invalidateRows(Node *node) const; // whole sub-tree
    // Re-sort everything (sizes or order changed), keeping persistent indexes
    // (and so the view's expanded folders and selection).
    void relayout();
    QModelIndex indexFor(const Node *node, int column = 0) const;
    QIcon iconFor(const Node *node) const;

    std::unique_ptr<Node> m_tree; // the scanned folder
    QString m_treePath;           // its path
    Node *m_root = nullptr;       // what's shown (m_tree or a folder inside)
    bool m_scanning = false;
    bool m_complete = false;
    bool m_showFiles = false;
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
    QIcon m_fileIcon;
    mutable QHash<QString, QIcon> m_iconByMime;
};
