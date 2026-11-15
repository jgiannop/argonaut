#include "Screen.h"

#include <QChar>

namespace Tui {

namespace {

// Cells one character takes. Enough for file names: marks that join the
// letter before them take none; East Asian wide characters and emoji two.
int cellsOf(char32_t c)
{
    if (c < 0x20 || (c >= 0x7f && c < 0xa0))
        return 1; // shown as a placeholder
    const QChar::Category category = QChar::category(c);
    if (category == QChar::Mark_NonSpacing || category == QChar::Mark_Enclosing || c == 0x200b || c == 0x200d
        || (c >= 0xfe00 && c <= 0xfe0f))
        return 0;
    if ((c >= 0x1100 && c <= 0x115f) || (c >= 0x2e80 && c <= 0xa4cf) || (c >= 0xac00 && c <= 0xd7a3)
        || (c >= 0xf900 && c <= 0xfaff) || (c >= 0xfe30 && c <= 0xfe6f) || (c >= 0xff00 && c <= 0xff60)
        || (c >= 0xffe0 && c <= 0xffe6) || (c >= 0x1f300 && c <= 0x1faff) || (c >= 0x20000 && c <= 0x3fffd))
        return 2;
    return 1;
}

} // namespace

void Screen::resize(int width, int height)
{
    m_width = qMax(0, width);
    m_height = qMax(0, height);
    m_cells.resize(m_width * m_height);
    clear();
}

void Screen::clear()
{
    m_cells.fill(Cell());
    m_cursorX = m_cursorY = -1;
}

int Screen::put(int x, int y, const QString &text, const Style &style, int maxWidth)
{
    const int limit = maxWidth < 0 ? m_width - x : qMin(maxWidth, m_width - x);
    int used = 0;
    const QList<uint> points = text.toUcs4();
    for (uint point : points) {
        char32_t c = point;
        if (c < 0x20 || (c >= 0x7f && c < 0xa0))
            c = U'?'; // a control character in a name must not reach the terminal
        const int w = cellsOf(c);
        if (w == 0) {
            continue; // joining marks are left out: one cell, one character
        }
        if (used + w > limit)
            break;
        if (y >= 0 && y < m_height && x + used >= 0) {
            Cell &cell = m_cells[y * m_width + x + used];
            cell.ch = c;
            cell.style = style;
            cell.tail = false;
            if (w == 2) {
                Cell &second = m_cells[y * m_width + x + used + 1];
                second.ch = U' ';
                second.style = style;
                second.tail = true;
            }
        }
        used += w;
    }
    return used;
}

void Screen::fill(int x, int y, int width, int height, const Style &style, char32_t ch)
{
    for (int row = qMax(0, y); row < qMin(m_height, y + height); ++row) {
        for (int column = qMax(0, x); column < qMin(m_width, x + width); ++column) {
            Cell &cell = m_cells[row * m_width + column];
            cell.ch = ch;
            cell.style = style;
            cell.tail = false;
        }
    }
}

QString Screen::line(int y) const
{
    QString out;
    if (y < 0 || y >= m_height)
        return out;
    for (int x = 0; x < m_width; ++x) {
        const Cell &cell = m_cells.at(y * m_width + x);
        if (!cell.tail)
            out += QString::fromUcs4(&cell.ch, 1);
    }
    while (out.endsWith(QLatin1Char(' ')))
        out.chop(1);
    return out;
}

QString Screen::text() const
{
    QStringList rows;
    for (int y = 0; y < m_height; ++y)
        rows << line(y);
    return rows.join(QLatin1Char('\n'));
}

int Screen::widthOf(const QString &text)
{
    int width = 0;
    const QList<uint> points = text.toUcs4();
    for (uint point : points)
        width += cellsOf(point);
    return width;
}

QString Screen::elided(const QString &text, int width, bool middle)
{
    if (width <= 0)
        return {};
    if (widthOf(text) <= width)
        return text;
    if (width == 1)
        return QStringLiteral("…");
    const QList<uint> points = text.toUcs4();
    const int room = width - 1; // one cell for "…"
    const int headRoom = middle ? (room + 1) / 2 : room;
    const int tailRoom = room - headRoom;
    QList<uint> head, tail;
    int used = 0;
    for (uint point : points) {
        const int w = cellsOf(point);
        if (used + w > headRoom)
            break;
        head << point;
        used += w;
    }
    used = 0;
    for (auto it = points.crbegin(); it != points.crend() && tailRoom > 0; ++it) {
        const int w = cellsOf(*it);
        if (used + w > tailRoom)
            break;
        tail.prepend(*it);
        used += w;
    }
    return QString::fromUcs4(reinterpret_cast<const char32_t *>(head.constData()), head.size()) + QStringLiteral("…")
           + QString::fromUcs4(reinterpret_cast<const char32_t *>(tail.constData()), tail.size());
}

} // namespace Tui
