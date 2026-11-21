#pragma once

#include "Theme.h"

#include <QCache>
#include <QHash>
#include <QPersistentModelIndex>
#include <QPixmap>
#include <QStringList>
#include <QStyledItemDelegate>

class Thumbnailer;

// Paints files in the main view.
//  Grid: big icon/thumbnail, name wrapped to 3 lines (middle-elided),
//        rounded hover/selection tile.
//  List: small icon, name, and size/date on the right.
//  Columns (the list laid out in columns): each item a row of its own cell,
//        small icon, name and its size at the right.
class FileItemDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    // Zoom levels (icon px), smallest first, and the default of each.
    static const QList<int> &gridSizes(); // 32 48 64 96 128 192 256
    static const QList<int> &listSizes(); // 16 24 32 48
    static constexpr int DefaultGridIconSize = 48;
    static constexpr int DefaultListIconSize = 24;

    FileItemDelegate(Thumbnailer *thumbnailer, QObject *parent = nullptr);

    void setGridMode(bool grid);
    // The list in columns: items in cells side by side, one row each.
    void setColumnsMode();
    bool isColumnsMode() const { return m_columns; }
    // Zoom. Views re-lay out themselves (sizeHintChanged), keeping the selection.
    void setIconSize(int px);
    int iconSize() const { return m_iconSize; }
    void setTheme(const Theme &theme);
    // An extension's mark colour ("warning", "success"…) in `theme`: libadwaita's shades.
    static QColor badgeColorFor(const Theme &theme, const QString &name);
    // Folder under a drag in progress: drawn highlighted ("drop here").
    void setDropTarget(const QModelIndex &index) { m_dropTarget = index; }
    QSize gridCellSize(const QFontMetrics &fm) const;
    // A cell of the list in `count` columns across `width` pixels.
    QSize columnCellSize(const QFontMetrics &fm, int width, int count) const;
    // Where the inline rename field goes for an item drawn in `itemRect`
    // (`index` gives the tree indentation in the list).
    QRect nameEditorRect(const QRect &itemRect, const QFontMetrics &fm,
                         const QModelIndex &index = QModelIndex()) const;
    // Expandable folders (list): how far an item is moved right, and the
    // ▸ arrow's place in its Name cell (empty if it has none).
    static constexpr int TreeStep = 20;
    int treeIndent(const QModelIndex &index) const;
    QRect expanderRect(const QRect &cellRect, const QModelIndex &index) const;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

private:
    void paintGrid(QPainter *painter, const QStyleOptionViewItem &option,
                   const QModelIndex &index) const;
    void paintList(QPainter *painter, const QStyleOptionViewItem &option,
                   const QModelIndex &index) const;
    void paintColumnsCell(QPainter *painter, const QStyleOptionViewItem &option,
                          const QModelIndex &index) const;
    // An extension's mark at the right end of `nameRect`, which loses that room.
    void paintMarkAtRight(QPainter *painter, QRect &nameRect, const QStyleOptionViewItem &option,
                          const QModelIndex &index) const;
    void paintIcon(QPainter *painter, const QRect &rect, const QModelIndex &index) const;
    void paintExpander(QPainter *painter, const QRect &rect, bool expanded) const;
    QPixmap renderThumbnail(const QPixmap &thumb, const QSize &box, qreal dpr) const;
    void paintContentBadge(QPainter *painter, const QRect &iconRect, const QModelIndex &index) const;
    void paintStarBadge(QPainter *painter, const QRect &iconRect, const QModelIndex &index) const;
    // Symlink arrow (bottom-left) and 🔒 for items you can't change (top-left).
    void paintAccessBadges(QPainter *painter, const QRect &iconRect, const QModelIndex &index) const;
    // Git: a coloured letter (M, A, U, C) or a dot for folders with changes.
    // List: at the right end of the name; grid: a tag on the icon.
    QColor badgeColor(const QString &name) const;
    void paintBadgeTag(QPainter *painter, const QRect &iconRect, const QModelIndex &index) const;
    QStringList wrappedName(const QString &name, const QStyleOptionViewItem &option,
                            int width) const;

    Thumbnailer *m_thumbnailer;
    Theme m_theme;
    bool m_grid = true;
    bool m_columns = false;
    int m_iconSize = DefaultGridIconSize;
    QPersistentModelIndex m_dropTarget;
    mutable QHash<QString, QStringList> m_wrapCache;
    // Thumbnails pre-rendered at their on-screen size (cost in KiB).
    mutable QCache<QString, QPixmap> m_thumbCache{32 * 1024};
};
