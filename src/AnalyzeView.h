#pragma once

#include "FileJob.h"

#include <QList>
#include <QString>
#include <QTreeView>

class DiskUsageModel;

// The Analyze page (Ctrl+3): a tree of how much disk space a folder and
// everything inside it uses, biggest first, with a bar per row. It has its
// own model and selection, apart from the file views', so file commands can
// never act on something chosen here by accident: what happens to a row is
// asked for through signals and decided by the window.
class AnalyzeView : public QTreeView
{
    Q_OBJECT

public:
    explicit AnalyzeView(QWidget *parent = nullptr);

    // Shows `folder`. Measured only if it isn't inside what was measured
    // already, so going into a sub-folder and back up is instant.
    void setFolder(const QString &folder);
    bool isEmpty() const; // nothing is shown (the folder went away)
    void rescan();        // measure again: other programs' changes aren't watched

    // Also list each folder's biggest files. The open folders stay open.
    void setShowFiles(bool show);

    struct Row
    {
        QString path; // empty for no row, or the "N smaller files" row
        bool isFile = false;
    };
    Row selectedRow() const;
    Row rowAt(const QPoint &viewportPos); // also makes it the current row

    // For the floating bar: progress while measuring, then the selected
    // row's share or the total.
    QString statusText() const;

    // Keeps the sizes right after one of our own file jobs, without
    // measuring everything again: what went away leaves the tree, what
    // arrived is measured and added.
    void applyJob(FileJob::Kind kind, const QList<TransferResult> &results);

signals:
    void folderActivated(const QString &path); // Enter or double-click on a folder
    void fileActivated(const QString &path);
    void trashRequested(const QString &path); // Delete on the selected row
    void statusChanged();

private:
    void keepTopRowOpen();

    DiskUsageModel *m_model;
};
