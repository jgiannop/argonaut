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
enum class Paper : quint8 {
    Normal, Bar, Cursor, CursorIdle, Field, Side,
    Hover,  // the row or menu item under the mouse
    Shadow, // under a menu or the palette
};
constexpr int InkCount = 8;
constexpr int PaperCount = 8;

// The colours the inks and papers stand for. Not themed (the default): the
// terminal's own text and background, and a few of its basic colours. Themed
// (one of Argonaut's themes): each is one of the terminal's 256 colours,
// the nearest to the theme's, and every cell is painted, background too.
//
// `exact` (with the effects on): the theme's very colours too, as 0xRRGGBB.
// A terminal that can show any colour gets those; one that can't, the
// nearest of its 256 as before.
struct Palette
{
    bool themed = false;
    bool exact = false;
    qint16 ink[InkCount] = {};
    qint16 paper[PaperCount] = {};
    quint32 inkRgb[InkCount] = {};
    quint32 paperRgb[PaperCount] = {};

    bool operator==(const Palette &o) const
    {
        if (themed != o.themed || exact != o.exact)
            return false;
        for (int i = 0; i < InkCount; ++i) {
            if (ink[i] != o.ink[i] || inkRgb[i] != o.inkRgb[i])
                return false;
        }
        for (int i = 0; i < PaperCount; ++i) {
            if (paper[i] != o.paper[i] || paperRgb[i] != o.paperRgb[i])
                return false;
        }
        return true;
    }
};

struct Style
{
    Ink ink = Ink::Normal;
    Paper paper = Paper::Normal;
    bool bold = false;
    // A cell of the shell panel carries the shell's own colours instead:
    // two of the terminal's 256 (-1: the terminal's default).
    bool raw = false;
    bool underline = false;
    bool reverse = false;
    qint16 fg = -1;
    qint16 bg = -1;

    bool operator==(const Style &o) const
    {
        return ink == o.ink && paper == o.paper && bold == o.bold && raw == o.raw && underline == o.underline
               && reverse == o.reverse && fg == o.fg && bg == o.bg;
    }
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
    // What is there stays, drawn fainter: the rest of the screen while a
    // menu is open over it (dim), and the cells its shadow falls on (shade).
    void dim(int x, int y, int width, int height);
    void shade(int x, int y, int width, int height);
    // One cell as it is (the shell panel's); outside the grid it is dropped.
    void set(int x, int y, const Cell &cell)
    {
        if (x >= 0 && y >= 0 && x < m_width && y < m_height)
            m_cells[y * m_width + x] = cell;
    }

    const Cell &at(int x, int y) const { return m_cells.at(y * m_width + x); }
    // One row as text (what a test compares), without trailing spaces.
    QString line(int y) const;
    QString text() const; // all rows, one per line

    // Where the text cursor is shown (a prompt being typed); hidden when x < 0.
    // The frame's colours (the theme), for the terminal to show it in.
    void setPalette(const Palette &palette) { m_palette = palette; }
    const Palette &palette() const { return m_palette; }

    // The interface wants to hear where the mouse is as it moves (to light
    // up what is under it), not only its clicks.
    void setWantsMotion(bool on) { m_motion = on; }
    bool wantsMotion() const { return m_motion; }

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
    bool m_motion = false;
    Palette m_palette;
    QList<Cell> m_cells;
};

} // namespace Tui
