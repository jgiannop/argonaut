#include "SideTree.h"

#include "FileItemDelegate.h"

#include <QDir>
#include <QFileSystemModel>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QSortFilterProxyModel>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

// What the extension leaves out is left out; what it says is faint is drawn
// faint. Paths are relative to the tree's folder.
class SideTreeFilter : public QSortFilterProxyModel
{
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

    Extensions::Tree tree;
    QColor faintColor;

    QString relativeOf(const QString &path) const
    {
        if (tree.folder.isEmpty() || !path.startsWith(tree.folder + QLatin1Char('/')))
            return {};
        return path.mid(tree.folder.size() + 1);
    }
    QString pathOf(const QModelIndex &index) const
    {
        return static_cast<QFileSystemModel *>(sourceModel())->filePath(mapToSource(index));
    }
    bool faint(const QModelIndex &index) const
    {
        const QString relative = relativeOf(pathOf(index));
        return !relative.isEmpty() && tree.isFaint(relative);
    }
    void changed() { invalidateFilter(); }

protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override
    {
        const auto *files = static_cast<QFileSystemModel *>(sourceModel());
        const QString relative = relativeOf(files->filePath(files->index(row, 0, parent)));
        return relative.isEmpty() || !tree.isHidden(relative); // the folders above the tree's are its way in
    }
    QVariant data(const QModelIndex &index, int role) const override
    {
        if (role == Qt::ForegroundRole && faintColor.isValid() && faint(index))
            return faintColor;
        if (role == Qt::ToolTipRole) {
            const QString path = pathOf(index);
            QString tip = QDir::toNativeSeparators(path);
            const Extensions::Badge badge = Extensions::Host::instance()->badge(tree.folder, path);
            if (badge.valid && !badge.tooltip.isEmpty())
                tip += QLatin1Char('\n') + badge.tooltip;
            if (faint(index))
                tip += QLatin1Char('\n') + QObject::tr("Ignored by the project (made by a build or a tool)");
            return tip;
        }
        return QSortFilterProxyModel::data(index, role);
    }
};

namespace {

// A row, then the extensions' mark at its right (Git's M, A, U), in its colour.
class MarkDelegate : public QStyledItemDelegate
{
public:
    MarkDelegate(SideTreeFilter *filter, const Theme *theme, QObject *parent)
        : QStyledItemDelegate(parent), m_filter(filter), m_theme(theme)
    {
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyledItemDelegate::paint(painter, option, index);
        const Extensions::Badge badge = Extensions::Host::instance()->badge(m_filter->tree.folder, m_filter->pathOf(index));
        if (!badge.valid)
            return;
        painter->save();
        const QRect r = option.rect.adjusted(0, 0, -6, 0);
        painter->setRenderHint(QPainter::Antialiasing);
        const QColor color = FileItemDelegate::badgeColorFor(*m_theme, badge.color);
        if (badge.text.isEmpty()) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(color);
            painter->drawEllipse(QPointF(r.right() - 3, r.center().y() + 0.5), 3, 3);
        } else {
            QFont font = option.font;
            font.setBold(true);
            painter->setFont(font);
            painter->setPen(color);
            painter->drawText(r, Qt::AlignRight | Qt::AlignVCenter, badge.text);
        }
        painter->restore();
    }

private:
    SideTreeFilter *m_filter;
    const Theme *m_theme;
};

} // namespace

SideTree::SideTree(QWidget *parent)
    : QWidget(parent)
{
    setObjectName("sideTree");
    setAttribute(Qt::WA_StyledBackground);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *heading = new QWidget(this);
    auto *row = new QHBoxLayout(heading);
    row->setContentsMargins(14, 6, 6, 2);
    m_title = new QLabel(heading);
    m_title->setObjectName("sideTreeTitle");
    QFont bold = m_title->font();
    bold.setBold(true);
    m_title->setFont(bold);
    m_close = new QToolButton(heading);
    m_close->setObjectName("sideTreeClose");
    m_close->setAutoRaise(true);
    m_close->setFocusPolicy(Qt::NoFocus);
    m_close->setToolTip(tr("Close the project"));
    connect(m_close, &QToolButton::clicked, this, &SideTree::closeRequested);
    row->addWidget(m_title, 1);
    row->addWidget(m_close);
    layout->addWidget(heading);

    m_files = new QFileSystemModel(this);
    m_files->setReadOnly(true);
    m_files->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    m_filter = new SideTreeFilter(this);
    m_filter->setSourceModel(m_files);

    m_view = new QTreeView(this);
    m_view->setObjectName("sideTreeView");
    m_view->setFocusPolicy(Qt::NoFocus); // the keys stay with the files
    m_view->setHeaderHidden(true);
    m_view->setUniformRowHeights(true);
    m_view->setIndentation(14);
    m_view->setTextElideMode(Qt::ElideMiddle);
    m_view->setModel(m_filter);
    m_view->setItemDelegate(new MarkDelegate(m_filter, &m_theme, m_view));
    for (int column = 1; column < m_files->columnCount(); ++column)
        m_view->setColumnHidden(column, true);
    m_view->header()->setStretchLastSection(true);
    layout->addWidget(m_view, 1);

    connect(m_view, &QTreeView::clicked, this, [this](const QModelIndex &index) {
        const QString path = pathAt(index);
        if (path.isEmpty())
            return;
        if (QFileInfo(path).isDir()) {
            m_view->expand(index);
            emit folderClicked(path);
        } else {
            emit fileClicked(path);
        }
    });
    connect(m_view, &QTreeView::doubleClicked, this, [this](const QModelIndex &index) {
        const QString path = pathAt(index);
        if (!path.isEmpty() && !QFileInfo(path).isDir())
            emit fileActivated(path);
    });
    // New marks for the folder: drawn again.
    connect(Extensions::Host::instance(), &Extensions::Host::folderChanged, this, [this](const QString &folder) {
        if (m_watched.contains(folder))
            m_view->viewport()->update();
    });
}

SideTree::~SideTree()
{
    unwatchAll();
}

void SideTree::setTree(const Extensions::Tree &tree)
{
    const bool another = tree.folder != m_tree.folder;
    m_tree = tree;
    m_filter->tree = tree;
    m_title->setText(tree.title);
    m_title->setToolTip(tree.tooltip);
    if (!another) {
        m_filter->changed();
        m_view->viewport()->update();
        return;
    }
    unwatchAll();
    if (tree.folder.isEmpty()) {
        m_view->setRootIndex(QModelIndex());
        return;
    }
    const QModelIndex root = m_files->setRootPath(tree.folder);
    m_files->sort(0); // folders first, then names in natural order
    m_filter->changed();
    m_view->setRootIndex(m_filter->mapFromSource(root));
    watch(tree.folder);
}

void SideTree::applyTheme(const Theme &theme)
{
    m_theme = theme;
    QColor faint = theme.dimFg;
    faint.setAlphaF(qMin(1.0, faint.alphaF()) * 0.7);
    m_filter->faintColor = faint;
    m_close->setIcon(glyphIcon(Glyph::Close, theme.dimFg));
    m_view->viewport()->update();
}

void SideTree::reveal(const QString &path)
{
    if (m_tree.folder.isEmpty() || (path != m_tree.folder && !path.startsWith(m_tree.folder + QLatin1Char('/'))))
        return;
    const QString marked = pathAt(m_view->currentIndex());
    if (marked == path || QFileInfo(marked).path() == path)
        return; // that row, or a file in it, is marked already
    const QModelIndex index = m_filter->mapFromSource(m_files->index(path));
    if (!index.isValid())
        return;
    m_view->setCurrentIndex(index);
    m_view->scrollTo(index);
}

QString SideTree::pathAt(const QModelIndex &viewIndex) const
{
    return viewIndex.isValid() ? m_filter->pathOf(viewIndex) : QString();
}

QModelIndex SideTree::indexOf(const QString &path) const
{
    return m_filter->mapFromSource(m_files->index(path));
}

bool SideTree::isFaint(const QModelIndex &viewIndex) const
{
    return m_filter->faint(viewIndex);
}

void SideTree::watch(const QString &folder)
{
    if (m_watched.contains(folder))
        return;
    m_watched.insert(folder);
    Extensions::Host::instance()->watch(folder);
}

void SideTree::unwatchAll()
{
    for (const QString &folder : std::as_const(m_watched))
        Extensions::Host::instance()->unwatch(folder);
    m_watched.clear();
}
