#pragma once

// The conversation with Argonaut, for the extensions that come with it: a
// JSON object a line, read from standard input and written to standard
// output (docs/extensions.md). Every extension had its own copy of this
// loop; it is here once.

#include <QByteArray>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSocketNotifier>

#include <cerrno>
#include <cstdio>
#include <functional>
#include <unistd.h>

// Says something to Argonaut.
inline void send(const QJsonObject &message)
{
    const QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    std::fwrite(line.constData(), 1, size_t(line.size()), stdout);
    std::fflush(stdout);
}

// Hears what Argonaut says: `handle` gets each message as it comes, and the
// application quits when Argonaut closes the pipe (or is gone). Make one in
// main(), after the application object, and keep it while the program runs.
//
// Several messages can arrive in one go (arrow keys held down in the file
// list ask for one preview after another): `afterAll`, if given, is called
// once after the last of them, for an extension that only answers the
// newest.
class Conversation
{
public:
    explicit Conversation(std::function<void(const QJsonObject &)> handle, std::function<void()> afterAll = {})
        : m_input(STDIN_FILENO, QSocketNotifier::Read)
        , m_handle(std::move(handle))
        , m_afterAll(std::move(afterAll))
    {
        QObject::connect(&m_input, &QSocketNotifier::activated, &m_input, [this] { read(); });
    }

private:
    void read()
    {
        char chunk[65536];
        const ssize_t got = ::read(STDIN_FILENO, chunk, sizeof chunk);
        // A signal came in between (a helper program of ours ended): that is
        // not the pipe closing. The notifier calls again when there is more.
        if (got < 0 && (errno == EINTR || errno == EAGAIN))
            return;
        if (got <= 0) { // Argonaut closed the pipe
            m_input.setEnabled(false);
            QCoreApplication::quit();
            return;
        }
        m_buffer.append(chunk, qsizetype(got));
        qsizetype end;
        while ((end = m_buffer.indexOf('\n')) >= 0) {
            const QJsonObject message = QJsonDocument::fromJson(m_buffer.left(end)).object();
            m_buffer.remove(0, end + 1);
            m_handle(message);
        }
        if (m_afterAll)
            m_afterAll();
    }

    QSocketNotifier m_input;
    QByteArray m_buffer;
    std::function<void(const QJsonObject &)> m_handle;
    std::function<void()> m_afterAll;
};
