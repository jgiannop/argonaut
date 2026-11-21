#pragma once

#include <QObject>
#include <QString>

#include <memory>

// The user's shell on a pseudo-terminal: what TerminalWidget types into and
// reads from. Two versions behind this one interface:
//   PtyUnix.cpp     forkpty(); $SHELL, else /bin/sh
//   PtyWindows.cpp  ConPTY (CreatePseudoConsole, Windows 10 1809 and later);
//                   $SHELL if it names a program, else pwsh, PowerShell, cmd
// What comes out is the terminal's byte stream (UTF-8 with escape
// sequences), on both: libvterm reads it the same way.
class Pty : public QObject
{
    Q_OBJECT

public:
    explicit Pty(QObject *parent = nullptr);
    ~Pty() override; // ends the shell

    // Starts the shell in `directory` with a screen of `rows` × `cols`.
    bool start(const QString &directory, int rows, int cols);
    bool isRunning() const;
    void write(const QByteArray &bytes);
    void resize(int rows, int cols);

    // No program is running in the foreground: what is typed goes to the
    // shell's prompt. Unix: the terminal's foreground group is the shell's.
    // Windows: the shell has no child process.
    bool isIdle() const;
    // The shell's folder where the system can say (Linux: /proc); else empty,
    // and the shell has to say it itself (TerminalWidget reads OSC 7 / 9;9
    // and, on Windows, the title the prompt sets).
    QString currentDirectory() const;
    // What to type for the shell to go to `directory`, Enter included.
    QByteArray changeDirectoryCommand(const QString &directory) const;

signals:
    void received(const QByteArray &bytes);
    void finished(); // the shell has ended

private:
    struct Private;
    std::unique_ptr<Private> d;
};
