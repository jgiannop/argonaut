#include "FileItemDelegate.h"

#include "Thumbnailer.h"

#include <QAbstractItemView>
#include <QAbstractProxyModel>
#include <QFileSystemModel>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QTextLayout>

#include <cmath>

namespace {
constexpr int GridCellWidth = 118;
constexpr int GridMaxLines = 3;
constexpr int GridPad = 6;
constexpr int ListInfoWidth = 230;
} // namespace

// The view may show the file model through proxies (search filter); find
// the QFileSystemModel and the matching index in it.
static const QFileSystemModel *fileModel(const QModelIndex &index, QModelIndex *source)
{
    QModelIndex i = index;
    while (auto *proxy = qobject_cast<const QAbstractProxyModel *>(i.model()))
        i = proxy->mapToSource(i);
    *source = i;
    return qobject_cast<const QFileSystemModel *>(i.model());
}

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
    m_thumbCache.clear(); // the frame colour is baked in
}

QSize FileItemDelegate::gridCellSize(const QFontMetrics &fm) const
{
    return QSize(GridCellWidth,
                 GridPad + GridIconSize + GridPad + GridMaxLines * fm.lineSpacing() + 2 * GridPad);
}

QRect FileItemDelegate::nameEditorRect(const QRect &itemRect, const QFontMetrics &fm) const
{
    const int height = fm.height() + 12;
    if (m_grid) {
        // Under the icon; a bit wider than the cell so names are editable.
        const QRect cell = itemRect.adjusted(3, 2, -3, -2);
        const int top = cell.top() + GridPad + GridIconSize + GridPad - 5;
        const int width = qMax(cell.width(), 190);
        return QRect(cell.center().x() - width / 2 + 1, top, width, height);
    }
    const QRect row = itemRect.adjusted(6, 1, -6, -1);
    const int left = row.left() + 10 + ListIconSize + 12 - 6;
    const int right = row.right() - 12 - ListInfoWidth - 6;
    return QRect(left, row.center().y() - height / 2 + 1, qMax(120, right - left), height);
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
    QModelIndex source;
    if (auto *model = fileModel(index, &source)) {
        const QFileInfo fi = model->fileInfo(source);
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

// Draw `pm` (already at its final size) centred in `rect`, snapped to whole
// device pixels so it's a straight copy: no filtering, no blur.
static void drawCentered(QPainter *painter, const QRect &rect, const QPixmap &pm)
{
    const qreal dpr = pm.devicePixelRatio();
    const QSizeF logical = QSizeF(pm.size()) / dpr;
    const QPointF topLeft(std::round((rect.center().x() + 0.5 - logical.width() / 2) * dpr) / dpr,
                          std::round((rect.center().y() + 0.5 - logical.height() / 2) * dpr) / dpr);
    painter->drawPixmap(topLeft, pm);
}

// Thumbnail scaled to fit `box`, with rounded corners and a hairline frame
// baked in. Done once per thumbnail and size, then reused for every repaint.
QPixmap FileItemDelegate::renderThumbnail(const QPixmap &thumb, const QSize &box, qreal dpr) const
{
    const QSizeF logical = QSizeF(thumb.size()).scaled(QSizeF(box), Qt::KeepAspectRatio);
    const QSize device(qMax(1, qRound(logical.width() * dpr)), qMax(1, qRound(logical.height() * dpr)));

    QPixmap out(device);
    out.setDevicePixelRatio(dpr);
    out.fill(Qt::transparent);

    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r(0, 0, device.width() / dpr, device.height() / dpr);
    QPainterPath clip;
    clip.addRoundedRect(r, 3, 3);
    p.setClipPath(clip);
    // Smooth, high-quality downscale once (much better than per-paint filtering).
    const QImage scaled = thumb.toImage().scaled(device, Qt::IgnoreAspectRatio,
                                                 Qt::SmoothTransformation);
    p.drawImage(r, scaled);
    p.setClipping(false);
    QColor frame = m_theme.fg;
    frame.setAlpha(30);
    p.setPen(QPen(frame, 1));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
    p.end();
    return out;
}

// Thumbnail when available, otherwise the file-type icon.
void FileItemDelegate::paintIcon(QPainter *painter, const QRect &rect,
                                 const QModelIndex &index) const
{
    const qreal dpr = painter->device()->devicePixelRatioF();

    if (m_thumbnailer) {
        QModelIndex source;
        if (auto *model = fileModel(index, &source)) {
            const QPixmap thumb = m_thumbnailer->thumbnail(model->fileInfo(source));
            if (!thumb.isNull()) {
                // cacheKey() changes whenever the thumbnail is replaced.
                const QString key = QStringLiteral("%1:%2x%3@%4")
                                        .arg(thumb.cacheKey())
                                        .arg(rect.width())
                                        .arg(rect.height())
                                        .arg(dpr);
                QPixmap *ready = m_thumbCache.object(key);
                if (!ready) {
                    ready = new QPixmap(renderThumbnail(thumb, rect.size(), dpr));
                    const int costKiB = qMax(1, ready->width() * ready->height() * 4 / 1024);
                    m_thumbCache.insert(key, ready, costKiB);
                    ready = m_thumbCache.object(key);
                }
                if (ready) {
                    drawCentered(painter, rect, *ready);
                    return;
                }
            }
        }
    }

    const QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
    const QPixmap pm = icon.pixmap(rect.size(), dpr);
    if (!pm.isNull())
        drawCentered(painter, rect, pm);
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
