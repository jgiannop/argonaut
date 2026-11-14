#include "FileItemDelegate.h"

#include "DirectoryModel.h"
#include "Thumbnailer.h"

#include <QAbstractItemView>
#include <QPainter>
#include <QPainterPath>
#include <QTextLayout>

#include <algorithm>
#include <cmath>

namespace {
constexpr int GridCellWidth = 118;
constexpr int GridMaxLines = 3;
constexpr int GridPad = 6;
constexpr int ListInset = 6;   // gap between the row highlight and the view edge
constexpr int ListTextPad = 10;
} // namespace

FileItemDelegate::FileItemDelegate(Thumbnailer *thumbnailer, QObject *parent)
    : QStyledItemDelegate(parent)
    , m_thumbnailer(thumbnailer)
{
}

const QList<int> &FileItemDelegate::gridSizes()
{
    static const QList<int> sizes = {32, 48, 64, 96, 128, 192, 256};
    return sizes;
}

const QList<int> &FileItemDelegate::listSizes()
{
    static const QList<int> sizes = {16, 24, 32, 48};
    return sizes;
}

void FileItemDelegate::setGridMode(bool grid)
{
    m_grid = grid;
}

void FileItemDelegate::setIconSize(int px)
{
    if (px == m_iconSize)
        return;
    m_iconSize = px;
    m_wrapCache.clear();  // cells are wider or narrower now
    m_thumbCache.clear(); // rendered at the old size
    emit sizeHintChanged(QModelIndex()); // the views lay out again
}

void FileItemDelegate::setTheme(const Theme &theme)
{
    m_theme = theme;
    m_thumbCache.clear(); // the frame colour is baked in
}

QSize FileItemDelegate::gridCellSize(const QFontMetrics &fm) const
{
    // Wider cells for bigger icons, so names get room too.
    const int width = qMax(m_iconSize <= 32 ? 96 : GridCellWidth, m_iconSize + 70);
    return QSize(width, GridPad + m_iconSize + GridPad + GridMaxLines * fm.lineSpacing() + 2 * GridPad);
}

int FileItemDelegate::treeIndent(const QModelIndex &index) const
{
    if (m_grid || !index.isValid())
        return 0;
    const int depth = index.data(DirectoryModel::TreeDepthRole).toInt();
    return depth < 0 ? 0 : (depth + 1) * TreeStep; // room for the arrow, then one step per level
}

QRect FileItemDelegate::expanderRect(const QRect &cellRect, const QModelIndex &index) const
{
    if (m_grid || index.column() != 0 || !index.data(DirectoryModel::ExpandableRole).toBool())
        return {};
    const int depth = index.data(DirectoryModel::TreeDepthRole).toInt();
    const int left = cellRect.left() + ListInset + 4 + depth * TreeStep;
    return QRect(left, cellRect.center().y() - TreeStep / 2 + 1, TreeStep, TreeStep);
}

// ▸ closed, ▾ open.
void FileItemDelegate::paintExpander(QPainter *painter, const QRect &rect, bool expanded) const
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(QPen(m_theme.dimFg, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter->setBrush(Qt::NoBrush);
    const QPointF c = QRectF(rect).center();
    QPainterPath chevron;
    if (expanded) {
        chevron.moveTo(c + QPointF(-3.5, -1.8));
        chevron.lineTo(c + QPointF(0, 1.8));
        chevron.lineTo(c + QPointF(3.5, -1.8));
    } else {
        chevron.moveTo(c + QPointF(-1.8, -3.5));
        chevron.lineTo(c + QPointF(1.8, 0));
        chevron.lineTo(c + QPointF(-1.8, 3.5));
    }
    painter->drawPath(chevron);
    painter->restore();
}

QRect FileItemDelegate::nameEditorRect(const QRect &itemRect, const QFontMetrics &fm,
                                       const QModelIndex &index) const
{
    const int height = fm.height() + 12;
    if (m_grid) {
        // Under the icon; a bit wider than the cell so names are editable.
        const QRect cell = itemRect.adjusted(3, 2, -3, -2);
        const int top = cell.top() + GridPad + m_iconSize + GridPad - 5;
        const int width = qMax(cell.width(), 190);
        return QRect(cell.center().x() - width / 2 + 1, top, width, height);
    }
    // List: `itemRect` is the Name cell; the field covers the name part.
    const int left = itemRect.left() + ListInset + 10 + treeIndent(index) + m_iconSize + 12 - 6;
    return QRect(left, itemRect.center().y() - height / 2 + 1,
                 qMax(120, itemRect.right() - 4 - left), height);
}

QSize FileItemDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    if (m_grid)
        return gridCellSize(option.fontMetrics);
    const int height = std::max({36, option.fontMetrics.height() + 14, m_iconSize + 12});
    // Width matters only for "fit column to contents".
    const QString text = index.data().toString();
    int width = option.fontMetrics.horizontalAdvance(text) + 2 * ListTextPad;
    if (index.column() == 0)
        width += ListInset + 10 + treeIndent(index) + m_iconSize + 12;
    return QSize(width, height);
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

    const QRect iconRect(cell.center().x() - m_iconSize / 2 + 1, cell.top() + GridPad,
                         m_iconSize, m_iconSize);
    const int textTop = iconRect.bottom() + 1 + GridPad;
    const int tileBottom = textTop + int(lines.size()) * fm.lineSpacing() + GridPad;

    const bool dropHere = m_dropTarget.isValid() && m_dropTarget.row() == index.row();
    const bool selected = (option.state & QStyle::State_Selected) || dropHere;
    const bool hovered = option.state & QStyle::State_MouseOver;
    if (selected || hovered) {
        QPainterPath tile;
        tile.addRoundedRect(QRectF(cell.left(), cell.top(), cell.width(), tileBottom - cell.top()),
                            8, 8);
        painter->fillPath(tile, selected ? m_theme.itemSelected : m_theme.hover);
        if (dropHere) {
            painter->setPen(QPen(m_theme.accent, 2));
            painter->drawPath(tile);
        }
    }

    paintIcon(painter, iconRect, index);
    paintContentBadge(painter, iconRect, index);
    paintAccessBadges(painter, iconRect, index);
    paintGitTag(painter, iconRect, index);
    paintStarBadge(painter, iconRect, index);

    painter->setPen(m_theme.fg);
    painter->setFont(option.font);
    int y = textTop;
    for (const QString &line : lines) {
        painter->drawText(QRect(cell.left() + GridPad, y, textWidth, fm.lineSpacing()),
                          Qt::AlignHCenter | Qt::AlignTop, line);
        y += fm.lineSpacing();
    }
}

// List (table) mode: each column is painted separately, but the hover /
// selection highlight is one rounded bar across the whole row: inner cells
// extend it past their edges and clip, so only the row's ends are rounded.
void FileItemDelegate::paintList(QPainter *painter, const QStyleOptionViewItem &option,
                                 const QModelIndex &index) const
{
    const int column = index.column();
    const int lastColumn = index.model()->columnCount(index.parent()) - 1;
    const bool dropHere = m_dropTarget.isValid() && m_dropTarget.row() == index.row();
    const bool selected = (option.state & QStyle::State_Selected) || dropHere;
    const bool hovered = option.state & QStyle::State_MouseOver;
    if (selected || hovered) {
        QRectF bar = QRectF(option.rect).adjusted(0, 1, 0, -1);
        bar.setLeft(column == 0 ? bar.left() + ListInset : bar.left() - 12);
        bar.setRight(column == lastColumn ? bar.right() - ListInset : bar.right() + 12);
        QPainterPath path;
        path.addRoundedRect(bar, 6, 6);
        painter->save();
        painter->setClipRect(option.rect);
        painter->fillPath(path, selected ? m_theme.itemSelected : m_theme.hover);
        if (dropHere) {
            painter->setPen(QPen(m_theme.accent, 2));
            painter->drawPath(path);
        }
        painter->restore();
    }

    const QFontMetrics &fm = option.fontMetrics;
    painter->setFont(option.font);
    if (column == 0) {
        const QRect expander = expanderRect(option.rect, index);
        if (!expander.isEmpty())
            paintExpander(painter, expander, index.data(DirectoryModel::ExpandedRole).toBool());
        const QRect iconRect(option.rect.left() + ListInset + 10 + treeIndent(index),
                             option.rect.center().y() - m_iconSize / 2 + 1, m_iconSize,
                             m_iconSize);
        paintIcon(painter, iconRect, index);
        paintContentBadge(painter, iconRect, index);
        paintAccessBadges(painter, iconRect, index);
        paintStarBadge(painter, iconRect, index);
        const int left = iconRect.right() + 12;
        QRect nameRect(left, option.rect.top(), qMax(0, option.rect.right() - left - 4),
                       option.rect.height());
        const int git = index.data(DirectoryModel::GitStatusRole).toInt();
        if (git != 0) { // the git letter at the right end of the name cell
            const QString letter = gitLetter(git);
            QFont bold = option.font;
            bold.setBold(true);
            const int w = QFontMetrics(bold).horizontalAdvance(letter.isEmpty() ? QStringLiteral("M") : letter) + 10;
            const QRect tag(nameRect.right() - w + 1, nameRect.top(), w, nameRect.height());
            nameRect.setRight(tag.left() - 4);
            painter->save();
            if (letter.isEmpty()) { // a folder with changes inside: a dot
                painter->setRenderHint(QPainter::Antialiasing);
                painter->setPen(Qt::NoPen);
                painter->setBrush(gitColor(git));
                painter->drawEllipse(QRectF(tag.center().x() - 3, tag.center().y() - 2, 6, 6));
            } else {
                painter->setFont(bold);
                painter->setPen(gitColor(git));
                painter->drawText(tag, Qt::AlignCenter, letter);
            }
            painter->restore();
        }
        painter->setPen(m_theme.fg);
        painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                          fm.elidedText(option.text, Qt::ElideMiddle, nameRect.width()));
        return;
    }
    const QRect textRect = option.rect.adjusted(ListTextPad, 0, -ListTextPad, 0);
    painter->setPen(m_theme.dimFg);
    painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
                      fm.elidedText(option.text, Qt::ElideRight, textRect.width()));
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

    if (m_thumbnailer && !index.data(DirectoryModel::IsDirRole).toBool()) {
        {
            // The real type picks the thumbnailer (a PDF named .bin still gets
            // one); a program in disguise is never handed to one.
            ContentType::Result real;
            real.nameMime = index.data(DirectoryModel::NameMimeRole).toString();
            real.contentMime = index.data(DirectoryModel::ContentMimeRole).toString();
            real.verdict = ContentType::Verdict(index.data(DirectoryModel::VerdictRole).toInt());
            const QString mime = real.verdict == ContentType::Verdict::Dangerous
                                     ? QString()
                                     : ContentType::effectiveMime(real);
            const QPixmap thumb =
                m_thumbnailer->thumbnail(index.data(DirectoryModel::PathRole).toString(),
                                         index.data(DirectoryModel::ModifiedRole).toDateTime(), mime);
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

// "!" in a circle at the icon's corner when the content doesn't match the
// name: amber for a mismatch, red for a program in disguise.
// A small gold star on the icon's top-right corner for starred items.
void FileItemDelegate::paintStarBadge(QPainter *painter, const QRect &iconRect, const QModelIndex &index) const
{
    if (!index.data(DirectoryModel::StarredRole).toBool())
        return;
    const qreal d = qBound(11, iconRect.width() * 3 / 8, 20);
    const QPointF c(iconRect.right() - d / 2 + 3, iconRect.top() + d / 2 - 2);
    QPainterPath star;
    for (int i = 0; i < 10; ++i) {
        const qreal r = (i % 2 == 0 ? d / 2 : d / 4.6);
        const qreal a = -M_PI / 2 + i * M_PI / 5;
        const QPointF pt(c.x() + r * std::cos(a), c.y() + r * std::sin(a));
        if (i == 0)
            star.moveTo(pt);
        else
            star.lineTo(pt);
    }
    star.closeSubpath();
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(QPen(m_theme.window, 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)); // ring
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(star);
    painter->setPen(QPen(QColor("#c88800"), 0.8));
    painter->setBrush(QColor("#f6c915"));
    painter->drawPath(star);
    painter->restore();
}

QString FileItemDelegate::gitLetter(int state)
{
    switch (GitStatus::State(state)) {
    case GitStatus::Modified:
        return QStringLiteral("M");
    case GitStatus::Added:
        return QStringLiteral("A");
    case GitStatus::Untracked:
        return QStringLiteral("U");
    case GitStatus::Conflicted:
        return QStringLiteral("C");
    default:
        return {}; // Changed (a folder): a dot
    }
}

QColor FileItemDelegate::gitColor(int state) const
{
    // libadwaita's palette: brighter shades on dark themes.
    switch (GitStatus::State(state)) {
    case GitStatus::Added:
    case GitStatus::Untracked:
        return m_theme.dark ? QColor("#57e389") : QColor("#26a269");
    case GitStatus::Conflicted:
        return m_theme.dark ? QColor("#f66151") : QColor("#c01c28");
    default:
        return m_theme.dark ? QColor("#ffa348") : QColor("#c64600");
    }
}

// Grid: a small round tag at the icon's bottom-left (next to a link arrow).
void FileItemDelegate::paintGitTag(QPainter *painter, const QRect &iconRect, const QModelIndex &index) const
{
    const int git = index.data(DirectoryModel::GitStatusRole).toInt();
    if (git == 0)
        return;
    const qreal d = qBound(10, iconRect.width() * 3 / 8, 20);
    qreal left = iconRect.left() - 3;
    if (index.data(DirectoryModel::SymLinkRole).toBool())
        left += d + 1;
    const QString letter = gitLetter(git);
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    if (letter.isEmpty()) {
        const qreal dot = d * 0.55;
        painter->setPen(QPen(m_theme.window, 1.5));
        painter->setBrush(gitColor(git));
        painter->drawEllipse(QRectF(left + (d - dot) / 2, iconRect.bottom() - d + 3 + (d - dot) / 2, dot, dot));
    } else {
        const QRectF r(left, iconRect.bottom() - d + 3, d, d);
        painter->setPen(QPen(m_theme.window, 1.5));
        painter->setBrush(gitColor(git));
        painter->drawEllipse(r);
        QFont font = painter->font();
        font.setBold(true);
        font.setPixelSize(int(d * 0.62));
        painter->setFont(font);
        painter->setPen(Qt::white);
        painter->drawText(r, Qt::AlignCenter, letter);
    }
    painter->restore();
}

void FileItemDelegate::paintAccessBadges(QPainter *painter, const QRect &iconRect,
                                         const QModelIndex &index) const
{
    const bool link = index.data(DirectoryModel::SymLinkRole).toBool();
    const bool locked = index.data(DirectoryModel::LockedRole).toBool();
    if (!link && !locked)
        return;
    const qreal d = qBound(10, iconRect.width() * 3 / 8, 20);
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    // A dark disc on light themes, a light one on dark themes; the symbol
    // is cut out in the background colour, with a ring to part it from the icon.
    auto disc = [&](const QRectF &r) {
        painter->setPen(QPen(m_theme.window, 1.5));
        painter->setBrush(m_theme.fg);
        painter->drawEllipse(r);
    };
    if (link) {
        const QRectF r(iconRect.left() - 3, iconRect.bottom() - d + 3, d, d);
        disc(r);
        // A curved arrow pointing up-right.
        const qreal u = d / 10.0;
        const QPointF c = r.center();
        QPainterPath arrow;
        arrow.moveTo(c + QPointF(-2.6 * u, 2.6 * u));
        arrow.quadTo(c + QPointF(-2.6 * u, -1.6 * u), c + QPointF(1.8 * u, -1.6 * u));
        painter->setPen(QPen(m_theme.window, qMax(1.2, 1.3 * u), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->setBrush(Qt::NoBrush);
        painter->drawPath(arrow);
        QPainterPath head;
        head.moveTo(c + QPointF(0.4 * u, -3.4 * u));
        head.lineTo(c + QPointF(2.6 * u, -1.6 * u));
        head.lineTo(c + QPointF(0.4 * u, 0.2 * u));
        painter->drawPath(head);
    }
    if (locked) {
        const QRectF r(iconRect.left() - 3, iconRect.top() - 2, d, d);
        disc(r);
        const qreal u = d / 10.0;
        const QPointF c = r.center();
        // Body and shackle.
        const QRectF body(c.x() - 2.7 * u, c.y() - 0.6 * u, 5.4 * u, 3.8 * u);
        painter->setPen(Qt::NoPen);
        painter->setBrush(m_theme.window);
        painter->drawRoundedRect(body, 0.8 * u, 0.8 * u);
        QPainterPath shackle;
        shackle.moveTo(c.x() - 1.6 * u, body.top());
        shackle.lineTo(c.x() - 1.6 * u, c.y() - 1.8 * u);
        shackle.arcTo(QRectF(c.x() - 1.6 * u, c.y() - 3.4 * u, 3.2 * u, 3.2 * u), 180, -180);
        shackle.lineTo(c.x() + 1.6 * u, body.top());
        painter->setPen(QPen(m_theme.window, qMax(1.0, 1.1 * u)));
        painter->setBrush(Qt::NoBrush);
        painter->drawPath(shackle);
    }
    painter->restore();
}

void FileItemDelegate::paintContentBadge(QPainter *painter, const QRect &iconRect,
                                         const QModelIndex &index) const
{
    const auto verdict = ContentType::Verdict(index.data(DirectoryModel::VerdictRole).toInt());
    if (verdict != ContentType::Verdict::Mismatch && verdict != ContentType::Verdict::Dangerous)
        return;
    const int d = qBound(10, iconRect.width() * 3 / 8, 26); // not huge on big icons
    const QRectF badge(iconRect.right() - d + 3, iconRect.bottom() - d + 3, d, d);
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(QPen(m_theme.window, 1.5)); // ring separates it from the icon
    painter->setBrush(verdict == ContentType::Verdict::Dangerous ? QColor("#e01b24") : QColor("#e5a50a"));
    painter->drawEllipse(badge);
    QFont font = painter->font();
    font.setBold(true);
    font.setPixelSize(int(d * 0.75));
    painter->setFont(font);
    painter->setPen(Qt::white);
    painter->drawText(badge, Qt::AlignCenter, QStringLiteral("!"));
    painter->restore();
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
