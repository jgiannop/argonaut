#pragma once

#include "FileJob.h"

#include <QList>
#include <QObject>

#include <functional>
#include <QStringList>
#include <QThreadPool>

// Starts file jobs, tracks the running ones and keeps the undo history.
// Also reads/writes file lists on the system clipboard in formats other
// file managers understand (GNOME, KDE, Windows Explorer).
class FileOperations : public QObject
{
    Q_OBJECT

public:
    // Dynamic property on each job: true for user actions, false for undos
    // and redos.
    static constexpr const char *UserActionProperty = "argonautUserAction";
    // Dynamic property on undo/redo jobs: "undo" or "redo".
    static constexpr const char *HistoryProperty = "argonautHistory";

    explicit FileOperations(QObject *parent = nullptr);
    ~FileOperations() override;

    FileJob *copy(const QStringList &paths, const QString &targetDir);
    FileJob *move(const QStringList &paths, const QString &targetDir);
    // Moves (and renames) each item as given, as one undoable action named
    // `action` ("Organize" gives "Undo Organize").
    FileJob *moveItems(const QList<Transfer> &transfers, const QString &action);
    // Copies, each to its own folder and name (a plan's): undoable like any copy.
    FileJob *copyItems(const QList<Transfer> &transfers, const QString &action);
    FileJob *trash(const QStringList &paths);
    // Permanently, no undo (callers confirm first).
    FileJob *deletePermanently(const QStringList &paths);
    // Items in a trash "files" folder back to where they were deleted from.
    FileJob *restore(const QStringList &pathsInTrash);
    // Delete everything in every trash folder (callers confirm first).
    FileJob *emptyTrash();
    // Archives (if built with libarchive). Both are undoable: undo trashes
    // what was created.
    FileJob *extract(const QStringList &archives, const QString &targetDir);
    FileJob *compress(const QStringList &paths, const QString &targetDir, const QString &archiveName,
                      FileJob::ArchiveFormat format);
    static bool isArchive(const QString &mimeType);

    // Record an action done outside a job so it can be undone like one:
    // Move (e.g. a rename) moves back; Copy (e.g. "New Folder") trashes the
    // created items. `action` names it in the menus ("Rename" gives
    // "Undo Rename" / "Redo Rename").
    void recordUndo(FileJob::Kind kind, const QList<TransferResult> &results,
                    const QString &action = QString());

    // Ask about name clashes (conflictRaised) instead of always keeping both.
    void setAskOnConflict(bool ask) { m_askOnConflict = ask; }

    // Undo runs the inverse of the last action; redo runs the inverse of
    // that undo (restoring what it trashed, moving back what it moved…), so
    // the very same items come back. A new action clears what can be redone.
    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }
    QString undoText() const;
    QString redoText() const;
    void undo();
    void redo();

    const QList<FileJob *> &activeJobs() const { return m_active; }
    void cancelAll();

    static void setClipboardFiles(const QStringList &paths, bool cut);
    static bool clipboardFiles(QStringList *paths, bool *cut);
    static bool clipboardHasFiles();

signals:
    void jobStarted(FileJob *job);
    void jobFinished(FileJob *job); // the job is deleted right after
    void undoChanged();
    // `job` is paused: answer with job->resolveConflict().
    void conflictRaised(FileJob *job, const QString &source, const QString &existing);

private:
    struct UndoEntry
    {
        FileJob::Kind kind;            // the job whose effect this reverses
        QList<TransferResult> results; // what that job did
        FileJob::Kind action;          // the user's original action (for the menu text)
        QString name;                  // custom name, e.g. "Rename"
    };

    // `prepare` is called with the job before it starts: anything that
    // must hear its `finished` is connected there. (The job runs on another
    // thread and a quick one can be finished before startJob() returns; a
    // connection made after that would never be called.)
    FileJob *startJob(FileJob::Kind kind, QList<Transfer> transfers, bool recordUndo,
                      bool userAction, const QString &undoName = QString(),
                      const std::function<void(FileJob *)> &prepare = {});
    // Starts the job that reverses `entry`; when it finishes, its own
    // inverse goes on `onto` (undo -> redo list, redo -> undo list).
    void runInverse(const UndoEntry &entry, QList<UndoEntry> *onto, const char *history);
    QString historyText(const QList<UndoEntry> &list, bool undo) const;
    void pushUndo(const UndoEntry &entry);

    QList<FileJob *> m_active;
    QList<UndoEntry> m_undo;
    QList<UndoEntry> m_redo;
    QThreadPool m_pool;
    bool m_askOnConflict = false;
    FileJob::ArchiveFormat m_pendingFormat = FileJob::ArchiveFormat::Zip;
};
