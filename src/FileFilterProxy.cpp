#include "FileFilterProxy.h"

#include <QFileSystemModel>

FileFilterProxy::FileFilterProxy(QObject *parent)
    : QSortFilterProxyModel(parent)
{
    // No sorting here: keep QFileSystemModel's own order.
    setDynamicSortFilter(true);
}

void FileFilterProxy::setFolder(const QModelIndex &sourceFolder)
{
    if (m_folder == sourceFolder)
        return;
    m_folder = sourceFolder;
    if (!m_query.isEmpty())
        invalidateFilter();
}

void FileFilterProxy::setQuery(const QString &query)
{
    if (m_query == query)
        return;
    m_query = query;
    invalidateFilter();
}

bool FileFilterProxy::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    if (m_query.isEmpty() || sourceParent != m_folder)
        return true;
    const QString name = sourceModel()
                             ->index(sourceRow, 0, sourceParent)
                             .data(QFileSystemModel::FileNameRole)
                             .toString();
    return name.contains(m_query, Qt::CaseInsensitive);
}
