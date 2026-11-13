#pragma once

#include <QAbstractTableModel>
#include <QAtomicInteger>
#include <QCollatorSortKey>
#include <QFileIconProvider>
#include <QHash>
#include <QIcon>
#include <QThreadPool>
#include <QTimer>

#include <vector>

class QFileSystemWatcher;
class QSocketNotifier;

// One directory entry. Everything the views need is read once while
// listing (one stat per file), so painting and sorting never touch the disk.
struct FileEntry
{
    FileEntry(QString n, QCollatorSortKey k) : name(std::move(n)), key(std::move(k)) {}

    QString name;
    QCollatorSortKey key;     // natural, case-insensitive order ("file2" < "file10")
    mutable QString mimeName; // from the name, computed on demand
    qint64 size = 0;
    qint64 mtime = 0;         // ms since epoch
    int childCount = -1;      // folders: entries inside, -1 = not counted yet
    bool isDir = false;
    bool isSymLink = false;
    bool isHidden = false;
};

// Flat, sortable model of one folder's contents (replaces QFileSystemModel,
// which was ~25x slower than the disk on big folders and couldn't keep
// folders-first natural order as files appeared).
//
//  - Lists on a worker thread; big folders arrive in batches.
//  - Watches only the open folder and applies changes as minimal row
//    inserts/removes/updates, so selection and scroll position survive.
//  - Counts the items inside sub-folders in the background.
class DirectoryModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Column { NameColumn, SizeColumn, TypeColumn, ModifiedColumn, ColumnCount };
    enum Role {
        PathRole = Qt::UserRole + 1, // QString, absolute path
        IsDirRole,                   // bool
        SizeRole,                    // qint64 (files)
        ModifiedRole,                // QDateTime
        FileNameRole,                // QString
    };

    explicit DirectoryModel(QObject *parent = nullptr);
    ~DirectoryModel() override;

    // Show `path` (clean, '/' separators). Loads asynchronously; loaded()
    // fires when the listing is complete.
    void setPath(const QString &path);
    QString path() const { return m_path; }
    bool isLoading() const { return m_loading; }
    // Re-read the folder soon (changes are also picked up automatically).
    void refresh();

    void setShowHidden(bool show);
    bool showHidden() const { return m_showHidden; }

    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;
    int sortColumn() const { return m_sortColumn; }
    Qt::SortOrder sortOrder() const { return m_sortOrder; }

    QModelIndex indexForPath(const QString &path, int column = 0) const;
    QString filePath(const QModelIndex &index) const;
    QString fileName(const QModelIndex &index) const;
    bool isDir(const QModelIndex &index) const;
    qint64 size(const QModelIndex &index) const;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

    // Dragging out: the selected files as a standard uri-list (plus plain
    // text paths for terminals and editors). Drops are handled by the
    // window, not the model.
    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QModelIndexList &indexes) const override;
    Qt::DropActions supportedDragActions() const override;

signals:
    void loaded(const QString &path);     // first complete listing of `path`
    void folderGone(const QString &path); // the open folder was deleted or moved

private:
    using Entries = std::vector<FileEntry>;

    void watch(const QString &path);
    void folderChanged();
    void startScan(bool initial);
    void scheduleRescan();
    void applyBatch(quint64 generation, Entries batch, bool done);
    void applyRescan(quint64 generation, Entries fresh);
    void startCounting();
    void applyCounts(quint64 generation, const QHash<QString, int> &counts);

    bool lessThan(const FileEntry &a, const FileEntry &b) const;
    void sortRows();
    void resort(); // re-sort with layoutChanged, keeping persistent indexes
    void rebuildIndex();
    bool isShown(const FileEntry &e) const { return m_showHidden || !e.isHidden; }
    QString joinPath(const QString &name) const;
    const QString &mimeFor(const FileEntry &e) const;
    QIcon iconFor(const FileEntry &e) const;
    QString typeFor(const FileEntry &e) const;

    QString m_path;
    Entries m_rows;   // shown entries, sorted
    Entries m_hidden; // hidden entries while they aren't shown
    QHash<QString, int> m_rowByName;

    int m_sortColumn = NameColumn;
    Qt::SortOrder m_sortOrder = Qt::AscendingOrder;
    bool m_showHidden = false;
    bool m_loading = false;
    bool m_scanning = false;
    bool m_rescanPending = false;

    QAtomicInteger<quint64> m_generation{0}; // bumped per folder; stale results are dropped
    // Change notifications for the open folder only. Linux uses inotify
    // directly so that files being saved (IN_CLOSE_WRITE) update size and
    // date too; QFileSystemWatcher only reports entries added or removed.
    QFileSystemWatcher *m_watcher = nullptr; // other platforms
    QSocketNotifier *m_inotifyNotifier = nullptr;
    int m_inotifyFd = -1;
    int m_watchId = -1;
    QTimer m_rescanTimer;
    QThreadPool m_pool;

    mutable QFileIconProvider m_iconProvider;
    mutable QHash<QString, QIcon> m_iconCache;      // "mime:<type>" or "dir:<path>"
    mutable QHash<QString, QString> m_mimeByName;   // name pattern -> mime type
    mutable QHash<QString, QString> m_typeNames;    // mime type -> description
};
