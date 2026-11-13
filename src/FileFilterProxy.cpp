#include "FileFilterProxy.h"

FileFilterProxy::FileFilterProxy(QObject *parent)
    : QSortFilterProxyModel(parent)
{
    setDynamicSortFilter(true);
}

void FileFilterProxy::sort(int column, Qt::SortOrder order)
{
    if (sourceModel())
        sourceModel()->sort(column, order);
}
