#pragma once

#include "Extensions.h"
#include "Theme.h"

#include <QSet>
#include <QWidget>

class QFileSystemModel;
class QLabel;
class QModelIndex;
class QToolButton;
class QTreeView;
class SideTreeFilter;

// A folder as a tree, under the sidebar's places: the tree an extension
// asked for (Project Explorer: the open project). The extension says which
// folder and what in it is faint or left out (Extensions::Tree); the
// listing is Qt's (QFileSystemModel, which reads a folder when it is opened
// and watches what it has read), and the extensions' marks (Git's M, A, U)
// are drawn at the right of each row.
//
// It never takes the keys (Qt::NoFocus, like the preview pane): a click
// says what was clicked and the window shows it in the files.
class SideTree : public QWidget
{
    Q_OBJECT

public:
    explicit SideTree(QWidget *parent = nullptr);
    ~SideTree() override;

    // The tree to show (another folder starts again; the same one keeps
    // what is opened and only takes the new faint and hidden paths).
    void setTree(const Extensions::Tree &tree);
    Extensions::Tree tree() const { return m_tree; }
    void applyTheme(const Theme &theme);
    // The files' pane is in `path` now: its row is marked, if it is in the
    // tree and nothing inside it is marked already (a file just clicked).
    void reveal(const QString &path);

    QTreeView *view() const { return m_view; }
    QString pathAt(const QModelIndex &viewIndex) const;
    QModelIndex indexOf(const QString &path) const; // in the view; invalid until it is listed
    bool isFaint(const QModelIndex &viewIndex) const;

signals:
    void folderClicked(const QString &path);
    void fileClicked(const QString &path);
    void fileActivated(const QString &path); // a double click: open it
    void closeRequested();

private:
    void watch(const QString &folder);
    void unwatchAll();

    Extensions::Tree m_tree;
    QLabel *m_title = nullptr;
    QToolButton *m_close = nullptr;
    QTreeView *m_view = nullptr;
    QFileSystemModel *m_files = nullptr;
    SideTreeFilter *m_filter = nullptr;
    QSet<QString> m_watched; // folders the extensions were told about (for their marks)
    Theme m_theme;
};
