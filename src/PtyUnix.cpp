#include "Pty.h"

#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QSocketNotifier>
#include <QVector>

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

struct Pty::Private {
    int fd = -1;
    int pid = -1;
    QSocketNotifier *readNotifier = nullptr;
    QSocketNotifier *writeNotifier = nullptr;
    QByteArray pending; // what the pty couldn't take yet
};

namespace {

QByteArray shellQuote(const QString &path)
{
    QByteArray quoted = QFile::encodeName(path);
    quoted.replace('\'', "'\\''");
    return '\'' + quoted + '\'';
}

} // namespace

Pty::Pty(QObject *parent)
    : QObject(parent)
    , d(new Private)
{
}

Pty::~Pty()
{
    if (d->pid > 0) {
        ::kill(-d->pid, SIGHUP); // the shell's whole session
        ::kill(d->pid, SIGHUP);
    }
    if (d->fd >= 0)
        ::close(d->fd);
    if (d->pid > 0) {
        for (int i = 0; i < 20 && ::waitpid(d->pid, nullptr, WNOHANG) == 0; ++i)
            ::usleep(10000);
        if (::waitpid(d->pid, nullptr, WNOHANG) == 0) {
            ::kill(d->pid, SIGKILL);
            ::waitpid(d->pid, nullptr, 0);
        }
    }
}

bool Pty::start(const QString &directory, int rows, int cols)
{
    if (d->pid > 0)
        return true;

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
    env.insert("TERM_PROGRAM", "argonaut");
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
    size.ws_row = ushort(rows);
    size.ws_col = ushort(cols);
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

    d->pid = int(pid);
    d->fd = master;
    ::fcntl(d->fd, F_SETFL, ::fcntl(d->fd, F_GETFL) | O_NONBLOCK);

    d->readNotifier = new QSocketNotifier(d->fd, QSocketNotifier::Read, this);
    connect(d->readNotifier, &QSocketNotifier::activated, this, [this] {
        QByteArray bytes;
        char buffer[65536];
        bool ended = false;
        while (true) {
            const ssize_t n = ::read(d->fd, buffer, sizeof buffer);
            if (n > 0) {
                bytes.append(buffer, qsizetype(n));
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EINTR))
                break;
            ended = true; // EOF or EIO: the shell is gone
            break;
        }
        if (!bytes.isEmpty())
            emit received(bytes);
        if (!ended)
            return;
        d->readNotifier->setEnabled(false);
        d->writeNotifier->setEnabled(false);
        ::close(d->fd);
        d->fd = -1;
        if (d->pid > 0 && ::waitpid(d->pid, nullptr, WNOHANG) == 0) {
            ::kill(d->pid, SIGHUP);
            ::waitpid(d->pid, nullptr, 0);
        }
        d->pid = -1;
        d->pending.clear();
        emit finished();
    });

    d->writeNotifier = new QSocketNotifier(d->fd, QSocketNotifier::Write, this);
    d->writeNotifier->setEnabled(false);
    connect(d->writeNotifier, &QSocketNotifier::activated, this, [this] { write({}); });
    return true;
}

bool Pty::isRunning() const
{
    return d->pid > 0;
}

void Pty::write(const QByteArray &bytes)
{
    if (d->fd < 0)
        return;
    d->pending += bytes;
    while (!d->pending.isEmpty()) {
        const ssize_t n = ::write(d->fd, d->pending.constData(), size_t(d->pending.size()));
        if (n > 0) {
            d->pending.remove(0, qsizetype(n));
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        break; // EAGAIN: wait until the pty can take more
    }
    if (d->writeNotifier)
        d->writeNotifier->setEnabled(!d->pending.isEmpty());
}

void Pty::resize(int rows, int cols)
{
    if (d->fd < 0)
        return;
    winsize size{};
    size.ws_row = ushort(rows);
    size.ws_col = ushort(cols);
    ::ioctl(d->fd, TIOCSWINSZ, &size);
}

bool Pty::isIdle() const
{
    return d->pid > 0 && d->fd >= 0 && ::tcgetpgrp(d->fd) == d->pid;
}

QString Pty::currentDirectory() const
{
#if defined(Q_OS_LINUX)
    if (d->pid > 0)
        return QFileInfo(QStringLiteral("/proc/%1/cwd").arg(d->pid)).canonicalFilePath();
#endif
    return {};
}

QByteArray Pty::changeDirectoryCommand(const QString &directory) const
{
    // Ctrl+E Ctrl+U clears whatever is typed at the prompt; the leading
    // space keeps the command out of bash's history (HISTCONTROL=ignorespace).
    return "\x05\x15 cd -- " + shellQuote(directory) + "\r";
}
