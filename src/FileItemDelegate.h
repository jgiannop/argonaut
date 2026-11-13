#pragma once

#include "Theme.h"

#include <QHash>
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
    static constexpr int GridIconSize = 48;
    static constexpr int ListIconSize = 24;

    FileItemDelegate(Thumbnailer *thumbnailer, QObject *parent = nullptr);

    void setGridMode(bool grid);
    void setTheme(const Theme &theme);
    QSize gridCellSize(const QFontMetrics &fm) const;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

private:
    void paintGrid(QPainter *painter, const QStyleOptionViewItem &option,
                   const QModelIndex &index) const;
    void paintList(QPainter *painter, const QStyleOptionViewItem &option,
                   const QModelIndex &index) const;
    void paintIcon(QPainter *painter, const QRect &rect, const QModelIndex &index) const;
    QStringList wrappedName(const QString &name, const QStyleOptionViewItem &option,
                            int width) const;

    Thumbnailer *m_thumbnailer;
    Theme m_theme;
    bool m_grid = true;
    mutable QHash<QString, QStringList> m_wrapCache;
};
