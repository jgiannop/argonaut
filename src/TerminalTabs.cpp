#include "TerminalTabs.h"
#include "TerminalWidget.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMessageBox>
#include <QPushButton>
#include <QStackedWidget>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

TerminalTabs::TerminalTabs(QWidget *parent)
    : QWidget(parent)
{
    setObjectName("terminalPanel");
    setAttribute(Qt::WA_StyledBackground);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *bar = new QWidget(this);
    bar->setObjectName("terminalBar");
    bar->setAttribute(Qt::WA_StyledBackground);
    auto *row = new QHBoxLayout(bar);
    row->setContentsMargins(8, 3, 6, 3);
    row->setSpacing(4);
    m_tabs = new QTabBar(bar);
    m_tabs->setObjectName("terminalTabs");
    m_tabs->setDocumentMode(true);
    m_tabs->setDrawBase(false);
    m_tabs->setExpanding(false);
    m_tabs->setMovable(true);
    m_tabs->setUsesScrollButtons(true);
    m_tabs->setElideMode(Qt::ElideNone); // a long title is cut by retitle
    m_tabs->setFocusPolicy(Qt::NoFocus);
    m_new = new QToolButton(bar);
    m_new->setObjectName("flat");
    m_new->setText(QStringLiteral("+"));
    m_new->setToolTip(tr("New Terminal (Ctrl+Shift+`)"));
    m_new->setFocusPolicy(Qt::NoFocus);
    m_new->setAutoRaise(true);
    m_new->setFixedSize(26, 26);
    m_hide = new QToolButton(bar);
    m_hide->setObjectName("flat");
    m_hide->setToolTip(tr("Hide terminal (Ctrl+`)"));
    m_hide->setFocusPolicy(Qt::NoFocus);
    m_hide->setAutoRaise(true);
    m_hide->setFixedSize(26, 26);
    row->addWidget(m_tabs, 0, Qt::AlignVCenter);
    row->addWidget(m_new, 0, Qt::AlignVCenter);
    row->addStretch(1);
    row->addWidget(m_hide);
    layout->addWidget(bar);

    m_stack = new QStackedWidget(this);
    layout->addWidget(m_stack, 1);

    connect(m_new, &QToolButton::clicked, this, &TerminalTabs::newSessionRequested);
    connect(m_hide, &QToolButton::clicked, this, &TerminalTabs::hideRequested);
    connect(m_tabs, &QTabBar::currentChanged, this, [this](int index) {
        if (index < 0 || index >= m_stack->count())
            return;
        const bool hadFocus = hasFocusInside();
        m_stack->setCurrentIndex(index);
        if (hadFocus || isVisible())
            current()->setFocus(Qt::OtherFocusReason); // a click on a tab is to type there
    });
    connect(m_tabs, &QTabBar::tabMoved, this, [this](int from, int to) {
        QWidget *moved = m_stack->widget(from);
        m_stack->removeWidget(moved);
        m_stack->insertWidget(to, moved);
        m_stack->setCurrentIndex(m_tabs->currentIndex());
    });
}

// Ctrl+PgUp / Ctrl+PgDn in a terminal go round the sessions, as in VS Code
// and GNOME Terminal. Caught here, before the terminal would send them to its shell.
bool TerminalTabs::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::KeyPress) {
        const auto *key = static_cast<QKeyEvent *>(event);
        if (key->modifiers() == Qt::ControlModifier && (key->key() == Qt::Key_PageUp || key->key() == Qt::Key_PageDown)) {
            if (count() > 1) {
                const int step = key->key() == Qt::Key_PageUp ? count() - 1 : 1;
                setCurrent((currentIndex() + step) % count());
            }
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

TerminalWidget *TerminalTabs::current() const
{
    return qobject_cast<TerminalWidget *>(m_stack->currentWidget());
}

TerminalWidget *TerminalTabs::terminalAt(int index) const
{
    return qobject_cast<TerminalWidget *>(m_stack->widget(index));
}

int TerminalTabs::count() const
{
    return m_stack->count();
}

int TerminalTabs::currentIndex() const
{
    return m_stack->currentIndex();
}

void TerminalTabs::setCurrent(int index)
{
    if (index >= 0 && index < count())
        m_tabs->setCurrentIndex(index); // the stack follows (currentChanged)
}

bool TerminalTabs::hasFocusInside() const
{
    const QWidget *focus = QApplication::focusWidget();
    return focus && isAncestorOf(focus);
}

void TerminalTabs::setPassthroughShortcuts(const QList<QKeySequence> &shortcuts)
{
    m_passthrough = shortcuts;
    for (int i = 0; i < count(); ++i)
        terminalAt(i)->setPassthroughShortcuts(m_passthrough);
}

TerminalWidget *TerminalTabs::newSession(const QString &directory)
{
    auto *terminal = new TerminalWidget(m_stack);
    terminal->setPassthroughShortcuts(m_passthrough);
    terminal->installEventFilter(this); // Ctrl+PgUp / Ctrl+PgDn
    if (m_themed)
        terminal->setTheme(m_theme);
    if (!terminal->start(directory)) {
        delete terminal;
        return nullptr;
    }
    ++m_made;
    terminal->setProperty("argonautName", m_made == 1 ? tr("Terminal") : tr("Terminal %1").arg(m_made));
    connect(terminal, &TerminalWidget::titleChanged, this, [this, terminal] { retitle(terminal); });
    connect(terminal, &TerminalWidget::showDirectoryRequested, this, &TerminalTabs::showDirectoryRequested);
    // `exit`: its tab goes (after the terminal has finished telling it).
    connect(terminal, &TerminalWidget::finished, this, [this, terminal] {
        QTimer::singleShot(0, this, [this, terminal] {
            const int index = indexOf(terminal);
            if (index >= 0)
                removeAt(index);
        });
    });
    const int index = m_stack->addWidget(terminal);
    {
        const QSignalBlocker block(m_tabs);
        m_tabs->insertTab(index, QString());
        addCloseButton(index);
    }
    retitle(terminal);
    m_tabs->setCurrentIndex(index);
    m_stack->setCurrentIndex(index);
    return terminal;
}

bool TerminalTabs::closeSession(int index)
{
    TerminalWidget *terminal = terminalAt(index);
    if (!terminal)
        return false;
    if (terminal->isRunning() && !terminal->isShellIdle()) {
        QMessageBox box(QMessageBox::Question, tr("Close Terminal?"),
                        tr("A program is still running in “%1”. Closing the terminal stops it.")
                            .arg(m_tabs->tabToolTip(index)),
                        QMessageBox::NoButton, window());
        QPushButton *close = box.addButton(tr("Close Terminal"), QMessageBox::DestructiveRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(QMessageBox::Cancel);
        box.exec();
        if (box.clickedButton() != close)
            return false;
        index = indexOf(terminal); // (it may have ended meanwhile)
        if (index < 0)
            return true;
    }
    removeAt(index);
    return true;
}

// The session goes: its tab, its shell (the terminal's destructor ends it).
void TerminalTabs::removeAt(int index)
{
    TerminalWidget *terminal = terminalAt(index);
    if (!terminal)
        return;
    const bool hadFocus = hasFocusInside();
    {
        const QSignalBlocker block(m_tabs);
        m_tabs->removeTab(index);
    }
    m_stack->removeWidget(terminal);
    terminal->deleteLater();
    if (count() == 0) {
        emit allClosed();
        return;
    }
    const int next = qMin(index, count() - 1);
    {
        const QSignalBlocker block(m_tabs);
        m_tabs->setCurrentIndex(next);
    }
    m_stack->setCurrentIndex(m_tabs->currentIndex());
    if (hadFocus)
        current()->setFocus(Qt::OtherFocusReason);
}

int TerminalTabs::indexOf(const QObject *terminal) const
{
    for (int i = 0; i < count(); ++i) {
        if (m_stack->widget(i) == terminal)
            return i;
    }
    return -1;
}

// A tab says what its shell calls itself ("user@host: ~/dev"), cut when
// long; the whole title is its tooltip. A shell that says nothing: "Terminal 2".
void TerminalTabs::retitle(TerminalWidget *terminal)
{
    const int index = indexOf(terminal);
    if (index < 0)
        return;
    QString title = terminal->title().simplified();
    if (title.isEmpty())
        title = terminal->property("argonautName").toString();
    m_tabs->setTabToolTip(index, title);
    const QFontMetrics metrics = m_tabs->fontMetrics();
    m_tabs->setTabText(index, metrics.elidedText(title, Qt::ElideMiddle, metrics.horizontalAdvance(QLatin1Char('x')) * 26));
}

void TerminalTabs::addCloseButton(int index)
{
    auto *close = new QToolButton(m_tabs);
    close->setObjectName("flat");
    close->setFixedSize(18, 18);
    close->setIconSize(QSize(10, 10));
    close->setFocusPolicy(Qt::NoFocus);
    close->setAutoRaise(true);
    close->setToolTip(tr("Close This Terminal"));
    close->setIcon(glyphIcon(Glyph::Close, m_themed ? m_theme.dimFg : palette().color(QPalette::PlaceholderText)));
    // Tabs move and close, so the button finds its tab when it is pressed.
    connect(close, &QToolButton::clicked, this, [this, close] {
        for (int i = 0; i < m_tabs->count(); ++i) {
            if (m_tabs->tabButton(i, QTabBar::RightSide) == close) {
                closeSession(i);
                return;
            }
        }
    });
    m_tabs->setTabButton(index, QTabBar::RightSide, close);
}

void TerminalTabs::setTheme(const Theme &theme)
{
    m_theme = theme;
    m_themed = true;
    m_hide->setIcon(glyphIcon(Glyph::Close, theme.fg));
    for (int i = 0; i < count(); ++i) {
        terminalAt(i)->setTheme(theme);
        if (auto *close = qobject_cast<QToolButton *>(m_tabs->tabButton(i, QTabBar::RightSide)))
            close->setIcon(glyphIcon(Glyph::Close, theme.dimFg));
    }
}
