#pragma once

#include "FileRules.h"
#include "Places.h"
#include "Screen.h"

#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>

#include <functional>

class DirectoryModel;
class FileJob;
class FileOperations;

namespace Tui {

class Shell;

// A key press, as the interface understands it (the terminal turns what it
// reads into these; tests make them directly).
struct Key
{
    enum Code {
        None, Char, Enter, Escape, Tab, Backspace, Up, Down, Left, Right, Home, End, PageUp, PageDown,
        Insert, Delete, F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    };
    Code code = None;
    char32_t ch = 0;    // Char: the character ('a' with ctrl for Ctrl+A)
    bool ctrl = false;
    bool alt = false;
    bool shift = false; // only where a terminal can tell (Shift+Delete, Shift+Tab)

    static Key of(Code code) { Key k; k.code = code; return k; }
    static Key character(char32_t c) { Key k; k.code = Char; k.ch = c; return k; }
    static Key control(char32_t c) { Key k; k.code = Char; k.ch = c; k.ctrl = true; return k; }
    bool isChar(char32_t c) const { return code == Char && !ctrl && !alt && ch == c; }
    bool isControl(char32_t c) const { return code == Char && ctrl && ch == c; }
};

// What the mouse did, in cells from the top-left corner.
struct Mouse
{
    enum Kind { Press, RightPress, WheelUp, WheelDown };
    Kind kind = Press;
    int x = 0;
    int y = 0;
};

// Argonaut in a terminal: one pane of files, or two side by side (F4; it
// starts with two when `rightFolder` is given), the same engine
// as the window (folder model, copy/move/trash with undo, the same rules
// before anything permanent, Git marks from the extension), drawn as text.
// It never talks to a terminal itself: key() takes key presses, render()
// draws a frame into a Screen.
class App : public QObject
{
    Q_OBJECT

public:
    App(const QString &leftFolder, const QString &rightFolder, QObject *parent = nullptr);
    ~App() override;

    void key(const Key &key);
    // A click puts the cursor on a row (and makes its pane the active one);
    // a second click on the same row soon after opens it. The right button
    // selects, the wheel scrolls, and the key line at the bottom can be clicked.
    void mouse(const Mouse &mouse);
    void render(Screen &screen);

    // For tests (and the terminal's title).
    QString folder(int pane) const;
    int activePane() const { return m_active; }
    bool isSplit() const { return m_split; }
    bool isPreviewShown() const { return m_preview; } // the preview at the right (F3)
    bool isInPreview() const { return m_inPreview; }  // and the keys scroll it
    bool isPreviewLoading() const { return m_previewLoading; }
    bool isSidebarShown() const { return m_sidebar; } // the list of places at the left (Ctrl+B)
    bool isInSidebar() const { return m_inSidebar; }  // and the keys go to it
    void setSidebarShown(bool on);
    bool isShellOpen() const { return m_shell != nullptr; } // the shell panel is under the panes
    bool isInShell() const { return m_inShell; }            // and what is typed goes to it
    // false: no panel; F12 hands the whole terminal over (shellRequested).
    // That is how it is without libvterm.
    void setEmbeddedShell(bool on) { m_embeddedShell = on; }
    // How a pane shows its folder (F9 goes round them).
    enum class View {
        List,  // a line an item, with size and date
        Grid,  // names in columns
        Icons, // big cells: a picture drawn from blocks, the name under it
    };
    View view(int pane) const { return m_panes[pane == 1 ? 1 : 0].view; }
    bool isGrid(int pane) const { return view(pane) == View::Grid; }
    QString cursorPath() const;         // the item under the active pane's cursor
    QStringList selectedPaths() const;  // the active pane's selection
    QString message() const { return m_message; }
    bool isAsking() const { return m_mode != Mode::Normal; } // a question or a text field is open
    bool isBusy() const;                // a file operation is running
    bool isLoading() const;             // a folder is still being listed
    FileOperations *operations() const { return m_ops; }
    void selectPath(const QString &path); // puts the cursor on it (when it is listed)
    void notify(const QString &text) { say(text); }
    // The shell is gone: the pane follows it to where it ended up (empty or
    // gone: it stays), and both panes are read again.
    void shellClosed(const QString &folder); // a line for the user, from outside (the pager is missing)

signals:
    void changed();       // something to draw
    // Show this text file in the terminal (a pager), then come back. The
    // interface can't do that itself: it never touches the terminal.
    void viewRequested(const QString &path);
    // F12 where there is no shell panel: a shell in this folder takes the terminal until it is left
    // (the interface never touches the terminal). Whoever runs it calls
    // shellClosed() with the folder the shell was in at the end.
    void shellRequested(const QString &folder);
    void quitRequested();

private:
    struct Pane
    {
        DirectoryModel *model = nullptr;
        QString current;
        QStringList back;      // where Alt+Left / "back" goes
        int cursor = 0;
        int top = 0;           // the first row shown
        QString cursorPath;    // kept across reloads
        QString wantedCursor;  // to put the cursor on once it is listed
        bool fresh = true;     // just arrived: the cursor hasn't been moved here yet
        QSet<QString> selected;
        View view = View::List;
        int lineHeight = 1;    // from the last frame: screen lines a line of items takes (6 for big icons)
        int rowsShown = 1;     // from the last frame: how many lines of items fit (how far PageDown goes)
        int x = 0, width = 0;  // from the last frame: where it is on screen
        int rowsY = 0;         // the screen row of its first file row
        int headY = 0;         // the screen row of its heading
        int cols = 1;          // grid, from the last frame: items on a line
        int cellWidth = 1;     // grid: cells an item takes
        int widestName = 0;    // grid: the longest name's width (of the first few thousand)
        int barY = 0, barHeight = 0; // the scrollbar, from the last frame (height 0: there is none)
        bool widthStale = true;
    };
    enum class Mode { Normal, Prompt, Question, Conflict, Help };

    Pane &pane() { return m_panes[m_active]; }
    const Pane &pane() const { return m_panes[m_active]; }
    Pane &other() { return m_panes[1 - m_active]; }

    bool navigate(Pane &p, const QString &path, bool remember = true);
    void goUp();
    void openCursor();
    void openFile(const QString &path); // Enter or a double click on a file
    void moveCursor(Pane &p, int row);
    void rowsChanged(Pane &p);
    int upRows(const Pane &p) const;   // 1 when the list starts with ".."
    int rowCount(const Pane &p) const; // the list's rows: ".." and the items
    QString pathAt(const Pane &p, int row) const;
    QStringList targets() const; // the selection, else the item under the cursor
    bool canChangeTargets() const;

    void transfer(bool move);
    void remove(bool permanently);
    void newFolder();
    void rename();
    void goTo();
    void setPreviewShown(bool on);
    void syncPreview();              // the preview is of the item under the cursor
    void drawPreview(Screen &screen, int x, int width, int top, int height);
    bool previewKey(const Key &key); // false: not the preview's key
    void scrollPreview(int top);
    void refreshPlaces();
    void drawSidebar(Screen &screen, int top, int height);
    bool sidebarKey(const Key &key); // false: not the sidebar's key
    void openPlace(int entry);
    void nextArea(bool backwards);   // Tab: the panes and the sidebar in turn
    void toggleShell();
    void closeShell();
    void drawShell(Screen &screen, int x, int right, int y, int height);
    void setSplit(bool on);
    void toggleGrid();
    int perLine(const Pane &p) const { return p.view == View::List ? 1 : qMax(1, p.cols); } // items on a line of the pane
    void toggleHidden();
    void quit();
    void say(const QString &text);

    // A line of text typed at the bottom; `done` gets it on Enter.
    void prompt(const QString &label, const QString &text, int selectTo, std::function<void(const QString &)> done);
    // A FileRules question at the bottom; `yes` runs only on "y".
    void ask(const FileRules::Question &question, std::function<void()> yes);
    void promptKey(const Key &key);
    void questionKey(const Key &key);
    void conflictKey(const Key &key);
    void normalKey(const Key &key);
    void nextConflict();
    void jobFinished(FileJob *job);

    void drawPane(Screen &screen, int index, int x, int width, int top, int height);
    void drawGrid(Screen &screen, Pane &p, bool active, int x, int width, int top, int height);
    void drawIcons(Screen &screen, Pane &p, bool active, int x, int width, int top, int height);
    // The bottom line of a grid: the item under the cursor with its size
    // and date, then the counts. `lines`: of items, in all and on screen.
    void drawGridFooter(Screen &screen, const Pane &p, int x, int width, int y, int totalLines, int lines);
    // A scrollbar down the pane's right edge, when there are more lines of
    // items (`total`) than fit (`shown`). `height`: screen lines it runs over.
    void drawScrollbar(Screen &screen, Pane &p, bool active, int x, int y, int height, int total, int shown);
    void scrollTo(Pane &p, int top); // the first line of items shown; the cursor comes along if it must
    QString footerText(const Pane &p) const; // how many items, and what is selected
    void drawBottom(Screen &screen);
    void drawHelp(Screen &screen);
    QString statusText() const;

    FileOperations *m_ops;
    Pane m_panes[2];
    int m_active = 0;
    bool m_split = false;      // both panes are on screen
    bool m_secondUsed = false; // the second pane has been shown once (it has its own folder now)
    bool m_showHidden = false;
    // The shell panel (F12): made when first asked for, gone when the shell ends.
    Shell *m_shell = nullptr;
    bool m_inShell = false;       // keys go to the shell, not to the files
    bool m_embeddedShell = true;
    int m_shellX = 0, m_shellY = -1, m_shellHeight = 0; // from the last frame
    int m_lastWidth = 100, m_lastHeight = 30;
    // The sidebar: the places (Places::list, as in the window), looked up
    // now and then. Rows are entries with a line between the groups.
    bool m_sidebar = true;
    bool m_inSidebar = false;
    QList<Places::Entry> m_places;
    QList<int> m_sideRows;       // what each row is: an entry's number, or -1 for a line
    QTimer m_placesTimer;
    int m_sideCursor = 0;        // the row the keys are on
    int m_sideTop = 0;
    int m_sideWidth = 0;         // from the last frame (0: not on screen)
    int m_sideY = 0, m_sideShown = 0;
    // The preview (F3): what Preview::load gives for the item under the
    // cursor, as lines of text or a picture made of half blocks. Loaded on
    // another thread; an answer for an item we have moved on from is dropped.
    bool m_preview = false;
    bool m_inPreview = false;
    QString m_previewPath;       // what it is of ("" : nothing)
    quint64 m_previewRequest = 0;
    bool m_previewLoading = false;
    QString m_previewTitle, m_previewInfo;
    QStringList m_previewLines;  // text, an archive's entries, or a line of explanation
    bool m_previewWarning = false; // the lines are a warning (a program in disguise)
    QImage m_previewImage;       // a picture, at a few pixels to the cell
    QStringList m_previewWrapped; // m_previewLines cut to the pane's width…
    int m_previewWrapWidth = -1;  // …which was this
    int m_previewTop = 0;
    int m_previewX = 0, m_previewWidth = 0;        // from the last frame (0: not on screen)
    int m_previewY = 0, m_previewShown = 0, m_previewTotal = 0;
    int m_helpTop = 0;           // the help's first line shown
    int m_helpLines = 0, m_helpShown = 0; // from the last frame
    Mode m_mode = Mode::Normal;
    QString m_message;
    QTimer m_messageTimer;

    QString m_promptLabel;
    QString m_promptText;
    int m_promptCursor = 0;
    std::function<void(const QString &)> m_promptDone;
    FileRules::Question m_question;
    std::function<void()> m_questionYes;
    struct Conflict
    {
        FileJob *job;
        QString source;
        QString existing;
    };
    QList<Conflict> m_conflicts;

    // For the mouse: the key line's clickable words (from the last frame),
    // and the last click, to tell a double click.
    struct KeySpot
    {
        int from, to; // cells
        Key key;
    };
    QList<KeySpot> m_keySpots;
    int m_keysY = -1;
    QElapsedTimer m_lastClick;
    QString m_lastClickPath;
};

} // namespace Tui
