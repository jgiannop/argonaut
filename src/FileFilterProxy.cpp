#include "FileFilterProxy.h"

FileFilterProxy::FileFilterProxy(QObject *parent)
    : QSortFilterProxyModel(parent)
{
    setDynamicSortFilter(true);
}

void FileFilterProxy::setQuery(const QString &query)
{
    if (m_query == query)
        return;
    m_query = query;
    invalidateFilter();
}

void FileFilterProxy::sort(int column, Qt::SortOrder order)
{
    if (sourceModel())
        sourceModel()->sort(column, order);
}

bool FileFilterProxy::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    if (m_query.isEmpty() || sourceParent.isValid())
        return true;
    const QString name = sourceModel()->index(sourceRow, 0, sourceParent).data().toString();
    return name.contains(m_query, Qt::CaseInsensitive);
}
