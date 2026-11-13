#include "FileItemDelegate.h"

#include "Thumbnailer.h"

#include <QAbstractItemView>
#include <QFileSystemModel>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QTextLayout>

namespace {
constexpr int GridCellWidth = 118;
constexpr int GridMaxLines = 3;
constexpr int GridPad = 6;
constexpr int ListInfoWidth = 230;
} // namespace

FileItemDelegate::FileItemDelegate(Thumbnailer *thumbnailer, QObject *parent)
    : QStyledItemDelegate(parent)
    , m_thumbnailer(thumbnailer)
{
}

void FileItemDelegate::setGridMode(bool grid)
{
    m_grid = grid;
}

void FileItemDelegate::setTheme(const Theme &theme)
{
    m_theme = theme;
}

QSize FileItemDelegate::gridCellSize(const QFontMetrics &fm) const
{
    return QSize(GridCellWidth,
                 GridPad + GridIconSize + GridPad + GridMaxLines * fm.lineSpacing() + 2 * GridPad);
}

QSize FileItemDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const
{
    if (m_grid)
        return gridCellSize(option.fontMetrics);
    int width = 400;
    if (auto *view = qobject_cast<const QAbstractItemView *>(option.widget))
        width = view->viewport()->width();
    return QSize(width, qMax(36, option.fontMetrics.height() + 14));
}

void FileItemDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                             const QModelIndex &index) const
{
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setRenderHint(QPainter::SmoothPixmapTransform);
    if (m_grid)
        paintGrid(painter, opt, index);
    else
        paintList(painter, opt, index);
    painter->restore();
}

void FileItemDelegate::paintGrid(QPainter *painter, const QStyleOptionViewItem &option,
                                 const QModelIndex &index) const
{
    const QRect cell = option.rect.adjusted(3, 2, -3, -2);
    const QFontMetrics &fm = option.fontMetrics;
    const int textWidth = cell.width() - 2 * GridPad;
    const QStringList lines = wrappedName(option.text, option, textWidth);

    const QRect iconRect(cell.center().x() - GridIconSize / 2 + 1, cell.top() + GridPad,
                         GridIconSize, GridIconSize);
    const int textTop = iconRect.bottom() + 1 + GridPad;
    const int tileBottom = textTop + int(lines.size()) * fm.lineSpacing() + GridPad;

    const bool selected = option.state & QStyle::State_Selected;
    const bool hovered = option.state & QStyle::State_MouseOver;
    if (selected || hovered) {
        QPainterPath tile;
        tile.addRoundedRect(QRectF(cell.left(), cell.top(), cell.width(), tileBottom - cell.top()),
                            8, 8);
        painter->fillPath(tile, selected ? m_theme.itemSelected : m_theme.hover);
    }

    paintIcon(painter, iconRect, index);

    painter->setPen(m_theme.fg);
    painter->setFont(option.font);
    int y = textTop;
    for (const QString &line : lines) {
        painter->drawText(QRect(cell.left() + GridPad, y, textWidth, fm.lineSpacing()),
                          Qt::AlignHCenter | Qt::AlignTop, line);
        y += fm.lineSpacing();
    }
}

void FileItemDelegate::paintList(QPainter *painter, const QStyleOptionViewItem &option,
                                 const QModelIndex &index) const
{
    const QRect row = option.rect.adjusted(6, 1, -6, -1);
    const bool selected = option.state & QStyle::State_Selected;
    const bool hovered = option.state & QStyle::State_MouseOver;
    if (selected || hovered) {
        QPainterPath bg;
        bg.addRoundedRect(QRectF(row), 6, 6);
        painter->fillPath(bg, selected ? m_theme.itemSelected : m_theme.hover);
    }

    const QRect iconRect(row.left() + 10, row.center().y() - ListIconSize / 2 + 1, ListIconSize,
                         ListIconSize);
    paintIcon(painter, iconRect, index);

    // Secondary info: size (files) and modification date.
    QString info;
    if (auto *model = qobject_cast<const QFileSystemModel *>(index.model())) {
        const QFileInfo fi = model->fileInfo(index);
        const QLocale locale;
        const QString date = locale.toString(fi.lastModified(), QLocale::ShortFormat);
        info = fi.isDir() ? date : locale.formattedDataSize(fi.size()) + "    " + date;
    }

    const QFontMetrics &fm = option.fontMetrics;
    const int nameLeft = iconRect.right() + 12;
    const int infoLeft = row.right() - 12 - ListInfoWidth;
    const QRect nameRect(nameLeft, row.top(), qMax(0, infoLeft - nameLeft - 12), row.height());
    const QRect infoRect(infoLeft, row.top(), ListInfoWidth, row.height());

    painter->setFont(option.font);
    painter->setPen(m_theme.fg);
    painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                      fm.elidedText(option.text, Qt::ElideMiddle, nameRect.width()));
    painter->setPen(m_theme.dimFg);
    painter->drawText(infoRect, Qt::AlignRight | Qt::AlignVCenter,
                      fm.elidedText(info, Qt::ElideLeft, infoRect.width()));
}

// Thumbnail when available, otherwise the file-type icon.
void FileItemDelegate::paintIcon(QPainter *painter, const QRect &rect,
                                 const QModelIndex &index) const
{
    const qreal dpr = painter->device()->devicePixelRatioF();

    if (m_thumbnailer) {
        if (auto *model = qobject_cast<const QFileSystemModel *>(index.model())) {
            const QPixmap thumb = m_thumbnailer->thumbnail(model->fileInfo(index));
            if (!thumb.isNull()) {
                QSizeF size = QSizeF(thumb.size()).scaled(QSizeF(rect.size()), Qt::KeepAspectRatio);
                const QRectF target(rect.center().x() - size.width() / 2 + 0.5,
                                    rect.center().y() - size.height() / 2 + 0.5, size.width(),
                                    size.height());
                QPainterPath clip;
                clip.addRoundedRect(target, 3, 3);
                painter->save();
                painter->setClipPath(clip);
                painter->drawPixmap(target, thumb, QRectF(thumb.rect()));
                painter->restore();
                QColor frame = m_theme.fg;
                frame.setAlpha(30);
                painter->setPen(QPen(frame, 1));
                painter->setBrush(Qt::NoBrush);
                painter->drawRoundedRect(target.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
                return;
            }
        }
    }

    const QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
    const QPixmap pm = icon.pixmap(rect.size(), dpr);
    if (pm.isNull())
        return;
    const QSizeF logical = QSizeF(pm.size()) / pm.devicePixelRatio();
    const QPointF topLeft(rect.center().x() - logical.width() / 2 + 0.5,
                          rect.center().y() - logical.height() / 2 + 0.5);
    painter->drawPixmap(QRectF(topLeft, logical), pm, QRectF(pm.rect()));
}

// Word-wrap `name` into at most GridMaxLines lines; if it is longer, the
// last line holds the remainder elided in the middle (keeps the extension).
QStringList FileItemDelegate::wrappedName(const QString &name, const QStyleOptionViewItem &option,
                                          int width) const
{
    if (auto it = m_wrapCache.constFind(name); it != m_wrapCache.constEnd())
        return *it;
    if (m_wrapCache.size() > 20000)
        m_wrapCache.clear();

    QStringList lines;
    QTextLayout layout(name, option.font);
    QTextOption textOption;
    textOption.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(textOption);
    layout.beginLayout();
    while (true) {
        QTextLine line = layout.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(width);
        if (lines.size() == GridMaxLines - 1) {
            const QString rest = name.mid(line.textStart());
            lines.append(option.fontMetrics.elidedText(rest, Qt::ElideMiddle, width));
            break;
        }
        lines.append(name.mid(line.textStart(), line.textLength()).trimmed());
    }
    layout.endLayout();

    m_wrapCache.insert(name, lines);
    return lines;
}
