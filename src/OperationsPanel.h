#pragma once

#include "Theme.h"

#include <QFrame>
#include <QHash>

class FileJob;
class FileOperations;
class QLabel;
class QProgressBar;
class QToolButton;
class QVBoxLayout;

// Floating card at the bottom-left of the file view listing running file
// operations, each with a progress bar, size/time left and a cancel button.
// Jobs that finish within a moment never appear, so quick actions don't flash.
class OperationsPanel : public QFrame
{
    Q_OBJECT

public:
    OperationsPanel(FileOperations *operations, QWidget *parent);

    void setTheme(const Theme &theme);
    void reposition();

signals:
    void layoutChanged(); // shown, hidden or resized

private:
    struct Row
    {
        QWidget *widget = nullptr;
        QLabel *title = nullptr;
        QLabel *detail = nullptr;
        QProgressBar *bar = nullptr;
        QToolButton *cancel = nullptr;
    };

    void addJob(FileJob *job);
    void updateJob(FileJob *job);
    void removeJob(FileJob *job);
    void updateVisibility();

    QHash<FileJob *, Row> m_rows;
    QVBoxLayout *m_layout;
    Theme m_theme;
};
