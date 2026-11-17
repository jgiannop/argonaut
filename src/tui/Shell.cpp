#include "Shell.h"

#include "App.h"

#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QSocketNotifier>
#include <QVector>

#if defined(ARGONAUT_TUI_SHELL)

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#if defined(Q_OS_MACOS)
#include <util.h>
#elif defined(Q_OS_FREEBSD)
#include <libutil.h>
#else
#include <pty.h>
#endif
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "QuietCommand.h"

#include <QTimer>

namespace Tui {

struct Shell::Private
{
    VTerm *vt = nullptr;
    VTermScreen *screen = nullptr;
    bool cursorVisible = true;
    bool wholePanel = false; // a program has the alternate screen (an editor, a pager)
    QuietCommand quiet;      // a "cd" typed for the user, which isn't shown
    QList<QList<VTermScreenCell>> history; // the lines that scrolled off the top, oldest first

    // A line leaves the top: kept. While the panel looks back, what it shows stays where it is.
    static int pushLine(int cols, const VTermScreenCell *cells, void *user)
    {
        auto *self = static_cast<Shell *>(user);
        self->d->quiet.linePushed();
        self->d->history.append(QList<VTermScreenCell>(cells, cells + cols));
        if (self->d->history.size() > MaxHistory)
            self->d->history.removeFirst();
        else if (self->m_back > 0)
            ++self->m_back;
        return 1;
    }
    // The panel got taller: the newest kept line comes back onto the screen.
    static int popLine(int cols, VTermScreenCell *cells, void *user)
    {
        auto *self = static_cast<Shell *>(user);
        if (self->d->history.isEmpty())
            return 0;
        const QList<VTermScreenCell> line = self->d->history.takeLast();
        for (int i = 0; i < cols; ++i) {
            if (i < line.size()) {
                cells[i] = line.at(i);
            } else {
                cells[i] = VTermScreenCell{};
                cells[i].width = 1;
                cells[i].fg.type = VTERM_COLOR_DEFAULT_FG;
                cells[i].bg.type = VTERM_COLOR_DEFAULT_BG;
            }
        }
        self->m_back = qMin(self->m_back, int(self->d->history.size()));
        return 1;
    }

    static void output(const char *bytes, size_t length, void *user) { static_cast<Shell *>(user)->send(bytes, length); }
    static int termProp(VTermProp prop, VTermValue *value, void *user)
    {
        auto *self = static_cast<Shell *>(user);
        if (prop == VTERM_PROP_CURSORVISIBLE)
            self->d->cursorVisible = value->boolean;
        if (prop == VTERM_PROP_ALTSCREEN) {
            self->d->wholePanel = value->boolean;
            self->m_back = 0; // a program takes the panel, or gives it back: the newest is shown
        }
        return 1;
    }
};

namespace {

// A colour of the shell as one of the terminal's 256 (-1: its default).
qint16 colorIndex(const VTermScreen *screen, VTermColor color, bool foreground)
{
    if (foreground ? VTERM_COLOR_IS_DEFAULT_FG(&color) : VTERM_COLOR_IS_DEFAULT_BG(&color))
        return -1;
    if (VTERM_COLOR_IS_INDEXED(&color))
        return qint16(color.indexed.idx);
    vterm_screen_convert_color_to_rgb(screen, &color);
    const int r = color.rgb.red, g = color.rgb.green, b = color.rgb.blue;
    // The nearest of the 6×6×6 cube or the 24 greys.
    auto level = [](int v) { return v < 48 ? 0 : v < 115 ? 1 : (v - 35) / 40; };
    if (qAbs(r - g) < 10 && qAbs(g - b) < 10) {
        if (r < 8)
            return 16;
        if (r > 238)
            return 231;
        return qint16(232 + (r - 8) / 10);
    }
    return qint16(16 + 36 * level(r) + 6 * level(g) + level(b));
}

} // namespace

bool Shell::available()
{
    return true;
}

Shell::Shell(QObject *parent)
    : QObject(parent)
    , d(new Private)
{
}

Shell::~Shell()
{
    if (m_pid > 0) {
        ::kill(-m_pid, SIGHUP); // the shell's whole session
        ::kill(m_pid, SIGHUP);
    }
    if (m_fd >= 0)
        ::close(m_fd);
    if (m_pid > 0) {
        for (int i = 0; i < 20 && ::waitpid(m_pid, nullptr, WNOHANG) == 0; ++i)
            ::usleep(10000);
        if (::waitpid(m_pid, nullptr, WNOHANG) == 0) {
            ::kill(m_pid, SIGKILL);
            ::waitpid(m_pid, nullptr, 0);
        }
    }
    if (d->vt)
        vterm_free(d->vt);
    delete d;
}

bool Shell::start(const QString &folder, int rows, int columns)
{
    if (m_pid > 0)
        return true;
    m_rows = qMax(1, rows);
    m_columns = qMax(2, columns);
    d->vt = vterm_new(m_rows, m_columns);
    vterm_set_utf8(d->vt, 1);
    vterm_output_set_callback(d->vt, &Private::output, this);
    d->screen = vterm_obtain_screen(d->vt);
    static VTermScreenCallbacks callbacks{};
    callbacks.settermprop = &Private::termProp;
    callbacks.sb_pushline = &Private::pushLine;
    callbacks.sb_popline = &Private::popLine;
    vterm_screen_set_callbacks(d->screen, &callbacks, this);
    vterm_screen_enable_altscreen(d->screen, 1);
    vterm_screen_reset(d->screen, 1);

    // Everything is prepared before fork(): after it, in a program with
    // several threads, the child may only call async-signal-safe functions.
    QByteArray shell = qgetenv("SHELL");
    if (shell.isEmpty() || ::access(shell.constData(), X_OK) != 0)
        shell = "/bin/sh";
    const QByteArray argv0 = QFileInfo(QString::fromLocal8Bit(shell)).fileName().toLocal8Bit();
    const QByteArray dir = QFile::encodeName(folder);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("TERM"), QStringLiteral("xterm-256color"));
    env.insert(QStringLiteral("TERM_PROGRAM"), QStringLiteral("argonaut"));
    env.insert(QStringLiteral("PWD"), folder);
    env.remove(QStringLiteral("QT_QPA_PLATFORM")); // ours (offscreen): programs started from the shell want the real one
    env.remove(QStringLiteral("COLORTERM"));       // colours are shown as the nearest of 256
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
    size.ws_col = ushort(m_columns);
    int master = -1;
    const pid_t pid = ::forkpty(&master, nullptr, nullptr, &size);
    if (pid < 0)
        return false;
    if (pid == 0) {
        if (::chdir(dir.constData()) != 0 && ::chdir("/") != 0) {
            // nowhere to go: the shell starts wherever we are
        }
        for (int fd = 3; fd < 1024; ++fd)
            ::close(fd);
        ::execve(shell.constData(), argv, envp.data());
        ::_exit(127);
    }
    m_pid = int(pid);
    m_fd = master;
    ::fcntl(m_fd, F_SETFL, ::fcntl(m_fd, F_GETFL) | O_NONBLOCK);
    ::fcntl(m_fd, F_SETFD, FD_CLOEXEC);
    m_readNotifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
    connect(m_readNotifier, &QSocketNotifier::activated, this, &Shell::readPty);
    m_writeNotifier = new QSocketNotifier(m_fd, QSocketNotifier::Write, this);
    m_writeNotifier->setEnabled(false);
    connect(m_writeNotifier, &QSocketNotifier::activated, this, &Shell::flush);
    return true;
}

void Shell::resize(int rows, int columns)
{
    rows = qMax(1, rows);
    columns = qMax(2, columns);
    if (!d->vt || (rows == m_rows && columns == m_columns))
        return;
    m_rows = rows;
    m_columns = columns;
    vterm_set_size(d->vt, m_rows, m_columns);
    if (m_fd >= 0) {
        winsize size{};
        size.ws_row = ushort(m_rows);
        size.ws_col = ushort(m_columns);
        ::ioctl(m_fd, TIOCSWINSZ, &size);
    }
}

void Shell::readPty()
{
    char buffer[16384];
    bool any = false;
    bool ended = false;
    while (true) {
        const ssize_t n = ::read(m_fd, buffer, sizeof buffer);
        if (n > 0) {
            vterm_input_write(d->vt, buffer, size_t(n));
            d->quiet.output();
            any = true;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            break;
        ended = true; // the shell closed its side
        break;
    }
    if (ended)
        endQuiet();
    if (any && !d->quiet.active) // while a command typed for the user is being taken, the picture waits
        emit changed();
    if (ended) {
        m_readNotifier->setEnabled(false);
        m_writeNotifier->setEnabled(false);
        ::close(m_fd);
        m_fd = -1;
        // It is exiting; give it a moment, then it is the destructor's.
        for (int i = 0; i < 20 && ::waitpid(m_pid, nullptr, WNOHANG) == 0; ++i)
            ::usleep(5000);
        if (::waitpid(m_pid, nullptr, WNOHANG) != 0)
            m_pid = 0;
        emit finished();
    }
}

void Shell::send(const char *bytes, size_t length)
{
    if (m_fd < 0)
        return;
    m_pending.append(bytes, qsizetype(length));
    flush();
}

void Shell::flush()
{
    while (m_fd >= 0 && !m_pending.isEmpty()) {
        const ssize_t n = ::write(m_fd, m_pending.constData(), size_t(m_pending.size()));
        if (n > 0) {
            m_pending.remove(0, qsizetype(n));
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        break; // full for now (or gone)
    }
    if (m_writeNotifier)
        m_writeNotifier->setEnabled(m_fd >= 0 && !m_pending.isEmpty());
}

bool Shell::changeDirectory(const QString &folder)
{
    if (m_pid <= 0 || m_fd < 0 || ::tcgetpgrp(m_fd) != m_pid)
        return false; // a program has the terminal: it is not ours to type into
    QByteArray quoted = QFile::encodeName(folder);
    quoted.replace('\'', "'\\''");
    // Ctrl+E Ctrl+U clears what is typed at the prompt; the leading space
    // keeps the command out of bash's history (HISTCONTROL=ignorespace).
    const QByteArray line = "\x05\x15 cd -- '" + quoted + "'\r";
    scrollToNewest();
    // The typed line isn't to be seen (QuietCommand.h): nothing new is
    // drawn until the shell has taken it, then the line is taken out.
    m_quietTarget = QFileInfo(folder).canonicalFilePath();
    d->quiet.begin(d->vt);
    if (!m_quietTimer) {
        m_quietTimer = new QTimer(this);
        m_quietTimer->setInterval(15);
        connect(m_quietTimer, &QTimer::timeout, this, [this] {
            const bool done = m_pid > 0 && m_fd >= 0 && ::tcgetpgrp(m_fd) == m_pid && this->folder() == m_quietTarget;
            if (d->quiet.settled(done))
                endQuiet();
        });
    }
    m_quietTimer->start();
    send(line.constData(), size_t(line.size()));
    return true;
}

void Shell::endQuiet()
{
    if (m_quietTimer)
        m_quietTimer->stop();
    if (!d->quiet.active)
        return;
    const bool done = m_pid > 0 && m_fd >= 0 && ::tcgetpgrp(m_fd) == m_pid && folder() == m_quietTarget;
    if (d->vt)
        d->quiet.finish(d->vt, done);
    d->quiet.active = false;
    emit changed();
}

int Shell::historySize() const
{
    return d->wholePanel ? 0 : int(d->history.size());
}

void Shell::scrollBack(int lines)
{
    const int to = qBound(0, m_back + lines, historySize());
    if (to == m_back)
        return;
    m_back = to;
    emit changed();
}

void Shell::key(const Key &key)
{
    if (!d->vt || m_fd < 0)
        return;
    scrollToNewest(); // what is typed is seen
    int mod = VTERM_MOD_NONE;
    if (key.ctrl)
        mod |= VTERM_MOD_CTRL;
    if (key.alt)
        mod |= VTERM_MOD_ALT;
    if (key.shift)
        mod |= VTERM_MOD_SHIFT;
    VTermKey special = VTERM_KEY_NONE;
    switch (key.code) {
    case Key::Char:
        vterm_keyboard_unichar(d->vt, uint32_t(key.ch), VTermModifier(mod & ~VTERM_MOD_SHIFT));
        return;
    case Key::Enter: special = VTERM_KEY_ENTER; break;
    case Key::Escape: special = VTERM_KEY_ESCAPE; break;
    case Key::Tab: special = VTERM_KEY_TAB; break;
    case Key::Backspace: special = VTERM_KEY_BACKSPACE; break;
    case Key::Up: special = VTERM_KEY_UP; break;
    case Key::Down: special = VTERM_KEY_DOWN; break;
    case Key::Left: special = VTERM_KEY_LEFT; break;
    case Key::Right: special = VTERM_KEY_RIGHT; break;
    case Key::Home: special = VTERM_KEY_HOME; break;
    case Key::End: special = VTERM_KEY_END; break;
    case Key::PageUp: special = VTERM_KEY_PAGEUP; break;
    case Key::PageDown: special = VTERM_KEY_PAGEDOWN; break;
    case Key::Insert: special = VTERM_KEY_INS; break;
    case Key::Delete: special = VTERM_KEY_DEL; break;
    default:
        if (key.code >= Key::F1 && key.code <= Key::F12)
            special = VTermKey(VTERM_KEY_FUNCTION(1 + key.code - Key::F1));
        break;
    }
    if (special != VTERM_KEY_NONE)
        vterm_keyboard_key(d->vt, special, VTermModifier(mod));
}

void Shell::draw(Screen &screen, int x, int y) const
{
    if (!d->screen)
        return;
    // Looking back: the first rows are kept lines, the rest the screen's first rows.
    const int back = qMin(m_back, historySize());
    for (int row = 0; row < m_rows; ++row) {
        const QList<VTermScreenCell> *kept = row < back ? &d->history.at(d->history.size() - back + row) : nullptr;
        for (int column = 0; column < m_columns; ++column) {
            VTermScreenCell from;
            if (kept) {
                if (column >= kept->size())
                    break; // the line was shorter (the panel was narrower then)
                from = kept->at(column);
            } else if (!vterm_screen_get_cell(d->screen, VTermPos{row - back, column}, &from)) {
                continue;
            }
            Cell cell;
            // One character a cell: joining marks after the first are left out.
            cell.ch = from.chars[0] >= 32 && from.chars[0] != 0x7f ? char32_t(from.chars[0]) : U' ';
            cell.style.raw = true;
            cell.style.fg = colorIndex(d->screen, from.fg, true);
            cell.style.bg = colorIndex(d->screen, from.bg, false);
            cell.style.bold = from.attrs.bold;
            cell.style.underline = from.attrs.underline != 0;
            cell.style.reverse = from.attrs.reverse;
            screen.set(x + column, y + row, cell);
            if (from.width == 2 && column + 1 < m_columns) {
                Cell tail = cell;
                tail.ch = U' ';
                tail.tail = true;
                screen.set(x + column + 1, y + row, tail);
                ++column;
            }
        }
    }
}

bool Shell::cursor(int *x, int *y) const
{
    if (!d->vt || !d->cursorVisible)
        return false;
    VTermPos pos;
    vterm_state_get_cursorpos(vterm_obtain_state(d->vt), &pos);
    *x = pos.col;
    *y = pos.row + qMin(m_back, historySize()); // pushed down while the panel looks back
    return *y < m_rows;
}

QString Shell::folder() const
{
#if defined(Q_OS_LINUX)
    if (m_pid > 0)
        return QFileInfo(QStringLiteral("/proc/%1/cwd").arg(m_pid)).canonicalFilePath();
#endif
    return {};
}

} // namespace Tui

#else // no libvterm: there is no panel

namespace Tui {

struct Shell::Private
{
};

bool Shell::available()
{
    return false;
}

Shell::Shell(QObject *parent)
    : QObject(parent)
    , d(nullptr)
{
}

Shell::~Shell() = default;

bool Shell::start(const QString &, int, int)
{
    return false;
}

void Shell::resize(int, int) {}
void Shell::readPty() {}
void Shell::flush() {}
void Shell::send(const char *, size_t) {}
void Shell::key(const Key &) {}
void Shell::endQuiet() {}
bool Shell::changeDirectory(const QString &) { return false; }
int Shell::historySize() const { return 0; }
void Shell::scrollBack(int) {}
void Shell::draw(Screen &, int, int) const {}

bool Shell::cursor(int *, int *) const
{
    return false;
}

QString Shell::folder() const
{
    return {};
}

} // namespace Tui

#endif
