#include "AnalyzeView.h"

#include "DiskUsageModel.h"
#include "Format.h"
#include "UsageDelegate.h"

#include <QAction>
#include <QHeaderView>
#include <QLocale>

#include <functional>

AnalyzeView::AnalyzeView(QWidget *parent) : QTreeView(parent), m_model(new DiskUsageModel(this))
{
    setObjectName("analyzeView");
    setModel(m_model);
    setItemDelegate(new UsageDelegate(this));
    setUniformRowHeights(true);
    setAllColumnsShowFocus(true);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setContextMenuPolicy(Qt::CustomContextMenu);
    setFrameShape(QFrame::NoFrame);
    setSortingEnabled(true);
    sortByColumn(DiskUsageModel::SizeColumn, Qt::DescendingOrder);
    QHeaderView *columns = header();
    columns->setStretchLastSection(false);
    columns->setHighlightSections(false);
    columns->setSectionsMovable(false);
    columns->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    columns->setSectionResizeMode(DiskUsageModel::NameColumn, QHeaderView::Stretch);
    columns->resizeSection(DiskUsageModel::UsageColumn, 200);
    columns->resizeSection(DiskUsageModel::SizeColumn, 110);
    columns->resizeSection(DiskUsageModel::ItemsColumn, 130);

    connect(selectionModel(), &QItemSelectionModel::currentChanged, this, &AnalyzeView::statusChanged);
    connect(m_model, &DiskUsageModel::progress, this, &AnalyzeView::statusChanged);
    connect(m_model, &DiskUsageModel::finished, this, &AnalyzeView::statusChanged);
    // The analysed folder is the top row: keep it open.
    connect(m_model, &QAbstractItemModel::modelReset, this, &AnalyzeView::keepTopRowOpen);
    connect(m_model, &QAbstractItemModel::rowsInserted, this, [this](const QModelIndex &under) {
        if (!under.parent().isValid())
            keepTopRowOpen();
    });

    connect(this, &QTreeView::activated, this, [this](const QModelIndex &index) {
        const QString path = m_model->filePath(index);
        if (path.isEmpty())
            return;
        if (index.data(DiskUsageModel::KindRole).toInt() == DiskUsageModel::Node::File)
            emit fileActivated(path);
        else
            emit folderActivated(path);
    });

    // Delete works on the row chosen here, and only while this view has the
    // keyboard: the window's own Delete is about the file views.
    auto *trash = new QAction(tr("Move to Trash"), this);
    trash->setShortcut(QKeySequence::Delete);
    trash->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(trash, &QAction::triggered, this, [this] {
        const Row row = selectedRow();
        if (!row.path.isEmpty())
            emit trashRequested(row.path);
    });
    addAction(trash);
}

void AnalyzeView::keepTopRowOpen()
{
    expand(m_model->index(0, 0));
}

void AnalyzeView::setFolder(const QString &folder)
{
    m_model->setRoot(folder);
    keepTopRowOpen();
}

bool AnalyzeView::isEmpty() const
{
    return m_model->rowCount() == 0;
}

void AnalyzeView::rescan()
{
    m_model->rescan();
}

// Toggling files rebuilds the tree's rows: note which folders are open and
// which row is selected, and put both back.
void AnalyzeView::setShowFiles(bool show)
{
    QStringList expanded;
    std::function<void(const QModelIndex &)> collect = [&](const QModelIndex &parent) {
        for (int r = 0; r < m_model->rowCount(parent); ++r) {
            const QModelIndex child = m_model->index(r, 0, parent);
            if (isExpanded(child)) {
                expanded << m_model->filePath(child);
                collect(child);
            }
        }
    };
    collect(QModelIndex());
    const QString current = selectedRow().path;
    m_model->setShowFiles(show);
    for (const QString &path : std::as_const(expanded))
        expand(m_model->indexForPath(path));
    if (!current.isEmpty())
        setCurrentIndex(m_model->indexForPath(current));
}

AnalyzeView::Row AnalyzeView::selectedRow() const
{
    const QModelIndex current = currentIndex();
    if (!current.isValid())
        return {};
    return {m_model->filePath(current), current.data(DiskUsageModel::KindRole).toInt() == DiskUsageModel::Node::File};
}

AnalyzeView::Row AnalyzeView::rowAt(const QPoint &viewportPos)
{
    const QModelIndex index = indexAt(viewportPos);
    if (!index.isValid())
        return {};
    setCurrentIndex(index);
    return selectedRow();
}

QString AnalyzeView::statusText() const
{
    if (m_model->isScanning()) {
        return tr("Measuring… %1 in %2 items")
            .arg(formatSize(m_model->scannedBytes()), QLocale().toString(m_model->scannedItems()));
    }
    const QModelIndex current = currentIndex();
    if (current.isValid() && current.parent().isValid()) {
        return tr("“%1”: %2, %3% of “%4”")
            .arg(current.data().toString(), formatSize(current.data(DiskUsageModel::BytesRole).toLongLong()))
            .arg(qRound(current.data(DiskUsageModel::FractionRole).toDouble() * 100))
            .arg(current.parent().data().toString());
    }
    if (m_model->rowCount() == 0)
        return {};
    QString text = tr("%1 on disk in %2 items")
                       .arg(formatSize(m_model->rootBytes()), QLocale().toString(m_model->rootItems()));
    if (m_model->rootIncomplete())
        text += tr(" (some folders couldn't be read)");
    return text;
}

void AnalyzeView::applyJob(FileJob::Kind kind, const QList<TransferResult> &results)
{
    for (const TransferResult &r : results) {
        switch (kind) {
        case FileJob::Kind::Trash: // it still exists (in the Trash): measure it there
            m_model->removePath(r.source, DiskUsageModel::sizeOnDisk(r.target));
            break;
        case FileJob::Kind::Delete:
            m_model->removePath(r.source);
            break;
        case FileJob::Kind::Move:
            m_model->removePath(r.source, DiskUsageModel::sizeOnDisk(r.target));
            m_model->addPath(r.target);
            break;
        case FileJob::Kind::Copy:
        case FileJob::Kind::Restore:
        case FileJob::Kind::Extract:
        case FileJob::Kind::Compress:
            m_model->addPath(r.target);
            break;
        }
    }
}
