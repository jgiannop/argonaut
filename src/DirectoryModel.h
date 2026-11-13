#pragma once

#include <QAbstractTableModel>
#include <QAtomicInteger>
#include <QCollatorSortKey>
#include <QFileIconProvider>
#include <QHash>
#include <QIcon>
#include <QThreadPool>
#include <QSet>
#include <QTimer>

#include "ContentType.h"

#include <vector>

class QFileSystemWatcher;
class QSocketNotifier;

// One directory entry. Everything the views need is read once while
// listing (one stat per file), so painting and sorting never touch the disk.
struct FileEntry
{
    FileEntry(QString n, QCollatorSortKey k) : name(std::move(n)), key(std::move(k)) {}

    QString name;
    QString dir;              // folder that contains it
    QCollatorSortKey key;     // natural, case-insensitive order ("file2" < "file10")
    mutable QString mimeName; // from the name, computed on demand
    qint64 size = 0;
    qint64 mtime = 0;         // ms since epoch
    int childCount = -1;      // folders: entries inside, -1 = not counted yet
    bool isDir = false;
    bool isSymLink = false;
    bool isHidden = false;
    // Real type from the first bytes (ContentType), filled in the background.
    ContentType::Verdict verdict = ContentType::Verdict::NotChecked;
    QString contentMime;
    // Trash view only (from .trashinfo):
    QString originalPath;
    qint64 deleted = 0; // ms since epoch
    // Recent view only: when it was last used (ms since epoch).
    qint64 used = 0;
    QString fullPath; // dir + name, built once when listed (hashing and lookups use it a lot)
    const QString &path() const { return fullPath; }
    void setDir(const QString &folder)
    {
        dir = folder;
        fullPath = folder.endsWith(QLatin1Char('/')) ? folder + name : folder + QLatin1Char('/') + name;
    }
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
        OriginalPathRole,            // QString, Trash view only
        VerdictRole,                 // int, ContentType::Verdict
        NameMimeRole,                // QString: what the name says
        ContentMimeRole,             // QString: what the content says (if checked)
        StarredRole,                 // bool
    };
    // Places that aren't folders: lists of items from anywhere. setPath()
    // takes these paths; they're never on disk.
    enum class Place { Folder, Recent, Starred };
    static QString recentPath() { return QStringLiteral("recent:///"); }
    static QString starredPath() { return QStringLiteral("starred:///"); }
    static Place placeOf(const QString &path);
    static bool isVirtualPath(const QString &path) { return placeOf(path) != Place::Folder; }

    explicit DirectoryModel(QObject *parent = nullptr);
    ~DirectoryModel() override;

    // Show `path` (clean, '/' separators). Loads asynchronously; loaded()
    // fires when the listing is complete.
    void setPath(const QString &path);
    QString path() const { return m_path; }
    bool isLoading() const { return m_loading; }
    // Showing the Trash: all trash folders merged, with "Original Location"
    // and "Deleted" instead of "Type" and "Modified".
    bool isTrash() const { return m_trash; }
    // Showing Recent or Starred (items from many folders, with a Location column).
    Place place() const { return m_place; }
    bool isVirtual() const { return m_place != Place::Folder; }
    // Re-read the folder soon (changes are also picked up automatically).
    void refresh();

    // Search: with a query, the model shows every item under path() whose
    // name contains all its words (ignoring case and accents), found by a
    // walk on a worker thread. The Type column becomes "Location". An empty
    // query shows the folder again. setPath() always ends the search.
    void setSearch(const QString &query);
    QString searchQuery() const { return m_query; }
    bool isSearch() const { return !m_query.isEmpty(); }
    bool searchTruncated() const { return m_searchTruncated; } // hit MaxSearchResults
    static constexpr int MaxSearchResults = 10000;
    // Matching, exposed for tests: words = foldForSearch(query).split(' ').
    static QString foldForSearch(const QString &text);
    static bool matchesSearch(const QString &name, const QStringList &foldedWords);

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

    void relist(); // start over (folder or search changed)
    void watch(const QStringList &dirs);
    void folderChanged();
    void startScan(bool initial);
    void scheduleRescan();
    void applyBatch(quint64 generation, Entries batch, bool done);
    void applyRescan(quint64 generation, Entries fresh);
    void startCounting();
    void applyCounts(quint64 generation, const QHash<QString, int> &counts);

    bool lessThan(const FileEntry &a, const FileEntry &b) const;
    void sortRows();
    // Re-sort with layoutChanged, keeping persistent indexes. With
    // `sortedPrefix` >= 0 only rows from there on are new: they're sorted and
    // merged in (one pass) instead of sorting everything again.
    void resort(int sortedPrefix = -1);
    void rebuildIndex();      // marks m_rowByPath stale (cheap)
    void ensureIndex() const; // rebuilds it if stale, on first use
    bool isShown(const FileEntry &e) const { return m_showHidden || !e.isHidden; }

    const QString &mimeFor(const FileEntry &e) const;     // effective: content if it knows better
    const QString &nameMimeFor(const FileEntry &e) const; // from the name only
    // Content checks: cached results apply at once; the rest are read on a
    // worker (every file on local disks, visible ones on USB/network drives).
    void applyCachedContent(FileEntry &e) const;
    void wantContent(const FileEntry &e) const;
    void queueAllUnchecked();
    void flushContentQueue();
    struct ContentResult
    {
        QString path;
        qint64 size;
        qint64 mtime;
        ContentType::Result result;
    };
    void applyContent(quint64 generation, const QList<ContentResult> &results);
    QIcon iconFor(const FileEntry &e) const;
    QString typeFor(const FileEntry &e) const;
    QString searchLocation(const FileEntry &e) const;

    QString m_path;
    QStringList m_dirs; // folders listed: just m_path, or every trash folder
    bool m_trash = false;
    Place m_place = Place::Folder;
    QString m_query;                 // search (empty: just the folder)
    bool m_searchTruncated = false;
    Entries m_rows;   // shown entries, sorted
    Entries m_hidden; // hidden entries while they aren't shown
    mutable QHash<QString, int> m_rowByPath; // full path -> row; built lazily
    mutable bool m_indexStale = false;

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
    QList<int> m_watchIds;
    QTimer m_rescanTimer;
    QThreadPool m_pool;
    QThreadPool m_contentPool;
    bool m_thorough = true; // check every file (local disk) or only visible ones
    struct Wanted
    {
        QString path;
        qint64 size;
        qint64 mtime;
    };
    mutable QList<Wanted> m_wanted;
    mutable QSet<QString> m_queued;
    mutable QTimer m_contentTimer;

    mutable QFileIconProvider m_iconProvider;
    mutable QHash<QString, QIcon> m_iconCache;      // "mime:<type>" or "dir:<path>"
    mutable QHash<QString, QString> m_mimeByName;   // name pattern -> mime type
    mutable QHash<QString, QString> m_typeNames;    // mime type -> description
};
