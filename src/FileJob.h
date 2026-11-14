#pragma once

#include <QAtomicInt>
#include <QElapsedTimer>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QWaitCondition>

#include <optional>

class QFileInfo;
class QThreadPool;

// One item to transfer: `source` goes into `targetDir` as `targetName`
// (an existing name is never overwritten: "name (copy).ext" is used instead).
struct Transfer
{
    QString source;
    QString targetDir;
    QString targetName;
};

// What actually happened to one item, used to undo it.
struct TransferResult
{
    QString source;
    QString target; // new path (Copy/Move/Restore) or path inside the trash (Trash)
};

// A file operation that runs on a worker thread. Progress is read with
// progress() (thread-safe); signals are delivered queued to the GUI thread.
class FileJob : public QObject
{
    Q_OBJECT

public:
    // Restore: out of the Trash (removes the .trashinfo, recreates the folder).
    // Delete: permanently, no undo.
    // Extract: each source archive into targetDir (needs libarchive).
    // Compress: all sources into one archive targetDir/targetName.
    enum class Kind { Copy, Move, Trash, Restore, Delete, Extract, Compress };
    enum class ArchiveFormat { Zip, TarGz, TarXz, SevenZip };
    static bool archivesSupported();
    static QString archiveSuffix(ArchiveFormat format); // ".zip", ".tar.xz"…
    // What's inside an archive, without extracting (Quick Look). At most
    // `max` entries; `total` gets the full count. False if it can't be read.
    struct ArchiveEntry
    {
        QString path;
        qint64 size = 0;
        bool isDir = false;
    };
    static bool listArchive(const QString &archive, int max, QList<ArchiveEntry> *entries, int *total,
                            QString *error = nullptr);

    struct Progress
    {
        qint64 bytesDone = 0;
        qint64 bytesTotal = 0;
        int itemsDone = 0;
        int itemsTotal = 0;
        bool measuring = true;
        bool finishing = false; // waiting for a removable drive to finish writing
        QString current;        // name of the item being processed
    };

    // What to do when the destination already has an item with that name.
    enum class ConflictAction {
        Replace,  // the existing item goes to the Trash, then the transfer happens
        Skip,     // leave both as they are
        KeepBoth, // transfer under another name ("name (copy)" or a chosen one)
        Merge,    // folder onto folder: transfer the contents, asking again inside
        Cancel,   // stop the whole job
    };
    struct Resolution
    {
        ConflictAction action = ConflictAction::KeepBoth;
        bool applyToAll = false; // remember for the rest of this job (files and
                                 // folders separately)
        QString newName;         // KeepBoth with a chosen name
    };

    FileJob(Kind kind, QList<Transfer> transfers, QObject *parent = nullptr);

    // Ask (via conflict()) when the destination name is taken. Off: always
    // KeepBoth without asking (used by tests and non-interactive callers).
    void setInteractive(bool interactive) { m_interactive = interactive; }
    void setArchiveFormat(ArchiveFormat format) { m_archiveFormat = format; }
    // Answer the question conflict() asked (GUI thread).
    void resolveConflict(const Resolution &resolution);

    Kind kind() const { return m_kind; }
    const QList<Transfer> &transfers() const { return m_transfers; }
    void start(QThreadPool *pool);
    void cancel() { m_cancel.storeRelaxed(1); }
    bool isCanceled() const { return m_cancel.loadRelaxed() != 0; }
    qint64 elapsedMs() const { return m_elapsed.elapsed(); }

    Progress progress() const;
    QList<TransferResult> results() const; // complete after finished()
    QStringList errors() const;
    // Names of removable drives this job wrote to (and fully flushed).
    QStringList flushedDrives() const;

signals:
    void progressChanged();
    void finished();
    // The worker is paused until resolveConflict() (or cancel()) is called.
    void conflict(const QString &source, const QString &existing);

private:
    // Worker thread only.
    void run();
    void runTrash();
    void runDelete();
    void runExtract();  // FileJobArchive.cpp
    void runCompress(); // FileJobArchive.cpp
    void runTransfers();
    bool transferItem(const QString &source, const QString &dir, const QString &name,
                      qint64 bytes, bool topLevel);
    bool mergeInto(const QString &sourceDir, const QString &targetDir);
    bool moveOrCopy(const QString &source, const QString &target, qint64 bytes,
                    const QString &dir);
    Resolution askUser(const QString &source, const QString &existing);
    bool copyTree(const QString &source, const QString &target);
    bool copyFile(const QString &source, const QString &target, const QFileInfo &info);
    void flushDrive(const QString &dir);
    qint64 treeSize(const QString &path);
    void addBytes(qint64 bytes);
    void setCurrent(const QString &name);
    void itemDone();
    void addResult(const QString &source, const QString &target);
    void addError(const QString &message);
    void notify(bool force);

    const Kind m_kind;
    const QList<Transfer> m_transfers;
    QAtomicInt m_cancel;
    QElapsedTimer m_elapsed;     // started in start(), read from the GUI thread
    QElapsedTimer m_notifyTimer; // worker thread only
    bool m_safeWrites = false;   // worker thread only: current target is removable
    bool m_interactive = false;
    ArchiveFormat m_archiveFormat = ArchiveFormat::Zip;
    std::optional<ConflictAction> m_fileChoice;   // "apply to all", worker thread only
    std::optional<ConflictAction> m_folderChoice;
    QMutex m_askMutex;
    QWaitCondition m_answered;
    std::optional<Resolution> m_answer; // guarded by m_askMutex

    mutable QMutex m_mutex; // guards everything below
    Progress m_progress;
    QList<TransferResult> m_results;
    QStringList m_errors;
    QStringList m_flushedDrives;
};

// True if `dir` is on a removable drive (USB stick, SD card, external disk),
// where copies are flushed to the device before they count as done.
// KOSMOS_SAFE_WRITES=1/0 forces the answer (for tests).
bool needsSafeWrites(const QString &dir);
// True if `path` is on a network filesystem (NFS, SMB, sshfs, gvfs…).
bool isNetworkFilesystem(const QString &path);

// Path in `dir` for `name` that doesn't exist yet: "name", "name (copy)",
// "name (copy 2)"… keeping the extension (".tar.gz" counts as one).
QString uniqueTargetPath(const QString &dir, const QString &name, bool isDir);
