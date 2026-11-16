#pragma once

#include "Screen.h"

#include <QByteArray>
#include <QObject>
#include <QString>

class QSocketNotifier;

namespace Tui {

struct Key;

// A shell inside the terminal interface: the panel under the file panes.
// It runs $SHELL on a pseudo-terminal and keeps what the shell draws as a
// grid of cells (libvterm does the work of a terminal), which draw() copies
// into the interface's Screen. Keys are handed to it as the interface's own
// Key and reach the shell as the bytes a terminal would send.
//
// Without libvterm (available() is false) it can't be started; the
// interface then hands the whole terminal to a shell instead.
class Shell : public QObject
{
    Q_OBJECT

public:
    explicit Shell(QObject *parent = nullptr);
    ~Shell() override;

    static bool available();

    bool start(const QString &folder, int rows, int columns);
    bool isRunning() const { return m_pid > 0; }
    void resize(int rows, int columns);
    int rows() const { return m_rows; }
    int columns() const { return m_columns; }

    void key(const Key &key);
    // Its cells, with their own colours, at (x, y) of `screen`.
    void draw(Screen &screen, int x, int y) const;
    // Where its text cursor is, in its own cells; false while it is hidden.
    bool cursor(int *x, int *y) const;
    // The folder the shell is in (Linux); empty when it can't be told.
    QString folder() const;

signals:
    void changed();  // something new to draw
    void finished(); // the shell has ended

private:
    void readPty();
    void flush();
    void send(const char *bytes, size_t length);

    struct Private;
    Private *d;
    int m_pid = 0;
    int m_fd = -1;
    int m_rows = 0;
    int m_columns = 0;
    QByteArray m_pending; // typed, not yet taken by the shell
    QSocketNotifier *m_readNotifier = nullptr;
    QSocketNotifier *m_writeNotifier = nullptr;
};

} // namespace Tui
