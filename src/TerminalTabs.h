#pragma once

#include "Theme.h"

#include <QKeySequence>
#include <QList>
#include <QWidget>

class QStackedWidget;
class QTabBar;
class QToolButton;
class TerminalWidget;

// The terminal panel: several shells, each in a tab of its own, as in VS
// Code. A bar on top with the sessions' tabs (each with ×), "+" for a new
// one and the button that hides the panel; under it the session chosen.
// A session that ends (`exit`) closes its tab; the last one gone, the
// panel says so (allClosed) and the window hides it.
class TerminalTabs : public QWidget
{
    Q_OBJECT

public:
    explicit TerminalTabs(QWidget *parent = nullptr);

    TerminalWidget *current() const; // null: no session
    TerminalWidget *terminalAt(int index) const;
    int count() const;
    int currentIndex() const;
    void setCurrent(int index);
    // A new shell in `directory`, in a tab of its own, made the current one.
    // Null: it couldn't be started (no tab is added then).
    TerminalWidget *newSession(const QString &directory);
    // Closes a session. A program running in it (not the shell at its
    // prompt) is asked about first; false: the user kept it.
    bool closeSession(int index);
    bool hasFocusInside() const;

    void setTheme(const Theme &theme);
    // Keys that keep working while a terminal has the keyboard (the
    // window's terminal keys). Ctrl+PgUp / Ctrl+PgDn are the panel's own.
    void setPassthroughShortcuts(const QList<QKeySequence> &shortcuts);

    QTabBar *tabBar() const { return m_tabs; }         // (tests)
    QToolButton *newButton() const { return m_new; }   // (tests)
    QToolButton *hideButton() const { return m_hide; } // (tests)

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

signals:
    void newSessionRequested(); // "+": the window says where (the open folder)
    void hideRequested();       // the panel's ✕
    void allClosed();           // the last session ended or was closed
    void showDirectoryRequested(const QString &directory); // Ctrl+Up in a session

private:
    void addCloseButton(int index);
    void retitle(TerminalWidget *terminal);
    int indexOf(const QObject *terminal) const;
    void removeAt(int index);

    QTabBar *m_tabs = nullptr;
    QStackedWidget *m_stack = nullptr;
    QToolButton *m_new = nullptr;
    QToolButton *m_hide = nullptr;
    Theme m_theme;
    bool m_themed = false;
    QList<QKeySequence> m_passthrough;
    int m_made = 0; // sessions started, for "Terminal 2"…
};
