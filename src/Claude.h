#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

// Calls to the Claude API (core code, no widgets). Argonaut is C++ and there
// is no official C++ SDK, so this is the HTTP API directly:
// POST /v1/messages, POST /v1/messages/count_tokens, GET /v1/models.
namespace Claude {

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
    double dollars = 0;
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

    // Lists models: checks a key without spending anything.
    void testKey(const QString &key, std::function<void(bool ok, const QString &message)> done);
    // Counts a request's input tokens (the body as for /v1/messages, without max_tokens).
    void countTokens(QJsonObject request, std::function<void(qint64 tokens, const Error &error)> done);
    // Sends a Messages request. Model, max_tokens and fallbacks are filled in
    // when missing; 429/5xx/529 are retried twice. Spending is recorded.
    void send(QJsonObject request, std::function<void(const Response &response, const Error &error)> done);

    // Turns an HTTP error body into a message for people (exposed for tests).
    static Error parseError(int status, const QByteArray &body, const QString &networkError);

private:
    QNetworkReply *post(const QString &path, const QJsonObject &body, bool beta);
    void sendAttempt(QJsonObject request, int attempt,
                     std::function<void(const Response &response, const Error &error)> done);

    QNetworkAccessManager *m_net;
    QString m_key;
};

} // namespace Claude
