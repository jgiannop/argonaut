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

private:
    struct UndoEntry
    {
        FileJob::Kind kind;
        QList<TransferResult> results;
    };

    FileJob *startJob(FileJob::Kind kind, QList<Transfer> transfers, bool recordUndo);

    QList<FileJob *> m_active;
    QList<UndoEntry> m_undo;
    QThreadPool m_pool;
};
