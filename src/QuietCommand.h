#pragma once

// A command Argonaut types into a shell of its own for the user ("cd" to
// the folder the files are in) that shouldn't clutter the terminal: the
// picture is held back while the shell takes it, and then the line that was
// typed is taken out, so what is seen is only the prompt, in its new folder.
//
// Both terminals use it (the window's TerminalWidget and the terminal
// interface's Shell): each is its own emulator (libvterm), so it can edit
// what it shows. **Only the rows the typed command itself took are removed,
// never anything that was there before**, and nothing at all when the
// command didn't do what it was for (the shell's complaint stays to read).

#include <QByteArray>
#include <QElapsedTimer>

extern "C" {
#include <vterm.h>
}

struct QuietCommand
{
    bool active = false;
    int row = 0;    // where the cursor was when the command was typed: the prompt's row
    int pushed = 0; // lines that have scrolled off the top since
    QElapsedTimer since, lastOutput;

    static constexpr int QuietMs = 60;  // no output for this long: the shell has said what it had to
    static constexpr int GiveUpMs = 1500;

    // Just before the command is written to the shell.
    void begin(VTerm *vt)
    {
        VTermPos at;
        vterm_state_get_cursorpos(vterm_obtain_state(vt), &at);
        row = at.row;
        pushed = 0;
        active = true;
        since.start();
        lastOutput.start();
    }
    void linePushed() // from the emulator's "a line left the top"
    {
        if (active)
            ++pushed;
    }
    void output() // the shell wrote something
    {
        if (active)
            lastOutput.restart();
    }
    // Time to end it: the command has done its work (`done`) and the shell
    // has gone quiet, or it has taken too long to wait for.
    bool settled(bool done) const { return active && ((done && lastOutput.elapsed() >= QuietMs) || since.elapsed() > GiveUpMs); }

    // Ends it. When the command did its work, the rows it was typed on are
    // taken out of the emulator's screen and the cursor goes up with what
    // followed. True: they were.
    bool finish(VTerm *vt, bool done)
    {
        if (!active)
            return false;
        active = false;
        if (!done)
            return false;
        VTermState *state = vterm_obtain_state(vt);
        VTermPos now;
        vterm_state_get_cursorpos(state, &now);
        int rows = 0, columns = 0;
        vterm_get_size(vt, &rows, &columns);
        const int first = row - pushed; // where the typed line is now
        if (first < 0 || first >= rows)
            return false; // it has scrolled out of the screen: left as it is
        // The typed line, and the rows it ran on into (a long folder name).
        int taken = 1;
        while (first + taken < rows && vterm_state_get_lineinfo(state, first + taken)->continuation)
            ++taken;
        if (now.row < first + taken)
            return false; // the cursor is still in it: not what was expected, so nothing is touched
        // To its first row; delete its rows (what is below comes up); the cursor where it was, as many rows higher.
        const QByteArray edit = "\033[" + QByteArray::number(first + 1) + ";1H\033[" + QByteArray::number(taken) + "M\033["
                                + QByteArray::number(now.row - taken + 1) + ';' + QByteArray::number(now.col + 1) + 'H';
        vterm_input_write(vt, edit.constData(), size_t(edit.size()));
        return true;
    }
};
