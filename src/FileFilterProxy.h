#pragma once

#include <QSortFilterProxyModel>

// Sits between DirectoryModel and the views. Sorting is forwarded to the
// source model, which keeps its own (folders-first, natural) order; the
// proxy never reorders. (Search is done by DirectoryModel::setSearch, which
// also looks in sub-folders.)
class FileFilterProxy : public QSortFilterProxyModel
{
    Q_OBJECT

public:
    explicit FileFilterProxy(QObject *parent = nullptr);

    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;
};
