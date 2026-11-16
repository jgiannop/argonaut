#pragma once

#include "App.h"
#include "Screen.h"

#include <QHash>
#include <QList>

// The real terminal, through ncurses: shows a Tui::Screen and turns what is
// typed into Tui::Keys. Everything that knows about ncurses is in here.
namespace Tui {

class Terminal
{
public:
    Terminal();  // takes the terminal over (full screen, no echo, keys as they come)
    ~Terminal(); // and gives it back as it was

    int width() const;
    int height() const;
    void show(const Screen &screen);
    // The keys typed since the last call. `resized` is set when the
    // terminal's size changed (draw again).
    QList<Key> readKeys(bool *resized);
    // What the mouse did meanwhile (filled by readKeys).
    QList<Mouse> takeMouse();
    // Hands the terminal to another program (a pager) and takes it back.
    void suspend();
    void resume();

private:
    int pairFor(Ink ink, Paper paper);
    int rawPair(int fg, int bg); // for a cell with the shell's own colours

    int m_pairs[8][5] = {};
    int m_nextPair = 1;
    bool m_colors = false;
    QHash<int, int> m_rawPairs;
    QHash<int, Key> m_modified; // Ctrl+Up, Alt+Left…: ncurses' key code → the key
    QList<Mouse> m_mouse;
};

} // namespace Tui
