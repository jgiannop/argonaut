#pragma once

#include <QList>
#include <QString>

// What the terminal interface draws on: a grid of character cells, each with
// a colour and weight. The interface (Tui::App) draws a whole frame into a
// Screen; the terminal (Tui::Terminal, ncurses) then shows it. Tests read
// the same grid as text, so what is on screen can be checked without a
// terminal.
namespace Tui {

enum class Ink : quint8 { Normal, Dim, Accent, Folder, Good, Warning, Bad, Selected };
enum class Paper : quint8 { Normal, Bar, Cursor, CursorIdle, Field };

struct Style
{
    Ink ink = Ink::Normal;
    Paper paper = Paper::Normal;
    bool bold = false;

    bool operator==(const Style &o) const { return ink == o.ink && paper == o.paper && bold == o.bold; }
};

struct Cell
{
    char32_t ch = U' ';
    Style style;
    bool tail = false; // the second half of a double-width character
};

class Screen
{
public:
    Screen() = default;
    Screen(int width, int height) { resize(width, height); }

    void resize(int width, int height);
    int width() const { return m_width; }
    int height() const { return m_height; }
    void clear();

    // Writes `text` from (x, y), at most `maxWidth` cells (-1: to the edge).
    // Returns the cells used. Anything outside the grid is dropped.
    int put(int x, int y, const QString &text, const Style &style = {}, int maxWidth = -1);
    void fill(int x, int y, int width, int height, const Style &style, char32_t ch = U' ');

    const Cell &at(int x, int y) const { return m_cells.at(y * m_width + x); }
    // One row as text (what a test compares), without trailing spaces.
    QString line(int y) const;
    QString text() const; // all rows, one per line

    // Where the text cursor is shown (a prompt being typed); hidden when x < 0.
    void setCursor(int x, int y) { m_cursorX = x; m_cursorY = y; }
    int cursorX() const { return m_cursorX; }
    int cursorY() const { return m_cursorY; }

    // How many cells a text takes (CJK and emoji take two, accents none).
    static int widthOf(const QString &text);
    // `text` cut to `width` cells with "…" in the middle (names) or at the end.
    static QString elided(const QString &text, int width, bool middle = true);

private:
    int m_width = 0;
    int m_height = 0;
    int m_cursorX = -1;
    int m_cursorY = -1;
    QList<Cell> m_cells;
};

} // namespace Tui
