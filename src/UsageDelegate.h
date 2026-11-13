#pragma once

#include <QStyledItemDelegate>

// Analyze view: draws the Usage column as a bar (share of the parent folder)
// with the percentage, and gives every row the list view's height.
class UsageDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};
