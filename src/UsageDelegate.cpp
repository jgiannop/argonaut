#include "UsageDelegate.h"

#include "DiskUsageModel.h"
#include "Theme.h"

#include <QApplication>
#include <QPainter>

void UsageDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                          const QModelIndex &index) const
{
    if (index.column() != DiskUsageModel::UsageColumn) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }
    // Background (hover/selection) from the style, without the text.
    QStyleOptionViewItem bg = option;
    initStyleOption(&bg, index);
    bg.text.clear();
    const QWidget *widget = option.widget;
    QStyle *style = widget ? widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &bg, painter, widget);

    const Theme theme = currentTheme();
    const double fraction = qBound(0.0, index.data(DiskUsageModel::FractionRole).toDouble(), 1.0);
    const QString percent = index.data().toString();
    const QFontMetrics fm(option.font);
    const int textWidth = fm.horizontalAdvance(QStringLiteral("100%")) + 8;
    const QRect area = option.rect.adjusted(8, 0, -8, 0);
    const int barHeight = 8;
    const QRectF track(area.left(), area.center().y() - barHeight / 2.0 + 1,
                       qMax(0, area.width() - textWidth), barHeight);

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(Qt::NoPen);
    QColor trackColor = theme.fg;
    trackColor.setAlphaF(theme.dark ? 0.12 : 0.08);
    painter->setBrush(trackColor);
    painter->drawRoundedRect(track, barHeight / 2.0, barHeight / 2.0);
    if (fraction > 0) {
        QRectF fill = track;
        fill.setWidth(qMax(barHeight * 1.0, track.width() * fraction));
        QColor accent = theme.accent;
        // Big consumers stand out; small ones stay quiet.
        if (fraction < 0.1)
            accent.setAlphaF(0.55);
        painter->setBrush(accent);
        painter->drawRoundedRect(fill, barHeight / 2.0, barHeight / 2.0);
    }
    painter->setPen(option.state & QStyle::State_Selected ? theme.fg : theme.dimFg);
    painter->drawText(QRect(int(track.right()) + 4, area.top(), textWidth, area.height()),
                      Qt::AlignRight | Qt::AlignVCenter, percent);
    painter->restore();
}

QSize UsageDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    QSize size = QStyledItemDelegate::sizeHint(option, index);
    size.setHeight(qMax(size.height(), 34));
    return size;
}
