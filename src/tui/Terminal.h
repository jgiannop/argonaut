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

    bool canShowAnyColor() const;  // the terminal's palette can be set, and it shows every colour
    void setExactColors(bool on);  // gives the theme's very colours to palette entries of ours
    void setMotion(bool on);       // the mouse reports where it is as it moves
    void setDragging(bool on);     // …and, in any case, as it moves with a button held (a rectangle is dragged)

    int m_pairs[InkCount][PaperCount] = {};
    bool m_exact = false;          // the pairs are made of our own palette entries
    bool m_motion = false;
    bool m_dragging = false;
    bool m_again = false;          // everything is to be set up again (after another program had the terminal)
    Palette m_palette; // the colours the pairs were made for
    Screen m_shown;    // the last frame, to tell when many lines changed at once
    int m_nextPair = 1;
    bool m_colors = false;
    QHash<int, int> m_rawPairs;
    QHash<int, Key> m_modified; // Ctrl+Up, Alt+Left…: ncurses' key code → the key
    QList<Mouse> m_mouse;
};

} // namespace Tui
