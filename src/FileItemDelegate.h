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
    // Zoom. Views re-lay out themselves (sizeHintChanged), keeping the selection.
    void setIconSize(int px);
    int iconSize() const { return m_iconSize; }
    void setTheme(const Theme &theme);
    // Folder under a drag in progress: drawn highlighted ("drop here").
    void setDropTarget(const QModelIndex &index) { m_dropTarget = index; }
    QSize gridCellSize(const QFontMetrics &fm) const;
    // Where the inline rename field goes for an item drawn in `itemRect`.
    QRect nameEditorRect(const QRect &itemRect, const QFontMetrics &fm) const;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

private:
    void paintGrid(QPainter *painter, const QStyleOptionViewItem &option,
                   const QModelIndex &index) const;
    void paintList(QPainter *painter, const QStyleOptionViewItem &option,
                   const QModelIndex &index) const;
    void paintIcon(QPainter *painter, const QRect &rect, const QModelIndex &index) const;
    QPixmap renderThumbnail(const QPixmap &thumb, const QSize &box, qreal dpr) const;
    void paintContentBadge(QPainter *painter, const QRect &iconRect, const QModelIndex &index) const;
    void paintStarBadge(QPainter *painter, const QRect &iconRect, const QModelIndex &index) const;
    // Symlink arrow (bottom-left) and 🔒 for items you can't change (top-left).
    void paintAccessBadges(QPainter *painter, const QRect &iconRect, const QModelIndex &index) const;
    QStringList wrappedName(const QString &name, const QStyleOptionViewItem &option,
                            int width) const;

    Thumbnailer *m_thumbnailer;
    Theme m_theme;
    bool m_grid = true;
    int m_iconSize = DefaultGridIconSize;
    QPersistentModelIndex m_dropTarget;
    mutable QHash<QString, QStringList> m_wrapCache;
    // Thumbnails pre-rendered at their on-screen size (cost in KiB).
    mutable QCache<QString, QPixmap> m_thumbCache{32 * 1024};
};
