#pragma once

#include <QSortFilterProxyModel>

// Sits between DirectoryModel and the views and implements "search in the
// current folder": rows are shown only if their name contains the query
// (case-insensitive). Sorting is forwarded to the source model, which keeps
// its own (folders-first, natural) order; the proxy never reorders.
class FileFilterProxy : public QSortFilterProxyModel
{
    Q_OBJECT

public:
    explicit FileFilterProxy(QObject *parent = nullptr);

    void setQuery(const QString &query);
    QString query() const { return m_query; }

    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;

private:
    QString m_query;
};
