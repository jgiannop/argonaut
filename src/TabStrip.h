#pragma once

#include "Theme.h"

#include <QList>
#include <QStack>
#include <QString>
#include <QStringList>
#include <QTabBar>

#include <functional>

// The row of tabs. All tabs share the window's one file view, so a tab is
// only what it takes to show it again: its folder, back/forward history,
// selection and scroll position. Switching tabs saves the view into the tab
// you leave and restores the one you go to.
//
// The strip is hidden while there is a single tab. Closing the last tab
// closes the window.
class TabStrip : public QTabBar
{
    Q_OBJECT

public:
    struct Tab
    {
        QString path;
        QStack<QString> back;
        QStack<QString> forward;
        QStringList selection;
        int scroll = 0;
    };

    // What the strip needs from the window.
    struct Host
    {
        // Called before any change: the window gets the tabs' view in front
        // (with a split view, tabs belong to the first pane).
        std::function<void()> prepare;
        std::function<Tab()> capture;                   // the view as it is now
        std::function<void(const Tab &)> restore;       // show this tab
        std::function<QString(const QString &)> title;  // a folder's name for its tab
        std::function<void()> closeWindow;              // the last tab was closed
    };

    explicit TabStrip(const Host &host, QWidget *parent = nullptr);

    int activeIndex() const { return m_active; }
    QString pathAt(int index) const; // empty if there is no such tab

    // A new tab right after the active one, like browsers. A background tab
    // (`activate` false) doesn't take you there.
    void open(const QString &path, bool activate);
    void closeTab(int index);
    void closeActive() { closeTab(m_active); }
    void reopenClosed();  // Ctrl+Shift+T: the last closed tab, as it was
    void step(int delta); // the next (+1) or previous (-1) tab, wrapping round
    void activate(int index);

    // The active tab went to another folder: its name and tooltip follow.
    void setActivePath(const QString &path);
    void setTheme(const Theme &theme); // the close buttons' colour

private:
    void switchTo(int index);
    void saveActive();
    int insertTabFor(const Tab &tab, int index);
    void addCloseButton(int index);

    Host m_host;
    Theme m_theme;
    QList<Tab> m_tabs;
    QList<Tab> m_closed;     // for reopenClosed(), newest last
    int m_active = 0;
    bool m_editing = false;  // ignore our own signals while tabs are added or removed
};
