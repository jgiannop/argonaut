#include "TabStrip.h"

#include <QDir>
#include <QToolButton>

namespace {
constexpr int MaxClosedTabs = 10; // how many Ctrl+Shift+T can bring back
}

TabStrip::TabStrip(const Host &host, QWidget *parent) : QTabBar(parent), m_host(host)
{
    setObjectName("tabs");
    setExpanding(true);
    setMovable(true);
    setDocumentMode(true);
    setElideMode(Qt::ElideMiddle);
    setFocusPolicy(Qt::NoFocus);

    m_tabs.append(Tab());
    addTab(QString());
    addCloseButton(0);
    hide(); // shown from two tabs on

    connect(this, &QTabBar::currentChanged, this, &TabStrip::switchTo);
    // Dragging a tab to another place: our list follows, and so does the
    // number of the active one.
    connect(this, &QTabBar::tabMoved, this, [this](int from, int to) {
        m_tabs.move(from, to);
        if (m_active == from)
            m_active = to;
        else if (from < m_active && to >= m_active)
            --m_active;
        else if (from > m_active && to <= m_active)
            ++m_active;
    });
}

QString TabStrip::pathAt(int index) const
{
    return index >= 0 && index < m_tabs.size() ? m_tabs.at(index).path : QString();
}

void TabStrip::addCloseButton(int index)
{
    auto *close = new QToolButton(this);
    close->setObjectName("flat");
    close->setFixedSize(20, 20);
    close->setIconSize(QSize(12, 12));
    close->setFocusPolicy(Qt::NoFocus);
    close->setToolTip(tr("Close Tab (Ctrl+W)"));
    close->setIcon(glyphIcon(Glyph::Close, m_theme.dimFg));
    // Tabs move and close, so the button finds its tab when it is pressed.
    connect(close, &QToolButton::clicked, this, [this, close] {
        for (int i = 0; i < count(); ++i) {
            if (tabButton(i, QTabBar::RightSide) == close) {
                closeTab(i);
                return;
            }
        }
    });
    setTabButton(index, QTabBar::RightSide, close);
}

void TabStrip::setTheme(const Theme &theme)
{
    m_theme = theme;
    for (int i = 0; i < count(); ++i) {
        if (auto *close = qobject_cast<QToolButton *>(tabButton(i, QTabBar::RightSide)))
            close->setIcon(glyphIcon(Glyph::Close, theme.dimFg));
    }
}

void TabStrip::saveActive()
{
    if (m_active >= 0 && m_active < m_tabs.size())
        m_tabs[m_active] = m_host.capture();
}

int TabStrip::insertTabFor(const Tab &tab, int index)
{
    m_editing = true;
    m_tabs.insert(index, tab);
    insertTab(index, m_host.title(tab.path));
    setTabToolTip(index, QDir::toNativeSeparators(tab.path));
    addCloseButton(index);
    m_editing = false;
    setVisible(m_tabs.size() > 1);
    return index;
}

void TabStrip::open(const QString &path, bool activate)
{
    m_host.prepare();
    saveActive();
    Tab tab;
    tab.path = path;
    const int index = insertTabFor(tab, m_active + 1);
    if (activate)
        setCurrentIndex(index);
}

void TabStrip::closeTab(int index)
{
    m_host.prepare();
    if (index < 0 || index >= m_tabs.size())
        return;
    if (m_tabs.size() == 1) {
        m_host.closeWindow();
        return;
    }
    const bool wasActive = index == m_active;
    if (wasActive)
        saveActive();
    m_closed.append(m_tabs.at(index));
    if (m_closed.size() > MaxClosedTabs)
        m_closed.removeFirst();

    m_editing = true;
    m_tabs.removeAt(index);
    removeTab(index);
    m_editing = false;
    if (wasActive) {
        m_active = -1; // nothing to save: it's gone
        switchTo(currentIndex());
    } else if (index < m_active) {
        --m_active;
    }
    setVisible(m_tabs.size() > 1);
}

void TabStrip::reopenClosed()
{
    m_host.prepare();
    if (m_closed.isEmpty())
        return;
    saveActive();
    // With its history, selection and scroll position, not only its folder.
    setCurrentIndex(insertTabFor(m_closed.takeLast(), m_active + 1));
}

void TabStrip::step(int delta)
{
    const int tabs = int(m_tabs.size());
    if (tabs > 1)
        setCurrentIndex((m_active + delta + tabs) % tabs);
}

void TabStrip::activate(int index)
{
    if (index >= 0 && index < m_tabs.size())
        setCurrentIndex(index);
}

void TabStrip::switchTo(int index)
{
    if (m_editing || index < 0 || index >= m_tabs.size() || index == m_active)
        return;
    m_host.prepare();
    saveActive();
    m_active = index;
    m_host.restore(Tab(m_tabs.at(index))); // a copy: restoring renames the tab (setActivePath)
}

void TabStrip::setActivePath(const QString &path)
{
    if (m_active < 0 || m_active >= m_tabs.size())
        return;
    setTabText(m_active, m_host.title(path));
    setTabToolTip(m_active, QDir::toNativeSeparators(path));
    m_tabs[m_active].path = path;
}
