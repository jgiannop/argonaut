#pragma once

#include "Theme.h"

#include <QKeySequence>
#include <QList>
#include <QPoint>
#include <QVector>
#include <QWidget>

#include <vterm.h>

class QScrollBar;
class QSocketNotifier;

// An embedded terminal: the user's shell on a pseudo-terminal, emulated by
// libvterm and drawn with QPainter. Unix only (forkpty).
class TerminalWidget : public QWidget
{
    Q_OBJECT

public:
    explicit TerminalWidget(QWidget *parent = nullptr);
    ~TerminalWidget() override;

    // Start the user's shell ($SHELL) in `directory`.
    bool start(const QString &directory);
    bool isRunning() const { return m_pid > 0; }
    // True if the shell is at its prompt (no program running in the foreground).
    bool isShellIdle() const;
    // The shell's current directory (empty if unknown on this platform).
    QString shellDirectory() const;
    // `cd` the shell to `directory` if it's idle; returns whether it did.
    bool changeDirectory(const QString &directory);

    void setTheme(const Theme &theme);
    QString title() const { return m_title; }
    // Shortcuts that should keep working while the terminal has focus
    // (e.g. the one that hides it). Every other key goes to the shell.
    void setPassthroughShortcuts(const QList<QKeySequence> &shortcuts);
    // Visible screen text, one line per row (for tests).
    QString screenText() const;

signals:
    void titleChanged(const QString &title);
    void finished(); // the shell exited

protected:
    bool event(QEvent *event) override;
    bool focusNextPrevChild(bool) override { return false; } // Tab goes to the shell
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

private:
    using Cells = QVector<VTermScreenCell>;

    // libvterm callbacks
    static int onDamage(VTermRect rect, void *user);
    static int onMoveCursor(VTermPos pos, VTermPos oldPos, int visible, void *user);
    static int onTermProp(VTermProp prop, VTermValue *value, void *user);
    static int onBell(void *user);
    static int onPushLine(int cols, const VTermScreenCell *cells, void *user);
    static int onPopLine(int cols, VTermScreenCell *cells, void *user);
    static void onOutput(const char *bytes, size_t length, void *user);

    void readPty();
    void writePty(const QByteArray &bytes);
    void flushPending();
    void childExited();
    void updateGridSize();
    void updateScrollBar();
    void scrollToBottom();
    void applyColors();

    int scrollbackSize() const { return int(m_scrollback.size()); }
    bool cellAt(int line, int col, VTermScreenCell *cell) const; // absolute line
    QPoint cellUnder(const QPoint &pos) const;                    // (col, absolute line)
    bool isSelected(int line, int col) const;
    QString lineText(int line, int fromCol, int toCol) const;
    QString selectedText() const;
    void copySelection();
    void pasteText(const QString &text);
    QColor colorOf(VTermColor color, bool foreground) const;

    VTerm *m_vt = nullptr;
    VTermScreen *m_screen = nullptr;
    int m_ptyFd = -1;
    int m_pid = -1;
    QSocketNotifier *m_readNotifier = nullptr;
    QSocketNotifier *m_writeNotifier = nullptr;
    QByteArray m_pending; // output the pty couldn't take yet

    int m_rows = 24;
    int m_cols = 80;
    QFont m_font;
    int m_cellWidth = 8;
    int m_cellHeight = 16;
    int m_ascent = 12;
    QScrollBar *m_scrollBar = nullptr;
    QList<Cells> m_scrollback;
    int m_scrollOffset = 0; // lines scrolled up from the live screen

    VTermPos m_cursor{0, 0};
    bool m_cursorVisible = true;
    bool m_altScreen = false;
    QString m_title;
    QByteArray m_titleBuffer;

    // Selection, in (col, absolute line); absolute line 0 = oldest scrollback.
    QPoint m_selAnchor;
    QPoint m_selEnd;
    bool m_selecting = false;
    bool m_hasSelection = false;

    Theme m_theme;
    QColor m_background;
    QColor m_foreground;
    QList<QKeySequence> m_passthrough;
};
