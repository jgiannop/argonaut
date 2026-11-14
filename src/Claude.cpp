#include "Claude.h"

#include "KeyStore.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

namespace {

constexpr const char *kApiVersion = "2026-10-21";
constexpr const char *kFallbackBeta = "server-side-fallback-2026-07-01";
constexpr int kMaxTokens = 16000; // non-streaming: keeps responses under HTTP timeouts
constexpr int kCliTimeoutMs = 10 * 60 * 1000;

QJsonObject textBlock(const QString &text)
{
    return {{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), text}};
}

// Text, image and document blocks as they are (without cache markers); a
// plain string becomes one text block. Anything else is left out.
void appendContent(QJsonArray *out, const QJsonValue &content)
{
    if (content.isString()) {
        if (!content.toString().isEmpty())
            out->append(textBlock(content.toString()));
        return;
    }
    for (const QJsonValue &v : content.toArray()) {
        const QJsonObject b = v.toObject();
        const QString type = b.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("text"))
            out->append(textBlock(b.value(QStringLiteral("text")).toString()));
        else if (type == QLatin1String("image") || type == QLatin1String("document"))
            out->append(QJsonObject{{QStringLiteral("type"), type}, {QStringLiteral("source"), b.value(QStringLiteral("source"))}});
    }
}

// Claude Code takes one user message, not a Messages history with our own
// tools: the conversation so far is written out as that one message. Earlier
// tool calls become "you chose…", their results follow as content.
QJsonArray flattenForCli(const QJsonArray &messages)
{
    QJsonArray out;
    QHash<QString, QString> toolNames; // tool_use id -> name
    bool history = false;
    for (const QJsonValue &mv : messages) {
        const QJsonObject m = mv.toObject();
        const QJsonValue content = m.value(QStringLiteral("content"));
        if (m.value(QStringLiteral("role")).toString() == QLatin1String("assistant")) {
            history = true;
            if (content.isString()) {
                out.append(textBlock(QStringLiteral("Earlier you said:\n") + content.toString()));
                continue;
            }
            for (const QJsonValue &v : content.toArray()) {
                const QJsonObject b = v.toObject();
                const QString type = b.value(QStringLiteral("type")).toString();
                if (type == QLatin1String("text") && !b.value(QStringLiteral("text")).toString().trimmed().isEmpty()) {
                    out.append(textBlock(QStringLiteral("Earlier you said:\n") + b.value(QStringLiteral("text")).toString()));
                } else if (type == QLatin1String("tool_use")) {
                    const QString name = b.value(QStringLiteral("name")).toString();
                    toolNames.insert(b.value(QStringLiteral("id")).toString(), name);
                    out.append(textBlock(QStringLiteral("Earlier you chose the action \"%1\" with this input:\n%2")
                                             .arg(name, QString::fromUtf8(QJsonDocument(b.value(QStringLiteral("input")).toObject())
                                                                              .toJson(QJsonDocument::Compact)))));
                }
            }
            continue;
        }
        if (content.isString()) {
            appendContent(&out, content);
            continue;
        }
        for (const QJsonValue &v : content.toArray()) {
            const QJsonObject b = v.toObject();
            if (b.value(QStringLiteral("type")).toString() == QLatin1String("tool_result")) {
                const QString name = toolNames.value(b.value(QStringLiteral("tool_use_id")).toString());
                out.append(textBlock(QStringLiteral("Result of \"%1\":").arg(name)));
                appendContent(&out, b.value(QStringLiteral("content")));
            } else {
                appendContent(&out, QJsonArray{b});
            }
        }
    }
    if (history)
        out.append(textBlock(QStringLiteral("Now give your next answer.")));
    return out;
}

QString systemText(const QJsonValue &system)
{
    if (system.isString())
        return system.toString();
    QStringList parts;
    for (const QJsonValue &v : system.toArray())
        parts << v.toObject().value(QStringLiteral("text")).toString();
    return parts.join(QStringLiteral("\n\n"));
}

} // namespace

namespace Claude {

// --------------------------------------------------------------- connection

Connection connection()
{
    return QSettings().value(QStringLiteral("claude/connection")).toString() == QLatin1String("subscription")
               ? Connection::Subscription
               : Connection::ApiKey;
}

void setConnection(Connection c)
{
    QSettings().setValue(QStringLiteral("claude/connection"),
                         c == Connection::Subscription ? QStringLiteral("subscription") : QStringLiteral("api"));
}

QString cliProgram()
{
    const QString overridden = qEnvironmentVariable("ARGONAUT_CLAUDE_PROGRAM"); // tests
    if (!overridden.isEmpty())
        return overridden;
    const QString inPath = QStandardPaths::findExecutable(QStringLiteral("claude"));
    if (!inPath.isEmpty())
        return inPath;
    // Started from the desktop, PATH often lacks the folders its installers use.
    const QString home = QDir::homePath();
    return QStandardPaths::findExecutable(QStringLiteral("claude"),
                                          {home + QStringLiteral("/.local/bin"), home + QStringLiteral("/.claude/local"),
                                           home + QStringLiteral("/.npm-global/bin"), QStringLiteral("/usr/local/bin"),
                                           QStringLiteral("/opt/homebrew/bin")});
}

Login parseLogin(const QByteArray &authStatusJson)
{
    Login login;
    login.installed = true;
    QJsonParseError parse;
    const QJsonObject o = QJsonDocument::fromJson(authStatusJson, &parse).object();
    if (parse.error != QJsonParseError::NoError || !o.contains(QStringLiteral("loggedIn"))) {
        login.message = QObject::tr("Couldn't read Claude Code's sign-in status.");
        return login;
    }
    login.loggedIn = o.value(QStringLiteral("loggedIn")).toBool();
    login.plan = o.value(QStringLiteral("subscriptionType")).toString();
    login.subscription = login.loggedIn && o.value(QStringLiteral("authMethod")).toString() == QLatin1String("claude.ai");
    if (!login.loggedIn) {
        login.message = QObject::tr("Claude Code isn't signed in. Run “claude” in a terminal and sign in with your Claude account.");
    } else if (!login.subscription) {
        login.message = QObject::tr("Claude Code is signed in with an API account, not a Claude plan: "
                                    "what Argonaut asks is billed to that account.");
    } else if (login.plan.isEmpty()) {
        login.message = QObject::tr("Signed in to Claude Code with your Claude subscription.");
    } else {
        QString plan = login.plan;
        plan[0] = plan.at(0).toUpper();
        login.message = QObject::tr("Signed in to Claude Code with your Claude %1 plan.").arg(plan);
    }
    return login;
}

// ---------------------------------------------------------------- settings

Mode mode()
{
    const QString v = QSettings().value(QStringLiteral("claude/mode"), QStringLiteral("ask")).toString();
    if (v == QLatin1String("names"))
        return Mode::NamesOnly;
    if (v == QLatin1String("auto"))
        return Mode::Auto;
    return Mode::Ask;
}

void setMode(Mode m)
{
    QSettings().setValue(QStringLiteral("claude/mode"),
                         m == Mode::NamesOnly ? QStringLiteral("names") : m == Mode::Auto ? QStringLiteral("auto") : QStringLiteral("ask"));
}

QString modeName(Mode m)
{
    switch (m) {
    case Mode::NamesOnly:
        return QObject::tr("Names and details only");
    case Mode::Ask:
        return QObject::tr("Ask before reading files");
    case Mode::Auto:
        return QObject::tr("Read files when needed");
    }
    return {};
}

const QList<ModelInfo> &models()
{
    // Prices per million tokens (Haiku: prompts up to 100K tokens).
    static const QList<ModelInfo> list = {
        {QStringLiteral("claude-opus-5-5"), QStringLiteral("Claude Opus 5.5"), 4.0, 20.0, true},
        {QStringLiteral("claude-sonnet-5-5"), QStringLiteral("Claude Sonnet 5.5"), 2.0, 10.0, true},
        {QStringLiteral("claude-haiku-5-5"), QStringLiteral("Claude Haiku 5.5"), 0.10, 0.50, false},
    };
    return list;
}

ModelInfo modelInfo(const QString &id)
{
    for (const ModelInfo &m : models()) {
        if (m.id == id)
            return m;
    }
    return models().first();
}

QString model()
{
    return modelInfo(QSettings().value(QStringLiteral("claude/model")).toString()).id;
}

void setModel(const QString &id)
{
    QSettings().setValue(QStringLiteral("claude/model"), modelInfo(id).id);
}

double cost(const QString &modelId, qint64 inputTokens, qint64 outputTokens)
{
    const ModelInfo m = modelInfo(modelId);
    double in = m.inputPerMTok;
    double out = m.outputPerMTok;
    if (m.id == QLatin1String("claude-haiku-5-5") && inputTokens > 100000) {
        in = 0.50; // longer prompts cost more on Haiku
        out = 2.50;
    }
    return (double(inputTokens) * in + double(outputTokens) * out) / 1e6;
}

double spent()
{
    return QSettings().value(QStringLiteral("claude/spent"), 0.0).toDouble();
}

void addSpent(double dollars)
{
    QSettings().setValue(QStringLiteral("claude/spent"), spent() + dollars);
}

void resetSpent()
{
    QSettings().setValue(QStringLiteral("claude/spent"), 0.0);
}

// ------------------------------------------------------------------ client

Client::Client(QObject *parent) : QObject(parent), m_net(new QNetworkAccessManager(this)) {}

Connection Client::connection() const
{
    return m_connection < 0 ? Claude::connection() : Connection(m_connection);
}

bool Client::ready() const
{
    return connection() == Connection::Subscription ? !cliProgram().isEmpty() : !apiKey().isEmpty();
}

QString Client::apiKey() const
{
    return m_key.isEmpty() ? KeyStore::load() : m_key;
}

QString Client::baseUrl()
{
    const QString overridden = qEnvironmentVariable("ARGONAUT_CLAUDE_BASE_URL");
    return overridden.isEmpty() ? QStringLiteral("https://api.anthropic.com") : overridden;
}

Error Client::parseError(int status, const QByteArray &body, const QString &networkError)
{
    Error e;
    e.status = status;
    const QJsonObject root = QJsonDocument::fromJson(body).object();
    const QJsonObject err = root.value(QStringLiteral("error")).toObject();
    e.type = err.value(QStringLiteral("type")).toString();
    e.requestId = root.value(QStringLiteral("request_id")).toString();
    const QString detail = err.value(QStringLiteral("message")).toString();
    e.retryable = status == 429 || status == 500 || status == 529 || status == 503 || status == 502 || status == 0;
    switch (status) {
    case 0:
        e.message = QObject::tr("Couldn't reach the Claude API (%1)").arg(networkError);
        break;
    case 401:
        e.message = QObject::tr("The API key isn't valid. Check it in Preferences.");
        break;
    case 402:
        e.message = QObject::tr("Billing problem on your Anthropic account: %1").arg(detail);
        break;
    case 403:
        e.message = QObject::tr("This API key isn't allowed to do that: %1").arg(detail);
        break;
    case 404:
        e.message = QObject::tr("Not available for this account: %1").arg(detail);
        break;
    case 413:
        e.message = QObject::tr("The request is too large for Claude.");
        break;
    case 429:
        e.message = QObject::tr("Too many requests for now. Try again in a minute.");
        break;
    case 529:
        e.message = QObject::tr("Claude is overloaded right now. Try again shortly.");
        break;
    default:
        e.message = detail.isEmpty() ? QObject::tr("The Claude API answered with error %1").arg(status) : detail;
        break;
    }
    return e;
}

QNetworkReply *Client::post(const QString &path, const QJsonObject &body, bool beta)
{
    QNetworkRequest request(QUrl(baseUrl() + path));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("x-api-key", apiKey().toUtf8());
    request.setRawHeader("anthropic-version", kApiVersion);
    if (beta)
        request.setRawHeader("anthropic-beta", kFallbackBeta);
    request.setTransferTimeout(10 * 60 * 1000);
    return m_net->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
}

void Client::testKey(const QString &key, std::function<void(bool, const QString &)> done)
{
    QNetworkRequest request(QUrl(baseUrl() + QStringLiteral("/v1/models?limit=1")));
    request.setRawHeader("x-api-key", key.trimmed().toUtf8());
    request.setRawHeader("anthropic-version", kApiVersion);
    request.setTransferTimeout(30000);
    QNetworkReply *reply = m_net->get(request);
    connect(reply, &QNetworkReply::finished, this, [reply, done] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 200) {
            done(true, QObject::tr("The key works."));
            return;
        }
        const Error e = parseError(status, reply->readAll(),
                                   reply->error() == QNetworkReply::NoError ? QString() : reply->errorString());
        done(false, e.message);
    });
}

void Client::countTokens(QJsonObject request, std::function<void(qint64, const Error &)> done)
{
    if (connection() == Connection::Subscription) {
        Error e;
        e.message = QObject::tr("Token counts need an API key.");
        QTimer::singleShot(0, this, [done, e] { done(0, e); });
        return;
    }
    if (!request.contains(QStringLiteral("model")))
        request.insert(QStringLiteral("model"), model());
    request.remove(QStringLiteral("max_tokens"));
    request.remove(QStringLiteral("fallbacks"));
    QNetworkReply *reply = post(QStringLiteral("/v1/messages/count_tokens"), request, false);
    connect(reply, &QNetworkReply::finished, this, [reply, done] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        if (status == 200) {
            done(QJsonDocument::fromJson(body).object().value(QStringLiteral("input_tokens")).toInteger(), Error());
            return;
        }
        done(0, parseError(status, body, reply->error() == QNetworkReply::NoError ? QString() : reply->errorString()));
    });
}

void Client::send(QJsonObject request, std::function<void(const Response &, const Error &)> done)
{
    if (!request.contains(QStringLiteral("model")))
        request.insert(QStringLiteral("model"), model());
    if (connection() == Connection::Subscription) {
        sendThroughCli(request, std::move(done));
        return;
    }
    if (!request.contains(QStringLiteral("max_tokens")))
        request.insert(QStringLiteral("max_tokens"), kMaxTokens);
    // If a safety filter declines, the API retries on another model itself.
    if (modelInfo(request.value(QStringLiteral("model")).toString()).fallbacks && !request.contains(QStringLiteral("fallbacks")))
        request.insert(QStringLiteral("fallbacks"), QStringLiteral("default"));
    sendAttempt(request, 0, std::move(done));
}

void Client::sendAttempt(QJsonObject request, int attempt, std::function<void(const Response &, const Error &)> done)
{
    const bool beta = request.contains(QStringLiteral("fallbacks"));
    QNetworkReply *reply = post(QStringLiteral("/v1/messages"), request, beta);
    connect(reply, &QNetworkReply::finished, this, [this, reply, request, attempt, done] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        if (status != 200) {
            Error e = parseError(status, body, reply->error() == QNetworkReply::NoError ? QString() : reply->errorString());
            if (e.retryable && attempt < 2) {
                // Wait as asked (retry-after), or 1 s, then 4 s.
                int wait = attempt == 0 ? 1000 : 4000;
                const QByteArray after = reply->rawHeader("retry-after");
                if (!after.isEmpty())
                    wait = qBound(1000, after.toInt() * 1000, 60000);
                QTimer::singleShot(wait, this, [this, request, attempt, done] { sendAttempt(request, attempt + 1, done); });
                return;
            }
            done(Response(), e);
            return;
        }
        Response r;
        r.message = QJsonDocument::fromJson(body).object();
        r.stopReason = r.message.value(QStringLiteral("stop_reason")).toString();
        const QJsonObject usage = r.message.value(QStringLiteral("usage")).toObject();
        r.inputTokens = usage.value(QStringLiteral("input_tokens")).toInteger()
                        + usage.value(QStringLiteral("cache_creation_input_tokens")).toInteger()
                        + usage.value(QStringLiteral("cache_read_input_tokens")).toInteger();
        r.outputTokens = usage.value(QStringLiteral("output_tokens")).toInteger();
        const QString servedBy = r.message.value(QStringLiteral("model")).toString(request.value(QStringLiteral("model")).toString());
        r.dollars = cost(servedBy, r.inputTokens, r.outputTokens);
        addSpent(r.dollars);
        QStringList texts;
        for (const QJsonValue &block : r.message.value(QStringLiteral("content")).toArray()) {
            const QJsonObject b = block.toObject();
            if (b.value(QStringLiteral("type")).toString() == QLatin1String("text"))
                texts << b.value(QStringLiteral("text")).toString();
        }
        r.text = texts.join(QString());
        if (r.stopReason == QLatin1String("refusal")) {
            Error e;
            e.status = 200;
            e.type = QStringLiteral("refusal");
            e.message = QObject::tr("Claude declined this request.");
            done(r, e);
            return;
        }
        done(r, Error());
    });
}

// ------------------------------------------------ through Claude Code

// Claude Code runs with none of its own tools, settings, hooks, skills or MCP
// servers, and with our system prompt instead of its own: it only relays the
// request. Our tools become a structured output: {"tool": name, name: input}.
QStringList Client::cliArguments(const QJsonObject &request)
{
    QString system = systemText(request.value(QStringLiteral("system")));
    QJsonArray names;
    QJsonObject properties;
    QStringList described;
    for (const QJsonValue &v : request.value(QStringLiteral("tools")).toArray()) {
        const QJsonObject tool = v.toObject();
        const QString name = tool.value(QStringLiteral("name")).toString();
        if (name.isEmpty() || !tool.contains(QStringLiteral("input_schema")))
            continue;
        names.append(name);
        properties.insert(name, tool.value(QStringLiteral("input_schema")));
        described << QStringLiteral("- %1: %2").arg(name, tool.value(QStringLiteral("description")).toString());
    }
    if (!names.isEmpty()) {
        system += QStringLiteral("\n\nYou have no tools in this conversation. Give every answer through the structured "
                                 "output: set \"tool\" to one of the actions below, and put that action's input in the "
                                 "field with the same name.\n\n")
                  + described.join(QLatin1Char('\n'));
    }
    if (system.trimmed().isEmpty())
        system = QStringLiteral("You are Claude, helping inside the Argonaut file manager.");

    QStringList args = {QStringLiteral("-p"), QStringLiteral("--safe-mode"), QStringLiteral("--tools"), QString(),
                        QStringLiteral("--disable-slash-commands"), QStringLiteral("--strict-mcp-config"),
                        QStringLiteral("--no-session-persistence"), QStringLiteral("--input-format"), QStringLiteral("stream-json"),
                        QStringLiteral("--output-format"), QStringLiteral("stream-json"), QStringLiteral("--verbose"),
                        QStringLiteral("--model"), request.value(QStringLiteral("model")).toString(model()),
                        QStringLiteral("--system-prompt"), system};
    if (!names.isEmpty()) {
        properties.insert(QStringLiteral("tool"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("enum"), names}});
        const QJsonObject schema{{QStringLiteral("type"), QStringLiteral("object")},
                                 {QStringLiteral("properties"), properties},
                                 {QStringLiteral("required"), QJsonArray{QStringLiteral("tool")}},
                                 {QStringLiteral("additionalProperties"), false}};
        args << QStringLiteral("--json-schema") << QString::fromUtf8(QJsonDocument(schema).toJson(QJsonDocument::Compact));
    }
    return args;
}

QByteArray Client::cliInput(const QJsonObject &request)
{
    const QJsonObject message{{QStringLiteral("role"), QStringLiteral("user")},
                              {QStringLiteral("content"), flattenForCli(request.value(QStringLiteral("messages")).toArray())}};
    return QJsonDocument(QJsonObject{{QStringLiteral("type"), QStringLiteral("user")}, {QStringLiteral("message"), message}})
               .toJson(QJsonDocument::Compact)
           + '\n';
}

Response Client::parseCliOutput(const QJsonObject &request, const QByteArray &output, Error *error)
{
    Response r;
    QJsonObject result; // the last "result" line
    for (const QByteArray &line : output.split('\n')) {
        const QJsonObject o = QJsonDocument::fromJson(line).object();
        if (o.value(QStringLiteral("type")).toString() == QLatin1String("result"))
            result = o;
    }
    if (result.isEmpty()) {
        error->message = QObject::tr("Claude Code gave no answer.");
        return r;
    }
    const QString text = result.value(QStringLiteral("result")).toString();
    if (result.value(QStringLiteral("is_error")).toBool() || result.value(QStringLiteral("subtype")).toString() != QLatin1String("success")) {
        error->status = result.value(QStringLiteral("api_error_status")).toInt();
        error->type = result.value(QStringLiteral("subtype")).toString();
        if (text.contains(QLatin1String("/login")) || text.contains(QLatin1String("logged in"), Qt::CaseInsensitive))
            error->message = QObject::tr("Claude Code isn't signed in. Run “claude” in a terminal and sign in with your Claude account.");
        else
            error->message = text.isEmpty() ? QObject::tr("Claude Code couldn't finish the request.") : QObject::tr("Claude Code: %1").arg(text);
        return r;
    }
    const QJsonObject usage = result.value(QStringLiteral("usage")).toObject();
    r.inputTokens = usage.value(QStringLiteral("input_tokens")).toInteger()
                    + usage.value(QStringLiteral("cache_creation_input_tokens")).toInteger()
                    + usage.value(QStringLiteral("cache_read_input_tokens")).toInteger();
    r.outputTokens = usage.value(QStringLiteral("output_tokens")).toInteger();
    r.stopReason = result.value(QStringLiteral("stop_reason")).toString();
    if (r.stopReason == QLatin1String("refusal")) {
        error->status = 200;
        error->type = QStringLiteral("refusal");
        error->message = QObject::tr("Claude declined this request.");
        return r;
    }

    // Back into the shape of a Messages response, so callers needn't care.
    QJsonArray content;
    if (request.value(QStringLiteral("tools")).toArray().isEmpty()) {
        r.text = text;
        r.stopReason = QStringLiteral("end_turn");
        content.append(textBlock(text));
    } else {
        const QJsonObject structured = result.value(QStringLiteral("structured_output")).toObject();
        const QString tool = structured.value(QStringLiteral("tool")).toString();
        bool known = false;
        for (const QJsonValue &v : request.value(QStringLiteral("tools")).toArray())
            known = known || v.toObject().value(QStringLiteral("name")).toString() == tool;
        if (tool.isEmpty() || !known || !structured.value(tool).isObject()) {
            error->message = QObject::tr("Claude's answer wasn't in the expected form. Try again.");
            return r;
        }
        r.stopReason = QStringLiteral("tool_use");
        content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("tool_use")},
                                   {QStringLiteral("id"), QStringLiteral("cli_") + result.value(QStringLiteral("uuid")).toString()},
                                   {QStringLiteral("name"), tool},
                                   {QStringLiteral("input"), structured.value(tool)}});
    }
    r.message = QJsonObject{{QStringLiteral("type"), QStringLiteral("message")},
                            {QStringLiteral("role"), QStringLiteral("assistant")},
                            {QStringLiteral("model"), request.value(QStringLiteral("model"))},
                            {QStringLiteral("content"), content},
                            {QStringLiteral("stop_reason"), r.stopReason}};
    return r;
}

QProcess *Client::startCli(const QStringList &arguments)
{
    auto *process = new QProcess(this);
    // Without these, Claude Code would use (and bill) an API key instead of the plan.
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.remove(QStringLiteral("ANTHROPIC_API_KEY"));
    env.remove(QStringLiteral("ANTHROPIC_AUTH_TOKEN"));
    process->setProcessEnvironment(env);
    // Not the folder being organized: nothing there is read by Claude Code.
    process->setWorkingDirectory(QDir::tempPath());
    process->start(cliProgram(), arguments);
    return process;
}

void Client::sendThroughCli(const QJsonObject &request, std::function<void(const Response &, const Error &)> done)
{
    if (cliProgram().isEmpty()) {
        Error e;
        e.message = QObject::tr("Claude Code isn't installed. Install it, sign in with your Claude account, then try again.");
        QTimer::singleShot(0, this, [done, e] { done(Response(), e); });
        return;
    }
    QProcess *process = startCli(cliArguments(request));
    auto *timeout = new QTimer(process);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, process, [process] {
        process->setProperty("timedOut", true);
        process->kill();
    });
    timeout->start(kCliTimeoutMs);
    connect(process, &QProcess::errorOccurred, this, [process, done](QProcess::ProcessError failure) {
        if (failure != QProcess::FailedToStart)
            return; // the others end in finished()
        process->deleteLater();
        Error e;
        e.message = QObject::tr("Couldn't start Claude Code (%1).").arg(process->errorString());
        done(Response(), e);
    });
    connect(process, &QProcess::finished, this, [process, request, done](int code, QProcess::ExitStatus) {
        process->deleteLater();
        Error e;
        Response r;
        if (process->property("timedOut").toBool()) {
            e.message = QObject::tr("Claude Code took too long to answer.");
        } else {
            r = parseCliOutput(request, process->readAllStandardOutput(), &e);
            // No result line: say what it printed instead (a crash, a bad option…).
            const QString said = QString::fromUtf8(process->readAllStandardError()).trimmed();
            if (e.message.isEmpty() && code != 0)
                e.message = QObject::tr("Claude Code couldn't finish the request.");
            if (!e.message.isEmpty() && e.type.isEmpty() && !said.isEmpty())
                e.message = QObject::tr("Claude Code: %1").arg(said.left(300));
        }
        done(r, e);
    });
    process->write(cliInput(request));
    process->closeWriteChannel();
}

void Client::checkLogin(std::function<void(const Login &)> done)
{
    if (cliProgram().isEmpty()) {
        Login login;
        login.message = QObject::tr("Claude Code isn't installed on this computer.");
        QTimer::singleShot(0, this, [done, login] { done(login); });
        return;
    }
    QProcess *process = startCli({QStringLiteral("auth"), QStringLiteral("status"), QStringLiteral("--json")});
    process->closeWriteChannel();
    QTimer::singleShot(30000, process, [process] { process->kill(); });
    connect(process, &QProcess::errorOccurred, this, [process, done](QProcess::ProcessError failure) {
        if (failure != QProcess::FailedToStart)
            return;
        process->deleteLater();
        Login login;
        login.message = QObject::tr("Couldn't start Claude Code (%1).").arg(process->errorString());
        done(login);
    });
    connect(process, &QProcess::finished, this, [process, done] {
        process->deleteLater();
        done(parseLogin(process->readAllStandardOutput()));
    });
}

} // namespace Claude
