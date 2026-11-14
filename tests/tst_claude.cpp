// Tests for the Claude API client (against a fake server on localhost,
// ARGONAUT_CLAUDE_BASE_URL), the subscription connection (a fake Claude Code
// program, ARGONAUT_CLAUDE_PROGRAM) and the key store's file mode. No real
// API calls, and the real Claude Code is never started.

#include "Claude.h"
#include "KeyStore.h"
#include "FakeApi.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

namespace {

QByteArray message(const QString &text, const QString &stop = "end_turn", int in = 1200, int out = 300,
                   const QString &model = "claude-opus-5-5")
{
    QJsonObject m{{"type", "message"}, {"model", model}, {"stop_reason", stop},
                  {"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", text}}}},
                  {"usage", QJsonObject{{"input_tokens", in}, {"output_tokens", out}}}};
    return QJsonDocument(m).toJson(QJsonDocument::Compact);
}

} // namespace

class TestClaude : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void sendsAMessage();
    void retriesWhenOverloaded();
    void errorsForPeople();
    void testKey();
    void countTokens();
    void refusal();
    void costs();
    void keyStoreFile();
    void environmentKeyWins();
    void subscriptionArguments();
    void subscriptionSendsThroughClaudeCode();
    void subscriptionErrors();
    void subscriptionLogin();

private:
    // A fake `claude`: records its arguments, stdin and ANTHROPIC_API_KEY,
    // prints `reply` (or `auth` for "auth status") and exits with `exitCode`.
    QString fakeClaude(const QByteArray &reply, const QByteArray &auth = "{}", int exitCode = 0);
    QString read(const QString &name) const;
    QTemporaryDir m_cli;
};

QString TestClaude::fakeClaude(const QByteArray &reply, const QByteArray &auth, int exitCode)
{
    const QString dir = m_cli.path();
    auto write = [&](const QString &name, const QByteArray &data) {
        QFile f(dir + '/' + name);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(data);
    };
    write("reply.jsonl", reply);
    write("auth.json", auth);
    const QString script = dir + "/claude";
    write("claude", QStringLiteral("#!/bin/sh\n"
                                   "dir='%1'\n"
                                   "if [ \"$1\" = auth ]; then cat \"$dir/auth.json\"; exit 0; fi\n"
                                   "printf '%s\\n' \"$@\" > \"$dir/args.txt\"\n"
                                   "cat > \"$dir/stdin.txt\"\n"
                                   "echo \"${ANTHROPIC_API_KEY:-none}\" > \"$dir/env.txt\"\n"
                                   "pwd > \"$dir/cwd.txt\"\n"
                                   "cat \"$dir/reply.jsonl\"\n"
                                   "exit %2\n")
                       .arg(dir)
                       .arg(exitCode)
                       .toUtf8());
    QFile::setPermissions(script, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    qputenv("ARGONAUT_CLAUDE_PROGRAM", QFile::encodeName(script));
    return script;
}

QString TestClaude::read(const QString &name) const
{
    QFile f(m_cli.path() + '/' + name);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

void TestClaude::init()
{
    Claude::resetSpent();
    Claude::setModel("claude-opus-5-5");
    Claude::setConnection(Claude::Connection::ApiKey);
}

void TestClaude::sendsAMessage()
{
    FakeApi api;
    qputenv("ARGONAUT_CLAUDE_BASE_URL", api.url().toUtf8());
    api.replies.append({200, message("Hello there"), {}});
    Claude::Client client;
    client.setApiKey("sk-ant-test");
    bool done = false;
    Claude::Response response;
    Claude::Error error;
    client.send(QJsonObject{{"messages", QJsonArray{QJsonObject{{"role", "user"}, {"content", "Hi"}}}}},
                [&](const Claude::Response &r, const Claude::Error &e) {
                    response = r;
                    error = e;
                    done = true;
                });
    QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
    QVERIFY(error.message.isEmpty());
    QCOMPARE(response.text, QStringLiteral("Hello there"));
    QCOMPARE(response.inputTokens, 1200);
    QCOMPARE(api.requests.size(), 1);
    const auto &r = api.requests.first();
    QCOMPARE(r.method, QByteArray("POST"));
    QCOMPARE(r.path, QByteArray("/v1/messages"));
    QCOMPARE(r.headers.value("x-api-key"), QByteArray("sk-ant-test"));
    QCOMPARE(r.headers.value("anthropic-version"), QByteArray("2026-10-21"));
    QCOMPARE(r.headers.value("anthropic-beta"), QByteArray("server-side-fallback-2026-07-01"));
    QCOMPARE(r.body.value("model").toString(), QStringLiteral("claude-opus-5-5")); // the default
    QCOMPARE(r.body.value("max_tokens").toInt(), 16000);
    QCOMPARE(r.body.value("fallbacks").toString(), QStringLiteral("default"));

    // Haiku has no server-side fallback: no beta header, no field.
    api.replies.append({200, message("ok", "end_turn", 10, 10, "claude-haiku-5-5"), {}});
    done = false;
    client.send(QJsonObject{{"model", "claude-haiku-5-5"},
                            {"messages", QJsonArray{QJsonObject{{"role", "user"}, {"content", "Hi"}}}}},
                [&](const Claude::Response &, const Claude::Error &) { done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
    QVERIFY(!api.requests.last().headers.contains("anthropic-beta"));
    QVERIFY(!api.requests.last().body.contains("fallbacks"));
}

void TestClaude::retriesWhenOverloaded()
{
    FakeApi api;
    qputenv("ARGONAUT_CLAUDE_BASE_URL", api.url().toUtf8());
    api.replies.append({529, R"({"type":"error","error":{"type":"overloaded_error","message":"Overloaded"}})", {}});
    api.replies.append({200, message("finally"), {}});
    Claude::Client client;
    client.setApiKey("k");
    bool done = false;
    Claude::Response response;
    Claude::Error error;
    client.send(QJsonObject{{"messages", QJsonArray{QJsonObject{{"role", "user"}, {"content", "Hi"}}}}},
                [&](const Claude::Response &r, const Claude::Error &e) {
                    response = r;
                    error = e;
                    done = true;
                });
    QTRY_VERIFY_WITH_TIMEOUT(done, 15000);
    QVERIFY(error.message.isEmpty());
    QCOMPARE(response.text, QStringLiteral("finally"));
    QCOMPARE(api.requests.size(), 2);

    // Not retried: a bad key.
    api.replies.append({401, R"({"type":"error","error":{"type":"authentication_error","message":"invalid x-api-key"},"request_id":"req_1"})", {}});
    done = false;
    client.send(QJsonObject{{"messages", QJsonArray{QJsonObject{{"role", "user"}, {"content", "Hi"}}}}},
                [&](const Claude::Response &, const Claude::Error &e) {
                    error = e;
                    done = true;
                });
    QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
    QCOMPARE(error.status, 401);
    QCOMPARE(error.requestId, QStringLiteral("req_1"));
    QCOMPARE(api.requests.size(), 3);
}

void TestClaude::errorsForPeople()
{
    auto e = Claude::Client::parseError(401, R"({"error":{"type":"authentication_error","message":"x"}})", {});
    QVERIFY(e.message.contains("API key isn't valid"));
    QVERIFY(!e.retryable);
    e = Claude::Client::parseError(402, R"({"error":{"type":"billing_error","message":"Your credit balance is too low"}})", {});
    QVERIFY(e.message.contains("credit balance is too low"));
    e = Claude::Client::parseError(429, "{}", {});
    QVERIFY(e.retryable);
    e = Claude::Client::parseError(0, {}, "Connection refused");
    QVERIFY(e.message.contains("Connection refused"));
    QVERIFY(e.retryable);
    e = Claude::Client::parseError(400, R"({"error":{"type":"invalid_request_error","message":"messages: bad"}})", {});
    QCOMPARE(e.message, QStringLiteral("messages: bad"));
}

void TestClaude::testKey()
{
    FakeApi api;
    qputenv("ARGONAUT_CLAUDE_BASE_URL", api.url().toUtf8());
    api.replies.append({200, R"({"data":[{"id":"claude-opus-5-5"}]})", {}});
    api.replies.append({401, R"({"error":{"type":"authentication_error","message":"invalid"}})", {}});
    Claude::Client client;
    bool ok = false;
    QString message;
    int calls = 0;
    auto done = [&](bool o, const QString &m) {
        ok = o;
        message = m;
        ++calls;
    };
    client.testKey("good", done);
    QTRY_COMPARE_WITH_TIMEOUT(calls, 1, 10000);
    QVERIFY(ok);
    QCOMPARE(api.requests.first().method, QByteArray("GET"));
    QVERIFY(api.requests.first().path.startsWith("/v1/models")); // free: nothing is generated
    client.testKey("bad", done);
    QTRY_COMPARE_WITH_TIMEOUT(calls, 2, 10000);
    QVERIFY(!ok);
    QVERIFY(message.contains("isn't valid"));
    QCOMPARE(Claude::spent(), 0.0);
}

void TestClaude::countTokens()
{
    FakeApi api;
    qputenv("ARGONAUT_CLAUDE_BASE_URL", api.url().toUtf8());
    api.replies.append({200, R"({"input_tokens":2345})", {}});
    Claude::Client client;
    client.setApiKey("k");
    qint64 tokens = -1;
    client.countTokens(QJsonObject{{"max_tokens", 100}, {"fallbacks", "default"},
                                   {"messages", QJsonArray{QJsonObject{{"role", "user"}, {"content", "Hi"}}}}},
                       [&](qint64 t, const Claude::Error &) { tokens = t; });
    QTRY_COMPARE_WITH_TIMEOUT(tokens, 2345, 10000);
    QCOMPARE(api.requests.first().path, QByteArray("/v1/messages/count_tokens"));
    QVERIFY(!api.requests.first().body.contains("max_tokens"));
    QVERIFY(!api.requests.first().body.contains("fallbacks"));
    QVERIFY(api.requests.first().body.contains("model"));
}

void TestClaude::refusal()
{
    FakeApi api;
    qputenv("ARGONAUT_CLAUDE_BASE_URL", api.url().toUtf8());
    api.replies.append({200, message("", "refusal"), {}});
    Claude::Client client;
    client.setApiKey("k");
    Claude::Error error;
    bool done = false;
    client.send(QJsonObject{{"messages", QJsonArray{QJsonObject{{"role", "user"}, {"content", "Hi"}}}}},
                [&](const Claude::Response &, const Claude::Error &e) {
                    error = e;
                    done = true;
                });
    QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
    QCOMPARE(error.type, QStringLiteral("refusal"));
}

void TestClaude::costs()
{
    QCOMPARE(Claude::cost("claude-opus-5-5", 1000000, 0), 4.0);
    QCOMPARE(Claude::cost("claude-opus-5-5", 0, 1000000), 20.0);
    QCOMPARE(Claude::cost("claude-sonnet-5-5", 1000000, 1000000), 12.0);
    QCOMPARE(Claude::cost("claude-haiku-5-5", 100000, 0), 0.01);
    QCOMPARE(Claude::cost("claude-haiku-5-5", 200000, 0), 0.1); // longer prompts: $0.50 / MTok
    QCOMPARE(Claude::modelInfo("nonsense").id, QStringLiteral("claude-opus-5-5"));
    Claude::resetSpent();
    Claude::addSpent(0.25);
    Claude::addSpent(0.5);
    QCOMPARE(Claude::spent(), 0.75);
    Claude::setMode(Claude::Mode::NamesOnly);
    QCOMPARE(Claude::mode(), Claude::Mode::NamesOnly);
    Claude::setMode(Claude::Mode::Ask);
}

void TestClaude::keyStoreFile()
{
    qputenv("ARGONAUT_KEYSTORE", "file");
    qunsetenv("ANTHROPIC_API_KEY");
    KeyStore::remove();
    KeyStore::Where where = KeyStore::Where::Keyring;
    QCOMPARE(KeyStore::load(&where), QString());
    QCOMPARE(where, KeyStore::Where::None);
    QVERIFY(KeyStore::save("sk-ant-secret", &where));
    QCOMPARE(where, KeyStore::Where::File);
    QCOMPARE(KeyStore::load(&where), QStringLiteral("sk-ant-secret"));
    const QString file = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/anthropic-key";
    QCOMPARE(QFileInfo(file).permissions() & (QFile::ReadGroup | QFile::ReadOther | QFile::WriteGroup | QFile::WriteOther),
             QFile::Permissions());
    QVERIFY(KeyStore::remove());
    QCOMPARE(KeyStore::load(), QString());
    QVERIFY(!QFile::exists(file));
}

void TestClaude::environmentKeyWins()
{
    qputenv("ARGONAUT_KEYSTORE", "file");
    KeyStore::save("stored");
    qputenv("ANTHROPIC_API_KEY", "from-env");
    KeyStore::Where where;
    QCOMPARE(KeyStore::load(&where), QStringLiteral("from-env"));
    QCOMPARE(where, KeyStore::Where::Environment);
    qunsetenv("ANTHROPIC_API_KEY");
    KeyStore::remove();
}

namespace {

// A request with two tools and one finished round of reading, as Organizer sends.
QJsonObject toolRequest()
{
    const QJsonObject readSchema{{"type", "object"},
                                 {"properties", QJsonObject{{"files", QJsonObject{{"type", "array"}, {"items", QJsonObject{{"type", "string"}}}}}}},
                                 {"required", QJsonArray{"files"}},
                                 {"additionalProperties", false}};
    const QJsonObject planSchema{{"type", "object"},
                                 {"properties", QJsonObject{{"report", QJsonObject{{"type", "string"}}}}},
                                 {"required", QJsonArray{"report"}},
                                 {"additionalProperties", false}};
    const QJsonArray tools{QJsonObject{{"name", "read_files"}, {"description", "Read the start of files"}, {"strict", true}, {"input_schema", readSchema}},
                           QJsonObject{{"name", "propose_plan"}, {"description", "Propose the plan"}, {"strict", true}, {"input_schema", planSchema}}};
    const QJsonArray messages{
        QJsonObject{{"role", "user"}, {"content", "<inventory>\nscan.pdf\n</inventory>\n\nPlease organize this folder."}},
        QJsonObject{{"role", "assistant"},
                    {"content", QJsonArray{QJsonObject{{"type", "thinking"}, {"thinking", ""}, {"signature", "sig"}},
                                           QJsonObject{{"type", "tool_use"}, {"id", "toolu_1"}, {"name", "read_files"},
                                                       {"input", QJsonObject{{"files", QJsonArray{"scan.pdf"}}}}}}}},
        QJsonObject{{"role", "user"},
                    {"content", QJsonArray{QJsonObject{
                                    {"type", "tool_result"},
                                    {"tool_use_id", "toolu_1"},
                                    {"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", "scan.pdf begins: Invoice 2024"}},
                                                           QJsonObject{{"type", "image"},
                                                                       {"source", QJsonObject{{"type", "base64"}, {"media_type", "image/jpeg"}, {"data", "QUJD"}}}}}}}}}}};
    return QJsonObject{{"model", "claude-sonnet-5-5"}, {"system", "You tidy folders."}, {"tools", tools}, {"messages", messages}};
}

QByteArray cliResult(const QJsonObject &extra)
{
    QJsonObject result{{"type", "result"}, {"subtype", "success"}, {"is_error", false}, {"uuid", "u1"}, {"stop_reason", "tool_use"},
                       {"total_cost_usd", 0.5},
                       {"usage", QJsonObject{{"input_tokens", 10}, {"cache_creation_input_tokens", 90}, {"output_tokens", 40}}}};
    for (auto it = extra.begin(); it != extra.end(); ++it)
        result.insert(it.key(), it.value());
    return "{\"type\":\"system\",\"subtype\":\"init\"}\n" + QJsonDocument(result).toJson(QJsonDocument::Compact) + "\n";
}

} // namespace

// Claude Code is only a relay: none of its own tools or settings, our system
// prompt, and our tools as one structured output.
void TestClaude::subscriptionArguments()
{
    const QStringList args = Claude::Client::cliArguments(toolRequest());
    QVERIFY(args.contains("-p"));
    QVERIFY(args.contains("--safe-mode"));
    QVERIFY(args.contains("--no-session-persistence"));
    QVERIFY(args.contains("--strict-mcp-config"));
    QCOMPARE(args.value(args.indexOf("--tools") + 1), QString()); // no built-in tools at all
    QCOMPARE(args.value(args.indexOf("--model") + 1), QStringLiteral("claude-sonnet-5-5"));
    const QString system = args.value(args.indexOf("--system-prompt") + 1);
    QVERIFY(system.startsWith("You tidy folders."));
    QVERIFY(system.contains("- read_files: Read the start of files"));
    const QJsonObject schema = QJsonDocument::fromJson(args.value(args.indexOf("--json-schema") + 1).toUtf8()).object();
    const QJsonObject properties = schema.value("properties").toObject();
    QCOMPARE(properties.value("tool").toObject().value("enum").toArray(), (QJsonArray{"read_files", "propose_plan"}));
    QVERIFY(properties.value("propose_plan").toObject().value("properties").toObject().contains("report"));
    QCOMPARE(schema.value("required").toArray(), QJsonArray{"tool"});

    // The conversation so far as one user message; thinking blocks are left out.
    const QJsonObject line = QJsonDocument::fromJson(Claude::Client::cliInput(toolRequest())).object();
    QCOMPARE(line.value("type").toString(), QStringLiteral("user"));
    const QJsonArray content = line.value("message").toObject().value("content").toArray();
    QCOMPARE(content.size(), 6);
    QVERIFY(content[0].toObject().value("text").toString().contains("<inventory>"));
    QVERIFY(content[1].toObject().value("text").toString().contains("\"read_files\""));
    QVERIFY(content[1].toObject().value("text").toString().contains("scan.pdf"));
    QVERIFY(content[2].toObject().value("text").toString().contains("Result of \"read_files\""));
    QCOMPARE(content[3].toObject().value("text").toString(), QStringLiteral("scan.pdf begins: Invoice 2024"));
    QCOMPARE(content[4].toObject().value("type").toString(), QStringLiteral("image"));
    QCOMPARE(content[4].toObject().value("source").toObject().value("data").toString(), QStringLiteral("QUJD"));

    // Without tools: plain text, no schema.
    const QJsonObject plain{{"messages", QJsonArray{QJsonObject{{"role", "user"}, {"content", "Hi"}}}}};
    QVERIFY(!Claude::Client::cliArguments(plain).contains("--json-schema"));
    Claude::Error error;
    const Claude::Response r = Claude::Client::parseCliOutput(plain, cliResult({{"result", "Hello"}, {"stop_reason", "end_turn"}}), &error);
    QVERIFY(error.message.isEmpty());
    QCOMPARE(r.text, QStringLiteral("Hello"));
}

void TestClaude::subscriptionSendsThroughClaudeCode()
{
    FakeApi api; // must stay unused
    qputenv("ARGONAUT_CLAUDE_BASE_URL", api.url().toUtf8());
    qputenv("ANTHROPIC_API_KEY", "sk-ant-must-not-be-used");
    fakeClaude(cliResult({{"structured_output",
                           QJsonObject{{"tool", "propose_plan"}, {"propose_plan", QJsonObject{{"report", "One invoice."}}}}}}));
    Claude::setConnection(Claude::Connection::Subscription);
    Claude::Client client;
    QVERIFY(client.ready());
    bool done = false;
    Claude::Response response;
    Claude::Error error;
    client.send(toolRequest(), [&](const Claude::Response &r, const Claude::Error &e) {
        response = r;
        error = e;
        done = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
    qunsetenv("ANTHROPIC_API_KEY");
    QVERIFY2(error.message.isEmpty(), qPrintable(error.message));

    // The answer has the shape of a Messages response with a tool call.
    QCOMPARE(response.stopReason, QStringLiteral("tool_use"));
    const QJsonObject block = response.message.value("content").toArray().first().toObject();
    QCOMPARE(block.value("type").toString(), QStringLiteral("tool_use"));
    QCOMPARE(block.value("name").toString(), QStringLiteral("propose_plan"));
    QCOMPARE(block.value("input").toObject().value("report").toString(), QStringLiteral("One invoice."));
    QVERIFY(!block.value("id").toString().isEmpty());
    QCOMPARE(response.inputTokens, 100);
    QCOMPARE(response.outputTokens, 40);

    // Nothing is billed: no API request, no spending recorded, and the API
    // key never reaches Claude Code (it would be used instead of the plan).
    QCOMPARE(api.requests.size(), 0);
    QCOMPARE(response.dollars, 0.0);
    QCOMPARE(Claude::spent(), 0.0);
    QCOMPARE(read("env.txt").trimmed(), QStringLiteral("none"));
    QVERIFY(read("args.txt").contains("--safe-mode"));
    QVERIFY(read("stdin.txt").contains("Invoice 2024"));
    QVERIFY(QDir(read("cwd.txt").trimmed()) == QDir(QDir::tempPath()));

    // Token counts aren't available this way.
    bool counted = false;
    QString countError;
    client.countTokens(toolRequest(), [&](qint64, const Claude::Error &e) {
        countError = e.message;
        counted = true;
    });
    QTRY_VERIFY(counted);
    QVERIFY(!countError.isEmpty());
}

void TestClaude::subscriptionErrors()
{
    Claude::setConnection(Claude::Connection::Subscription);
    auto send = [](Claude::Error *error) {
        Claude::Client client;
        bool done = false;
        client.send(toolRequest(), [&](const Claude::Response &, const Claude::Error &e) {
            *error = e;
            done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
    };
    Claude::Error error;

    // Not signed in.
    fakeClaude(cliResult({{"is_error", true}, {"result", "Not logged in · Please run /login"}}), "{}", 1);
    send(&error);
    QVERIFY(error.message.contains("isn't signed in"));

    // The plan's limit: Claude Code's own words are passed on.
    fakeClaude(cliResult({{"is_error", true}, {"result", "You've hit your limit · resets 3pm"}}), "{}", 1);
    send(&error);
    QVERIFY(error.message.contains("resets 3pm"));

    // An answer that names no known action.
    fakeClaude(cliResult({{"structured_output", QJsonObject{{"tool", "delete_everything"}}}}));
    send(&error);
    QVERIFY(error.message.contains("expected form"));

    // Nothing printed at all.
    fakeClaude("", "{}", 3);
    send(&error);
    QVERIFY(!error.message.isEmpty());

    // Not installed.
    qputenv("ARGONAUT_CLAUDE_PROGRAM", QFile::encodeName(m_cli.path() + "/missing"));
    QVERIFY(Claude::Client().ready()); // a path is set; starting it is what fails
    send(&error);
    QVERIFY(error.message.contains("Couldn't start Claude Code"));
}

void TestClaude::subscriptionLogin()
{
    Claude::Login login = Claude::parseLogin(R"({"loggedIn":true,"authMethod":"claude.ai","subscriptionType":"max","email":"someone@example.com"})");
    QVERIFY(login.loggedIn && login.subscription);
    QCOMPARE(login.plan, QStringLiteral("max"));
    QVERIFY(login.message.contains("Max plan"));
    QVERIFY(!login.message.contains("example.com")); // the account is never shown

    login = Claude::parseLogin(R"({"loggedIn":true,"authMethod":"console"})");
    QVERIFY(login.loggedIn && !login.subscription);
    login = Claude::parseLogin(R"({"loggedIn":false})");
    QVERIFY(!login.loggedIn && !login.subscription);
    QVERIFY(login.message.contains("isn't signed in"));
    login = Claude::parseLogin("garbage");
    QVERIFY(!login.loggedIn && !login.message.isEmpty());

    fakeClaude("", R"({"loggedIn":true,"authMethod":"claude.ai","subscriptionType":"pro"})");
    Claude::Client client;
    bool done = false;
    client.checkLogin([&](const Claude::Login &l) {
        login = l;
        done = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
    QVERIFY(login.subscription);
    QCOMPARE(login.plan, QStringLiteral("pro"));
    QVERIFY(!QFile::exists(m_cli.path() + "/args.txt") || !read("args.txt").contains("auth")); // only "auth status" ran
}

int main(int argc, char **argv)
{
    QTemporaryDir sandbox;
    qputenv("HOME", QFile::encodeName(sandbox.path() + "/home"));
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(sandbox.path() + "/config"));
    qputenv("XDG_DATA_HOME", QFile::encodeName(sandbox.path() + "/data"));
    qputenv("ARGONAUT_KEYSTORE", "file"); // never the real keyring
    qunsetenv("ANTHROPIC_API_KEY");
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName("ArgonautTest");
    QCoreApplication::setApplicationName("ArgonautTest");
    TestClaude test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_claude.moc"
