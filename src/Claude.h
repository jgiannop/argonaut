#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QProcess;

// Calls to Claude (core code, no widgets), in one of two ways:
// - an API key: Argonaut is C++ and there is no official C++ SDK, so this is
//   the HTTP API directly (POST /v1/messages, POST /v1/messages/count_tokens,
//   GET /v1/models);
// - a Claude subscription: through the Claude Code program (`claude -p`) that
//   the user installed and signed in to. Argonaut never sees its login.
namespace Claude {

// --------------------------------------------------------------- connection

// How Argonaut reaches Claude (Preferences → Claude, "claude/connection").
enum class Connection {
    ApiKey,       // an Anthropic API key, billed per token (default)
    Subscription, // the user's Claude plan, through Claude Code
};
Connection connection();
void setConnection(Connection connection);

// The Claude Code program: ARGONAUT_CLAUDE_PROGRAM (tests), else `claude`
// from PATH or the folders its installers use. Empty if not installed.
QString cliProgram();

// What `claude auth status` says (exposed for tests).
struct Login
{
    bool installed = false;
    bool loggedIn = false;
    bool subscription = false; // signed in with a Claude plan, not an API key
    QString plan;              // "max", "pro"…
    QString message;           // for people
};
Login parseLogin(const QByteArray &authStatusJson);

// ---------------------------------------------------------------- settings

// What may be sent (Preferences → Claude, "claude/mode").
enum class Mode {
    NamesOnly, // names, sizes, dates, types: never contents
    Ask,       // Claude may ask to read the start of some files; you approve (default)
    Auto,      // the same, without asking
};
Mode mode();
void setMode(Mode mode);
QString modeName(Mode mode);

struct ModelInfo
{
    QString id;   // "claude-opus-5-5"
    QString name; // "Claude Opus 5.5"
    double inputPerMTok;
    double outputPerMTok;
    bool fallbacks; // the API's server-side fallback is offered for it
};
const QList<ModelInfo> &models();
ModelInfo modelInfo(const QString &id); // the default model if unknown
QString model();                        // "claude/model", default claude-opus-5-5
void setModel(const QString &id);

// Dollars for a request's tokens on a model.
double cost(const QString &modelId, qint64 inputTokens, qint64 outputTokens);
// What Argonaut has spent (from the usage of every response), and reset.
double spent();
void addSpent(double dollars);
void resetSpent();

// ------------------------------------------------------------------ client

struct Error
{
    int status = 0;        // HTTP status, 0 for network errors
    QString type;          // "authentication_error"…
    QString message;       // shown to the user
    QString requestId;
    bool retryable = false;
};

struct Response
{
    QJsonObject message; // the whole response body
    QString text;        // all text blocks joined
    QString stopReason;
    qint64 inputTokens = 0;
    qint64 outputTokens = 0;
    double dollars = 0; // 0 with a subscription: it counts toward the plan instead
};

class Client : public QObject
{
    Q_OBJECT

public:
    explicit Client(QObject *parent = nullptr);

    // The key to use (KeyStore by default); tests set one directly.
    void setApiKey(const QString &key) { m_key = key; }
    QString apiKey() const;
    // https://api.anthropic.com, or ARGONAUT_CLAUDE_BASE_URL (tests).
    static QString baseUrl();

    // The connection to use (the setting by default); tests set one directly.
    void setConnection(Connection connection) { m_connection = int(connection); }
    Connection connection() const;
    // True when send() can work: a key is saved, or Claude Code is installed.
    bool ready() const;
    // Asks Claude Code whether it is signed in (`claude auth status`): free.
    void checkLogin(std::function<void(const Login &login)> done);

    // Lists models: checks a key without spending anything.
    void testKey(const QString &key, std::function<void(bool ok, const QString &message)> done);
    // Counts a request's input tokens (the body as for /v1/messages, without max_tokens).
    void countTokens(QJsonObject request, std::function<void(qint64 tokens, const Error &error)> done);
    // Sends a Messages request. Model, max_tokens and fallbacks are filled in
    // when missing; 429/5xx/529 are retried twice. Spending is recorded.
    // With a subscription the same request goes through Claude Code, and the
    // answer comes back in the same shape (see sendThroughCli).
    void send(QJsonObject request, std::function<void(const Response &response, const Error &error)> done);

    // Turns an HTTP error body into a message for people (exposed for tests).
    static Error parseError(int status, const QByteArray &body, const QString &networkError);

    // A Messages request as Claude Code takes it (exposed for tests): the
    // command-line arguments, and the one user message written to its stdin.
    static QStringList cliArguments(const QJsonObject &request);
    static QByteArray cliInput(const QJsonObject &request);
    // Claude Code's output (stream-json lines) as a Messages response.
    static Response parseCliOutput(const QJsonObject &request, const QByteArray &output, Error *error);

private:
    QNetworkReply *post(const QString &path, const QJsonObject &body, bool beta);
    void sendAttempt(QJsonObject request, int attempt,
                     std::function<void(const Response &response, const Error &error)> done);

    void sendThroughCli(const QJsonObject &request, std::function<void(const Response &response, const Error &error)> done);
    QProcess *startCli(const QStringList &arguments);

    QNetworkAccessManager *m_net;
    QString m_key;
    int m_connection = -1; // -1: follow the setting
};

} // namespace Claude
