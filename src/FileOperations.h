#pragma once

#include "FileJob.h"

#include <QList>
#include <QObject>
#include <QStringList>
#include <QThreadPool>

// Starts file jobs, tracks the running ones and keeps the undo history.
// Also reads/writes file lists on the system clipboard in formats other
// file managers understand (GNOME, KDE, Windows Explorer).
class FileOperations : public QObject
{
    Q_OBJECT

public:
    // Dynamic property on each job: true for user actions, false for undos.
    static constexpr const char *UserActionProperty = "kaftilousUserAction";

    explicit FileOperations(QObject *parent = nullptr);
    ~FileOperations() override;

    FileJob *copy(const QStringList &paths, const QString &targetDir);
    FileJob *move(const QStringList &paths, const QString &targetDir);
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
    // created items. `label` names it in the Undo menu item.
    void recordUndo(FileJob::Kind kind, const QList<TransferResult> &results,
                    const QString &label = QString());

    // Ask about name clashes (conflictRaised) instead of always keeping both.
    void setAskOnConflict(bool ask) { m_askOnConflict = ask; }

    bool canUndo() const { return !m_undo.isEmpty(); }
    QString undoText() const;
    void undo();

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
        FileJob::Kind kind;
        QList<TransferResult> results;
        QString label; // custom Undo text, e.g. "Undo Rename"
    };

    FileJob *startJob(FileJob::Kind kind, QList<Transfer> transfers, bool recordUndo,
                      bool userAction);

    QList<FileJob *> m_active;
    QList<UndoEntry> m_undo;
    QThreadPool m_pool;
    bool m_askOnConflict = false;
    FileJob::ArchiveFormat m_pendingFormat = FileJob::ArchiveFormat::Zip;
};
