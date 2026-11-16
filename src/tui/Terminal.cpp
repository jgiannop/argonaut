#include "Terminal.h"

// ncurses defines macros (timeout, border, clear…) that collide with Qt, so
// it is included after everything else, and only here.
#include <clocale>
#include <cstdio>
#include <cwchar>

#define NCURSES_WIDECHAR 1
#include <curses.h>

namespace Tui {

namespace {

short colorOf(Ink ink)
{
    switch (ink) {
    case Ink::Normal:
        return -1; // the terminal's own text colour
    case Ink::Dim:
        return COLORS >= 16 ? 8 : -1; // bright black: grey
    case Ink::Accent:
        return COLOR_CYAN;
    case Ink::Folder:
        return COLOR_BLUE;
    case Ink::Good:
        return COLOR_GREEN;
    case Ink::Warning:
        return COLOR_YELLOW;
    case Ink::Bad:
        return COLOR_RED;
    case Ink::Selected:
        return COLOR_YELLOW;
    }
    return -1;
}

short colorOf(Paper paper)
{
    switch (paper) {
    case Paper::Normal:
        return -1; // the terminal's own background
    case Paper::Bar:
        return COLORS >= 256 ? 236 : COLOR_BLACK;
    case Paper::Cursor:
        return COLORS >= 256 ? 24 : COLOR_BLUE;
    case Paper::CursorIdle:
        return COLORS >= 256 ? 238 : COLOR_BLACK;
    case Paper::Field:
        return COLORS >= 256 ? 238 : COLOR_BLACK;
    }
    return -1;
}

} // namespace

Terminal::Terminal()
{
    std::setlocale(LC_ALL, ""); // the user's: names in any script
    ::initscr();
    ::raw();     // Ctrl+C, Ctrl+Z and Ctrl+Q are keys for us, not signals
    ::noecho();
    ::nonl();
    ::keypad(stdscr, TRUE);
    ::nodelay(stdscr, TRUE);
    ::set_escdelay(25); // Esc on its own is felt at once
    // Arrows, Home and End with Shift, Alt or Ctrl, as xterm-like terminals
    // send them ("ESC [ 1 ; 5 A" is Ctrl+Up). ncurses knows most of them
    // under codes of its own; the rest are given one here.
    {
        static const struct { char letter; Key::Code code; } arrows[] = {{'A', Key::Up},   {'B', Key::Down}, {'C', Key::Right},
                                                                         {'D', Key::Left}, {'H', Key::Home}, {'F', Key::End}};
        int next = KEY_MAX + 1000;
        for (int modifier = 2; modifier <= 7; ++modifier) {
            for (const auto &arrow : arrows) {
                char sequence[8];
                std::snprintf(sequence, sizeof sequence, "\033[1;%d%c", modifier, arrow.letter);
                int code = ::key_defined(sequence);
                if (code <= 0) {
                    code = next++;
                    ::define_key(sequence, code);
                }
                Key key = Key::of(arrow.code);
                key.shift = (modifier - 1) & 1;
                key.alt = (modifier - 1) & 2;
                key.ctrl = (modifier - 1) & 4;
                m_modified.insert(code, key);
            }
        }
    }
    ::curs_set(0);
    // The mouse: presses and the wheel. Clicks are reported at once (no
    // waiting to see whether a second one follows: the interface tells a
    // double click itself). A press and its release that arrive together
    // come as one "clicked", so that is asked for too.
    ::mousemask(BUTTON1_PRESSED | BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED | BUTTON3_PRESSED | BUTTON3_CLICKED
                    | BUTTON4_PRESSED
#if defined(BUTTON5_PRESSED)
                    | BUTTON5_PRESSED
#endif
                ,
                nullptr);
    ::mouseinterval(0);
    if (::has_colors()) {
        ::start_color();
        ::use_default_colors();
        m_colors = true;
    }
}

Terminal::~Terminal()
{
    ::curs_set(1);
    ::endwin();
}

int Terminal::width() const
{
    return COLS;
}

int Terminal::height() const
{
    return LINES;
}

int Terminal::pairFor(Ink ink, Paper paper)
{
    if (!m_colors)
        return 0;
    int &pair = m_pairs[int(ink)][int(paper)];
    if (pair == 0 && m_nextPair < COLOR_PAIRS) {
        pair = m_nextPair++;
        short fg = colorOf(ink);
        const short bg = colorOf(paper);
        // Folder blue on the blue cursor bar would vanish.
        if (paper == Paper::Cursor && (ink == Ink::Folder || ink == Ink::Dim))
            fg = COLOR_WHITE;
        ::init_pair(short(pair), fg, bg);
    }
    return pair;
}

int Terminal::rawPair(int fg, int bg)
{
    if (!m_colors)
        return 0;
    if (fg >= COLORS)
        fg = -1;
    if (bg >= COLORS)
        bg = -1;
    if (fg < 0 && bg < 0)
        return 0;
    const int id = (fg + 1) * 300 + (bg + 1);
    auto found = m_rawPairs.constFind(id);
    if (found != m_rawPairs.constEnd())
        return *found;
    int pair = 0;
    if (m_nextPair < COLOR_PAIRS) {
        pair = m_nextPair++;
#if defined(NCURSES_EXT_COLORS) && NCURSES_EXT_COLORS
        ::init_extended_pair(pair, fg, bg);
#else
        ::init_pair(short(pair), short(fg), short(bg));
#endif
    }
    m_rawPairs.insert(id, pair);
    return pair;
}

void Terminal::show(const Screen &screen)
{
    ::erase();
    const int rows = qMin(screen.height(), LINES);
    const int columns = qMin(screen.width(), COLS);
    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < columns; ++x) {
            const Cell &cell = screen.at(x, y);
            if (cell.tail)
                continue; // the wide character before it covers this cell
            attr_t attributes = cell.style.bold ? A_BOLD : A_NORMAL;
            if (!m_colors && (cell.style.paper == Paper::Cursor || cell.style.paper == Paper::Bar))
                attributes |= A_REVERSE;
            int pair;
            if (cell.style.raw) {
                pair = rawPair(cell.style.fg, cell.style.bg);
                if (cell.style.underline)
                    attributes |= A_UNDERLINE;
                if (cell.style.reverse)
                    attributes |= A_REVERSE;
            } else {
                pair = pairFor(cell.style.ink, cell.style.paper);
            }
            const wchar_t text[2] = {wchar_t(cell.ch), L'\0'};
            cchar_t out;
#if defined(NCURSES_EXT_COLORS) && NCURSES_EXT_COLORS
            ::setcchar(&out, text, attributes, short(qMin(pair, 32767)), &pair); // more pairs than a short holds
#else
            ::setcchar(&out, text, attributes, short(pair), nullptr);
#endif
            // The bottom-right cell can't be written without scrolling: skip it.
            if (y == LINES - 1 && x == COLS - 1)
                continue;
            mvadd_wch(y, x, &out);
        }
    }
    if (screen.cursorX() >= 0 && screen.cursorY() >= 0) {
        ::curs_set(1);
        ::move(screen.cursorY(), qMin(screen.cursorX(), COLS - 1));
    } else {
        ::curs_set(0);
    }
    ::refresh();
}

void Terminal::suspend()
{
    ::def_prog_mode();
    ::endwin();
}

void Terminal::resume()
{
    ::reset_prog_mode();
    ::clearok(stdscr, TRUE); // everything is drawn again
    ::refresh();
}

QList<Mouse> Terminal::takeMouse()
{
    QList<Mouse> out;
    out.swap(m_mouse);
    return out;
}

QList<Key> Terminal::readKeys(bool *resized)
{
    QList<Key> keys;
    if (resized)
        *resized = false;
    bool escape = false; // Esc just before a key: Alt+key
    while (true) {
        wint_t ch = 0;
        const int kind = ::get_wch(&ch);
        if (kind == ERR)
            break;
        Key key;
        if (kind == KEY_CODE_YES) {
            switch (ch) {
            case KEY_RESIZE:
                if (resized)
                    *resized = true;
                continue;
            case KEY_MOUSE: {
                MEVENT event;
                if (::getmouse(&event) == OK) {
                    Mouse mouse;
                    mouse.x = event.x;
                    mouse.y = event.y;
                    if (event.bstate & (BUTTON1_PRESSED | BUTTON1_CLICKED)) {
                        mouse.kind = Mouse::Press;
                    } else if (event.bstate & BUTTON1_DOUBLE_CLICKED) {
                        mouse.kind = Mouse::Press;
                        m_mouse.append(mouse); // two clicks in one
                    } else if (event.bstate & (BUTTON3_PRESSED | BUTTON3_CLICKED))
                        mouse.kind = Mouse::RightPress;
                    else if (event.bstate & BUTTON4_PRESSED)
                        mouse.kind = Mouse::WheelUp;
#if defined(BUTTON5_PRESSED)
                    else if (event.bstate & BUTTON5_PRESSED)
                        mouse.kind = Mouse::WheelDown;
#endif
                    else
                        continue;
                    m_mouse.append(mouse);
                }
                continue;
            }
            case KEY_UP: key.code = Key::Up; break;
            case KEY_DOWN: key.code = Key::Down; break;
            case KEY_LEFT: key.code = Key::Left; break;
            case KEY_RIGHT: key.code = Key::Right; break;
            case KEY_HOME: key.code = Key::Home; break;
            case KEY_END: key.code = Key::End; break;
            case KEY_PPAGE: key.code = Key::PageUp; break;
            case KEY_NPAGE: key.code = Key::PageDown; break;
            case KEY_IC: key.code = Key::Insert; break;
            case KEY_DC: key.code = Key::Delete; break;
            case KEY_SDC: key.code = Key::Delete; key.shift = true; break;
            case KEY_BACKSPACE: key.code = Key::Backspace; break;
            case KEY_ENTER: key.code = Key::Enter; break;
            case KEY_BTAB: key.code = Key::Tab; key.shift = true; break;
            default:
                if (ch >= KEY_F(1) && ch <= KEY_F(12))
                    key.code = Key::Code(Key::F1 + int(ch - KEY_F(1)));
                else if (m_modified.contains(int(ch)))
                    key = m_modified.value(int(ch));
                else
                    continue; // a key we have no use for
            }
        } else if (ch == 27) {
            if (escape)
                keys.append(Key::of(Key::Escape)); // two in a row: the first was a real Esc
            escape = true;
            continue;
        } else if (ch == '\r' || ch == '\n') {
            key.code = Key::Enter;
        } else if (ch == '\t') {
            key.code = Key::Tab;
        } else if (ch == 127 || ch == 8) {
            key.code = Key::Backspace;
        } else if (ch < 32) {
            key = Key::control(char32_t('a' + ch - 1)); // Ctrl+A is 1
        } else {
            key = Key::character(char32_t(ch));
        }
        if (escape) {
            key.alt = true;
            escape = false;
        }
        keys.append(key);
    }
    if (escape)
        keys.append(Key::of(Key::Escape));
    return keys;
}

} // namespace Tui
