#pragma once

#include <QAtomicInt>
#include <QElapsedTimer>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QStringList>

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
    enum class Kind { Copy, Move, Trash, Restore, Delete };

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

    FileJob(Kind kind, QList<Transfer> transfers, QObject *parent = nullptr);

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

private:
    // Worker thread only.
    void run();
    void runTrash();
    void runDelete();
    void runTransfers();
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

    mutable QMutex m_mutex; // guards everything below
    Progress m_progress;
    QList<TransferResult> m_results;
    QStringList m_errors;
    QStringList m_flushedDrives;
};

// True if `dir` is on a removable drive (USB stick, SD card, external disk),
// where copies are flushed to the device before they count as done.
// KAFTILOUS_SAFE_WRITES=1/0 forces the answer (for tests).
bool needsSafeWrites(const QString &dir);

// Path in `dir` for `name` that doesn't exist yet: "name", "name (copy)",
// "name (copy 2)"… keeping the extension (".tar.gz" counts as one).
QString uniqueTargetPath(const QString &dir, const QString &name, bool isDir);
