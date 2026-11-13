#pragma once

#include <QPersistentModelIndex>
#include <QSortFilterProxyModel>

// Sits between QFileSystemModel and the view and implements "search in the
// current folder": rows directly inside `folder` are shown only if their
// name contains the query (case-insensitive). Everything else passes, so
// the folder's ancestors stay reachable.
//
// We filter here instead of with QFileSystemModel::setNameFilters because
// that model never hides folders the view holds a persistent index to (e.g.
// the current item), so a folder could show up in unrelated results.
class FileFilterProxy : public QSortFilterProxyModel
{
    Q_OBJECT

public:
    explicit FileFilterProxy(QObject *parent = nullptr);

    void setFolder(const QModelIndex &sourceFolder);
    void setQuery(const QString &query);
    QString query() const { return m_query; }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;

private:
    QPersistentModelIndex m_folder;
    QString m_query;
};
