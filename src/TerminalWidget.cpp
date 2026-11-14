#include "TerminalWidget.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QPainter>
#include <QProcessEnvironment>
#include <QScrollBar>
#include <QSocketNotifier>

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#if defined(Q_OS_MACOS)
#include <util.h>
#else
#include <pty.h>
#endif

namespace {

constexpr int MaxScrollback = 10000;
constexpr int Padding = 6;

// GNOME Console's palette: readable on dark and light backgrounds.
const char *const kAnsi[16] = {
    "#241f31", "#c01c28", "#2ec27e", "#f5c211", "#1e78e4", "#9841bb", "#0ab9dc", "#c0bfbc",
    "#5e5c64", "#ed333b", "#57e389", "#f8e45c", "#51a1ff", "#c061cb", "#4fd2fd", "#f6f5f4",
};

VTermColor rgb(const QColor &c)
{
    VTermColor color;
    vterm_color_rgb(&color, uint8_t(c.red()), uint8_t(c.green()), uint8_t(c.blue()));
    return color;
}

QColor opaque(const QColor &over, const QColor &under)
{
    const qreal a = over.alphaF();
    return QColor::fromRgbF(over.redF() * a + under.redF() * (1 - a),
                            over.greenF() * a + under.greenF() * (1 - a),
                            over.blueF() * a + under.blueF() * (1 - a));
}

QString cellString(const VTermScreenCell &cell)
{
    QString text;
    for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; ++i)
        text += QString::fromUcs4(reinterpret_cast<const char32_t *>(&cell.chars[i]), 1);
    return text;
}

VTermScreenCell blankCell()
{
    VTermScreenCell cell{};
    cell.width = 1;
    vterm_color_indexed(&cell.fg, 7);
    cell.fg.type = VTERM_COLOR_DEFAULT_FG;
    cell.bg.type = VTERM_COLOR_DEFAULT_BG;
    return cell;
}

QByteArray shellQuote(const QString &path)
{
    QByteArray quoted = QFile::encodeName(path);
    quoted.replace('\'', "'\\''");
    return '\'' + quoted + '\'';
}

} // namespace

TerminalWidget::TerminalWidget(QWidget *parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setCursor(Qt::IBeamCursor);

    m_font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_font.setStyleHint(QFont::Monospace);
    if (m_font.pointSizeF() < 10.5)
        m_font.setPointSizeF(10.5);
    const QFontMetrics fm(m_font);
    m_cellWidth = qMax(1, fm.horizontalAdvance(QLatin1Char('M')));
    m_cellHeight = qMax(1, fm.height());
    m_ascent = fm.ascent();

    m_scrollBar = new QScrollBar(Qt::Vertical, this);
    m_scrollBar->setCursor(Qt::ArrowCursor);
    connect(m_scrollBar, &QScrollBar::valueChanged, this, [this](int value) {
        m_scrollOffset = m_scrollBar->maximum() - value;
        update();
    });

    m_vt = vterm_new(m_rows, m_cols);
    vterm_set_utf8(m_vt, 1);
    vterm_output_set_callback(m_vt, &TerminalWidget::onOutput, this);
    m_screen = vterm_obtain_screen(m_vt);
    static VTermScreenCallbacks callbacks{};
    callbacks.damage = &TerminalWidget::onDamage;
    callbacks.movecursor = &TerminalWidget::onMoveCursor;
    callbacks.settermprop = &TerminalWidget::onTermProp;
    callbacks.bell = &TerminalWidget::onBell;
    callbacks.sb_pushline = &TerminalWidget::onPushLine;
    callbacks.sb_popline = &TerminalWidget::onPopLine;
    vterm_screen_set_callbacks(m_screen, &callbacks, this);
    vterm_screen_set_damage_merge(m_screen, VTERM_DAMAGE_SCROLL);
    vterm_screen_enable_altscreen(m_screen, 1);
    vterm_screen_reset(m_screen, 1);
    applyColors();
}

TerminalWidget::~TerminalWidget()
{
    if (m_pid > 0) {
        ::kill(-m_pid, SIGHUP); // the shell's whole session
        ::kill(m_pid, SIGHUP);
    }
    if (m_ptyFd >= 0)
        ::close(m_ptyFd);
    if (m_pid > 0) {
        for (int i = 0; i < 20 && ::waitpid(m_pid, nullptr, WNOHANG) == 0; ++i)
            ::usleep(10000);
        if (::waitpid(m_pid, nullptr, WNOHANG) == 0) {
            ::kill(m_pid, SIGKILL);
            ::waitpid(m_pid, nullptr, 0);
        }
    }
    vterm_free(m_vt);
}

// ---------------------------------------------------------------- shell ---

bool TerminalWidget::start(const QString &directory)
{
    if (m_pid > 0)
        return true;
    // A fresh screen for a new session.
    m_scrollback.clear();
    m_scrollOffset = 0;
    m_hasSelection = false;
    vterm_screen_reset(m_screen, 1);
    updateScrollBar();
    update();

    // Prepare everything before fork(): after it, in a multi-threaded
    // program, the child may only call async-signal-safe functions.
    QByteArray shell = qgetenv("SHELL");
    if (shell.isEmpty() || ::access(shell.constData(), X_OK) != 0)
        shell = "/bin/sh";
    const QByteArray argv0 = QFileInfo(QString::fromLocal8Bit(shell)).fileName().toLocal8Bit();
    const QByteArray dir = QFile::encodeName(directory);

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("TERM", "xterm-256color");
    env.insert("COLORTERM", "truecolor");
    env.insert("TERM_PROGRAM", "kosmos");
    env.insert("PWD", directory);
    QList<QByteArray> envStrings;
    for (const QString &entry : env.toStringList())
        envStrings.append(entry.toLocal8Bit());
    QVector<char *> envp;
    for (QByteArray &entry : envStrings)
        envp.append(entry.data());
    envp.append(nullptr);
    char *argv[] = {const_cast<char *>(argv0.constData()), nullptr};

    winsize size{};
    size.ws_row = ushort(m_rows);
    size.ws_col = ushort(m_cols);
    int master = -1;
    const pid_t pid = ::forkpty(&master, nullptr, nullptr, &size);
    if (pid < 0)
        return false;
    if (pid == 0) {
        // Child: becomes the shell.
        if (::chdir(dir.constData()) != 0 && ::chdir("/") != 0) {
            // nowhere to go: the shell starts wherever we are
        }
        for (int fd = 3; fd < 1024; ++fd)
            ::close(fd);
        ::execve(shell.constData(), argv, envp.data());
        ::_exit(127);
    }

    m_pid = int(pid);
    m_ptyFd = master;
    ::fcntl(m_ptyFd, F_SETFL, ::fcntl(m_ptyFd, F_GETFL) | O_NONBLOCK);
    m_readNotifier = new QSocketNotifier(m_ptyFd, QSocketNotifier::Read, this);
    connect(m_readNotifier, &QSocketNotifier::activated, this, &TerminalWidget::readPty);
    m_writeNotifier = new QSocketNotifier(m_ptyFd, QSocketNotifier::Write, this);
    m_writeNotifier->setEnabled(false);
    connect(m_writeNotifier, &QSocketNotifier::activated, this, &TerminalWidget::flushPending);
    return true;
}

bool TerminalWidget::isShellIdle() const
{
    return m_pid > 0 && m_ptyFd >= 0 && ::tcgetpgrp(m_ptyFd) == m_pid;
}

QString TerminalWidget::shellDirectory() const
{
#if defined(Q_OS_LINUX)
    if (m_pid > 0)
        return QFileInfo(QStringLiteral("/proc/%1/cwd").arg(m_pid)).canonicalFilePath();
#endif
    return {};
}

bool TerminalWidget::changeDirectory(const QString &directory)
{
    if (!isShellIdle())
        return false; // never type into a running program
    if (QDir::cleanPath(shellDirectory()) == QDir::cleanPath(directory))
        return true;
    // Ctrl+E Ctrl+U clears whatever is typed at the prompt; the leading
    // space keeps the command out of bash's history (HISTCONTROL=ignorespace).
    writePty("\x05\x15 cd -- " + shellQuote(directory) + "\r");
    return true;
}

void TerminalWidget::readPty()
{
    char buffer[65536];
    while (true) {
        const ssize_t n = ::read(m_ptyFd, buffer, sizeof buffer);
        if (n > 0) {
            vterm_input_write(m_vt, buffer, size_t(n));
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            break;
        childExited(); // EOF or EIO: the shell is gone
        return;
    }
    vterm_screen_flush_damage(m_screen);
}

void TerminalWidget::writePty(const QByteArray &bytes)
{
    if (m_ptyFd < 0)
        return;
    m_pending += bytes;
    flushPending();
}

void TerminalWidget::flushPending()
{
    while (!m_pending.isEmpty()) {
        const ssize_t n = ::write(m_ptyFd, m_pending.constData(), size_t(m_pending.size()));
        if (n > 0) {
            m_pending.remove(0, qsizetype(n));
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        break; // EAGAIN: wait until the pty can take more
    }
    if (m_writeNotifier)
        m_writeNotifier->setEnabled(!m_pending.isEmpty());
}

void TerminalWidget::childExited()
{
    if (m_readNotifier)
        m_readNotifier->setEnabled(false);
    if (m_writeNotifier)
        m_writeNotifier->setEnabled(false);
    if (m_ptyFd >= 0)
        ::close(m_ptyFd);
    m_ptyFd = -1;
    if (m_pid > 0 && ::waitpid(m_pid, nullptr, WNOHANG) == 0) {
        ::kill(m_pid, SIGHUP);
        ::waitpid(m_pid, nullptr, 0);
    }
    m_pid = -1;
    m_pending.clear();
    emit finished();
}

// ------------------------------------------------------------- libvterm ---

int TerminalWidget::onDamage(VTermRect, void *user)
{
    static_cast<TerminalWidget *>(user)->update();
    return 1;
}

int TerminalWidget::onMoveCursor(VTermPos pos, VTermPos, int visible, void *user)
{
    auto *self = static_cast<TerminalWidget *>(user);
    self->m_cursor = pos;
    self->m_cursorVisible = visible;
    self->update();
    return 1;
}

int TerminalWidget::onTermProp(VTermProp prop, VTermValue *value, void *user)
{
    auto *self = static_cast<TerminalWidget *>(user);
    switch (prop) {
    case VTERM_PROP_CURSORVISIBLE:
        self->m_cursorVisible = value->boolean;
        self->update();
        break;
    case VTERM_PROP_ALTSCREEN:
        self->m_altScreen = value->boolean;
        self->updateScrollBar();
        break;
    case VTERM_PROP_TITLE:
        if (value->string.initial)
            self->m_titleBuffer.clear();
        self->m_titleBuffer.append(value->string.str, qsizetype(value->string.len));
        if (value->string.final) {
            self->m_title = QString::fromUtf8(self->m_titleBuffer);
            emit self->titleChanged(self->m_title);
        }
        break;
    default:
        break;
    }
    return 1;
}

int TerminalWidget::onBell(void *)
{
    return 1;
}

int TerminalWidget::onPushLine(int cols, const VTermScreenCell *cells, void *user)
{
    auto *self = static_cast<TerminalWidget *>(user);
    self->m_scrollback.append(Cells(cells, cells + cols));
    if (self->m_scrollback.size() > MaxScrollback) {
        self->m_scrollback.removeFirst();
        self->m_selAnchor.ry() -= 1;
        self->m_selEnd.ry() -= 1;
    } else if (self->m_scrollOffset > 0) {
        ++self->m_scrollOffset; // keep the view still while reading history
    }
    self->updateScrollBar();
    return 1;
}

int TerminalWidget::onPopLine(int cols, VTermScreenCell *cells, void *user)
{
    auto *self = static_cast<TerminalWidget *>(user);
    if (self->m_scrollback.isEmpty())
        return 0;
    const Cells line = self->m_scrollback.takeLast();
    for (int i = 0; i < cols; ++i)
        cells[i] = i < line.size() ? line.at(i) : blankCell();
    self->m_scrollOffset = qMin(self->m_scrollOffset, self->scrollbackSize());
    self->updateScrollBar();
    return 1;
}

void TerminalWidget::onOutput(const char *bytes, size_t length, void *user)
{
    static_cast<TerminalWidget *>(user)->writePty(QByteArray(bytes, qsizetype(length)));
}

// ------------------------------------------------------- size & colours ---

void TerminalWidget::setTheme(const Theme &theme)
{
    m_theme = theme;
    applyColors();
    update();
}

void TerminalWidget::applyColors()
{
    m_background = m_theme.window.isValid() ? (m_theme.dark ? m_theme.window.darker(112)
                                                             : m_theme.window.lighter(102))
                                            : QColor("#1e1e1e");
    m_foreground = m_theme.fg.isValid() ? opaque(m_theme.fg, m_background) : QColor("#e0e0e0");
    VTermState *state = vterm_obtain_state(m_vt);
    const VTermColor fg = rgb(m_foreground);
    const VTermColor bg = rgb(m_background);
    vterm_state_set_default_colors(state, &fg, &bg);
    for (int i = 0; i < 16; ++i) {
        VTermColor c = rgb(QColor(kAnsi[i]));
        vterm_state_set_palette_color(state, i, &c);
    }
}

void TerminalWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    const int barWidth = m_scrollBar->sizeHint().width();
    m_scrollBar->setGeometry(width() - barWidth, 0, barWidth, height());
    updateGridSize();
}

void TerminalWidget::updateGridSize()
{
    const int usableWidth = width() - 2 * Padding - m_scrollBar->width();
    const int rows = qMax(2, (height() - 2 * Padding) / m_cellHeight);
    const int cols = qMax(10, usableWidth / m_cellWidth);
    if (rows == m_rows && cols == m_cols)
        return;
    m_rows = rows;
    m_cols = cols;
    vterm_set_size(m_vt, m_rows, m_cols);
    vterm_screen_flush_damage(m_screen);
    if (m_ptyFd >= 0) {
        winsize size{};
        size.ws_row = ushort(m_rows);
        size.ws_col = ushort(m_cols);
        ::ioctl(m_ptyFd, TIOCSWINSZ, &size);
    }
    updateScrollBar();
    update();
}

void TerminalWidget::updateScrollBar()
{
    const int lines = m_altScreen ? 0 : scrollbackSize();
    const QSignalBlocker block(m_scrollBar);
    m_scrollBar->setRange(0, lines);
    m_scrollBar->setPageStep(m_rows);
    m_scrollOffset = qBound(0, m_scrollOffset, lines);
    m_scrollBar->setValue(lines - m_scrollOffset);
    m_scrollBar->setVisible(lines > 0);
}

void TerminalWidget::scrollToBottom()
{
    if (m_scrollOffset == 0)
        return;
    m_scrollOffset = 0;
    updateScrollBar();
    update();
}

void TerminalWidget::setPassthroughShortcuts(const QList<QKeySequence> &shortcuts)
{
    m_passthrough = shortcuts;
}

// -------------------------------------------------------------- drawing ---

bool TerminalWidget::cellAt(int line, int col, VTermScreenCell *cell) const
{
    const int sb = scrollbackSize();
    if (line < sb) {
        const Cells &cells = m_scrollback.at(line);
        if (col < cells.size()) {
            *cell = cells.at(col);
            return true;
        }
        *cell = blankCell();
        return true;
    }
    const int row = line - sb;
    if (row < 0 || row >= m_rows || col < 0 || col >= m_cols)
        return false;
    return vterm_screen_get_cell(m_screen, VTermPos{row, col}, cell) != 0;
}

QColor TerminalWidget::colorOf(VTermColor color, bool foreground) const
{
    if (foreground ? VTERM_COLOR_IS_DEFAULT_FG(&color) : VTERM_COLOR_IS_DEFAULT_BG(&color))
        return foreground ? m_foreground : m_background;
    vterm_screen_convert_color_to_rgb(m_screen, &color);
    return QColor(color.rgb.red, color.rgb.green, color.rgb.blue);
}

bool TerminalWidget::isSelected(int line, int col) const
{
    if (!m_hasSelection)
        return false;
    QPoint a = m_selAnchor;
    QPoint b = m_selEnd;
    if (a.y() > b.y() || (a.y() == b.y() && a.x() > b.x()))
        std::swap(a, b);
    if (line < a.y() || line > b.y())
        return false;
    if (line == a.y() && col < a.x())
        return false;
    if (line == b.y() && col > b.x())
        return false;
    return true;
}

void TerminalWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), m_background);
    p.setFont(m_font);

    QFont bold = m_font;
    bold.setBold(true);
    QFont italic = m_font;
    italic.setItalic(true);
    const QColor selection = m_theme.itemSelected.isValid()
                                 ? opaque(m_theme.itemSelected, m_background)
                                 : QColor("#3d5a80");
    const int top = scrollbackSize() - m_scrollOffset; // absolute line at row 0

    for (int row = 0; row < m_rows; ++row) {
        const int line = top + row;
        const int y = Padding + row * m_cellHeight;
        for (int col = 0; col < m_cols;) {
            VTermScreenCell cell;
            if (!cellAt(line, col, &cell)) {
                ++col;
                continue;
            }
            const int span = cell.width == 2 ? 2 : 1;
            QColor fg = colorOf(cell.fg, true);
            QColor bg = colorOf(cell.bg, false);
            if (cell.attrs.reverse)
                std::swap(fg, bg);
            if (isSelected(line, col))
                bg = selection;
            const QRect box(Padding + col * m_cellWidth, y, span * m_cellWidth, m_cellHeight);
            if (bg != m_background)
                p.fillRect(box, bg);

            const QString text = cell.attrs.conceal ? QString() : cellString(cell);
            if (!text.isEmpty() && text != QLatin1String(" ")) {
                p.setFont(cell.attrs.bold ? bold : (cell.attrs.italic ? italic : m_font));
                p.setPen(fg);
                p.drawText(box.left(), y + m_ascent, text);
            }
            if (cell.attrs.underline) {
                p.setPen(fg);
                p.drawLine(box.left(), y + m_ascent + 2, box.right(), y + m_ascent + 2);
            }
            if (cell.attrs.strike) {
                p.setPen(fg);
                p.drawLine(box.left(), y + m_cellHeight / 2, box.right(), y + m_cellHeight / 2);
            }
            col += span;
        }
    }

    // Cursor: a block when focused, an outline otherwise.
    if (m_cursorVisible && m_scrollOffset == 0 && m_pid > 0) {
        const QRect box(Padding + m_cursor.col * m_cellWidth, Padding + m_cursor.row * m_cellHeight,
                        m_cellWidth, m_cellHeight);
        if (hasFocus()) {
            p.fillRect(box, m_foreground);
            VTermScreenCell cell;
            if (vterm_screen_get_cell(m_screen, m_cursor, &cell)) {
                const QString text = cellString(cell);
                if (!text.isEmpty()) {
                    p.setFont(cell.attrs.bold ? bold : m_font);
                    p.setPen(m_background);
                    p.drawText(box.left(), box.top() + m_ascent, text);
                }
            }
        } else {
            p.setPen(m_foreground);
            p.drawRect(box.adjusted(0, 0, -1, -1));
        }
    }
}

// ------------------------------------------------------------- keyboard ---

bool TerminalWidget::event(QEvent *event)
{
    // Claim every key (Ctrl+C, Delete, Ctrl+A…) before the window's
    // shortcuts see it, except the few that must keep working here.
    if (event->type() == QEvent::ShortcutOverride) {
        auto *key = static_cast<QKeyEvent *>(event);
        const QKeySequence pressed(key->keyCombination());
        for (const QKeySequence &passthrough : std::as_const(m_passthrough)) {
            if (passthrough.matches(pressed) == QKeySequence::ExactMatch)
                return QWidget::event(event);
        }
        event->accept();
        return true;
    }
    return QWidget::event(event);
}

void TerminalWidget::keyPressEvent(QKeyEvent *event)
{
    const Qt::KeyboardModifiers mods = event->modifiers();
    const int key = event->key();

    // Terminal-style clipboard keys.
    if (mods == (Qt::ControlModifier | Qt::ShiftModifier)) {
        if (key == Qt::Key_C) {
            copySelection();
            return;
        }
        if (key == Qt::Key_V) {
            pasteText(QGuiApplication::clipboard()->text());
            return;
        }
    }
    if (mods == Qt::ShiftModifier && key == Qt::Key_Insert) {
        pasteText(QGuiApplication::clipboard()->text());
        return;
    }
    if (mods == Qt::ShiftModifier && (key == Qt::Key_PageUp || key == Qt::Key_PageDown)) {
        m_scrollBar->setValue(m_scrollBar->value() + (key == Qt::Key_PageUp ? -1 : 1) * m_rows);
        return;
    }

    if (m_pid <= 0)
        return;
    scrollToBottom();
    m_hasSelection = false;

    int vmod = VTERM_MOD_NONE;
    if (mods & Qt::ShiftModifier)
        vmod |= VTERM_MOD_SHIFT;
    if (mods & Qt::AltModifier)
        vmod |= VTERM_MOD_ALT;
    if (mods & Qt::ControlModifier)
        vmod |= VTERM_MOD_CTRL;
    const auto mod = VTermModifier(vmod);

    VTermKey vkey = VTERM_KEY_NONE;
    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        vkey = VTERM_KEY_ENTER;
        break;
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
        vkey = VTERM_KEY_TAB;
        break;
    case Qt::Key_Backspace:
        vkey = VTERM_KEY_BACKSPACE;
        break;
    case Qt::Key_Escape:
        vkey = VTERM_KEY_ESCAPE;
        break;
    case Qt::Key_Up:
        vkey = VTERM_KEY_UP;
        break;
    case Qt::Key_Down:
        vkey = VTERM_KEY_DOWN;
        break;
    case Qt::Key_Left:
        vkey = VTERM_KEY_LEFT;
        break;
    case Qt::Key_Right:
        vkey = VTERM_KEY_RIGHT;
        break;
    case Qt::Key_Insert:
        vkey = VTERM_KEY_INS;
        break;
    case Qt::Key_Delete:
        vkey = VTERM_KEY_DEL;
        break;
    case Qt::Key_Home:
        vkey = VTERM_KEY_HOME;
        break;
    case Qt::Key_End:
        vkey = VTERM_KEY_END;
        break;
    case Qt::Key_PageUp:
        vkey = VTERM_KEY_PAGEUP;
        break;
    case Qt::Key_PageDown:
        vkey = VTERM_KEY_PAGEDOWN;
        break;
    default:
        if (key >= Qt::Key_F1 && key <= Qt::Key_F35)
            vkey = VTermKey(VTERM_KEY_FUNCTION(key - Qt::Key_F1 + 1));
        break;
    }
    if (vkey != VTERM_KEY_NONE) {
        vterm_keyboard_key(m_vt, vkey, key == Qt::Key_Backtab ? VTERM_MOD_SHIFT : mod);
        return;
    }

    // Ctrl+letter etc.: send the base key so libvterm makes the control code.
    if (mods & Qt::ControlModifier) {
        uint32_t c = 0;
        if (key >= Qt::Key_A && key <= Qt::Key_Z)
            c = uint32_t('a' + (key - Qt::Key_A));
        else if (key >= 0x20 && key < 0x7f)
            c = uint32_t(key);
        if (c) {
            vterm_keyboard_unichar(m_vt, c, VTermModifier(vmod & ~VTERM_MOD_SHIFT));
            return;
        }
    }

    const QString text = event->text();
    for (const char32_t c : text.toUcs4()) {
        if (c < 0x20 || c == 0x7f)
            continue; // handled above
        vterm_keyboard_unichar(m_vt, uint32_t(c), VTermModifier(vmod & ~VTERM_MOD_SHIFT));
    }
}

// Composed text from input methods (dead keys, Greek accents, CJK…).
void TerminalWidget::inputMethodEvent(QInputMethodEvent *event)
{
    if (m_pid > 0 && !event->commitString().isEmpty()) {
        scrollToBottom();
        for (const char32_t c : event->commitString().toUcs4())
            vterm_keyboard_unichar(m_vt, uint32_t(c), VTERM_MOD_NONE);
    }
    event->accept();
}

QVariant TerminalWidget::inputMethodQuery(Qt::InputMethodQuery query) const
{
    if (query == Qt::ImCursorRectangle)
        return QRect(Padding + m_cursor.col * m_cellWidth, Padding + m_cursor.row * m_cellHeight,
                     m_cellWidth, m_cellHeight);
    if (query == Qt::ImFont)
        return m_font;
    return QWidget::inputMethodQuery(query);
}

void TerminalWidget::focusInEvent(QFocusEvent *event)
{
    QWidget::focusInEvent(event);
    update();
}

void TerminalWidget::focusOutEvent(QFocusEvent *event)
{
    QWidget::focusOutEvent(event);
    update();
}

// -------------------------------------------------- mouse & clipboard ---

QPoint TerminalWidget::cellUnder(const QPoint &pos) const
{
    const int col = qBound(0, (pos.x() - Padding) / m_cellWidth, m_cols - 1);
    const int row = qBound(0, (pos.y() - Padding) / m_cellHeight, m_rows - 1);
    return QPoint(col, scrollbackSize() - m_scrollOffset + row);
}

void TerminalWidget::mousePressEvent(QMouseEvent *event)
{
    setFocus(Qt::MouseFocusReason);
    if (event->button() == Qt::LeftButton) {
        m_selAnchor = m_selEnd = cellUnder(event->position().toPoint());
        m_selecting = true;
        m_hasSelection = false;
        update();
    } else if (event->button() == Qt::MiddleButton) {
        pasteText(QGuiApplication::clipboard()->text(QClipboard::Selection));
    }
}

void TerminalWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_selecting)
        return;
    m_selEnd = cellUnder(event->position().toPoint());
    m_hasSelection = m_selEnd != m_selAnchor;
    update();
}

void TerminalWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    m_selecting = false;
    if (m_hasSelection && QGuiApplication::clipboard()->supportsSelection())
        QGuiApplication::clipboard()->setText(selectedText(), QClipboard::Selection);
}

// Double-click selects a word (path-friendly: keeps / . - _ ~).
void TerminalWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
    const QPoint at = cellUnder(event->position().toPoint());
    auto isWordChar = [this, &at](int col) {
        VTermScreenCell cell;
        if (!cellAt(at.y(), col, &cell))
            return false;
        const QString s = cellString(cell);
        return !s.isEmpty() && (s.at(0).isLetterOrNumber() || QStringLiteral("/._-~:@+").contains(s.at(0)));
    };
    if (!isWordChar(at.x()))
        return;
    int from = at.x();
    int to = at.x();
    while (from > 0 && isWordChar(from - 1))
        --from;
    while (to < m_cols - 1 && isWordChar(to + 1))
        ++to;
    m_selAnchor = QPoint(from, at.y());
    m_selEnd = QPoint(to, at.y());
    m_hasSelection = true;
    m_selecting = false;
    update();
    if (QGuiApplication::clipboard()->supportsSelection())
        QGuiApplication::clipboard()->setText(selectedText(), QClipboard::Selection);
}

void TerminalWidget::wheelEvent(QWheelEvent *event)
{
    const int lines = -event->angleDelta().y() / 40; // 3 lines per notch
    if (lines != 0)
        m_scrollBar->setValue(m_scrollBar->value() + lines);
    event->accept();
}

QString TerminalWidget::lineText(int line, int fromCol, int toCol) const
{
    QString text;
    for (int col = fromCol; col <= toCol && col < m_cols; ++col) {
        VTermScreenCell cell;
        if (!cellAt(line, col, &cell))
            break;
        const QString s = cellString(cell);
        text += s.isEmpty() ? QStringLiteral(" ") : s;
        if (cell.width == 2)
            ++col;
    }
    while (text.endsWith(QLatin1Char(' ')))
        text.chop(1);
    return text;
}

QString TerminalWidget::selectedText() const
{
    if (!m_hasSelection)
        return {};
    QPoint a = m_selAnchor;
    QPoint b = m_selEnd;
    if (a.y() > b.y() || (a.y() == b.y() && a.x() > b.x()))
        std::swap(a, b);
    QStringList lines;
    for (int line = a.y(); line <= b.y(); ++line)
        lines.append(lineText(line, line == a.y() ? a.x() : 0, line == b.y() ? b.x() : m_cols - 1));
    return lines.join(QLatin1Char('\n'));
}

QString TerminalWidget::screenText() const
{
    QStringList lines;
    for (int row = 0; row < m_rows; ++row)
        lines.append(lineText(scrollbackSize() + row, 0, m_cols - 1));
    return lines.join(QLatin1Char('\n'));
}

void TerminalWidget::copySelection()
{
    if (m_hasSelection)
        QGuiApplication::clipboard()->setText(selectedText());
}

// Paste as one block: with bracketed paste on (bash, zsh, vim…) the shell
// won't run pasted lines until you press Enter.
void TerminalWidget::pasteText(const QString &text)
{
    if (m_pid <= 0 || text.isEmpty())
        return;
    scrollToBottom();
    vterm_keyboard_start_paste(m_vt);
    for (const char32_t c : QString(text).replace(QLatin1String("\r\n"), QLatin1String("\r"))
                                .replace(QLatin1Char('\n'), QLatin1Char('\r'))
                                .toUcs4())
        vterm_keyboard_unichar(m_vt, uint32_t(c), VTERM_MOD_NONE);
    vterm_keyboard_end_paste(m_vt);
}

void TerminalWidget::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);
    QAction *copy = menu.addAction(tr("Copy"), this, &TerminalWidget::copySelection);
    copy->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C));
    copy->setEnabled(m_hasSelection);
    QAction *paste = menu.addAction(tr("Paste"), this,
                                    [this] { pasteText(QGuiApplication::clipboard()->text()); });
    paste->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V));
    menu.exec(event->globalPos());
}
