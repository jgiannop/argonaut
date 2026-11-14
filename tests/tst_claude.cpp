// Tests for the Claude API client (against a fake server on localhost,
// ARGONAUT_CLAUDE_BASE_URL) and the key store's file mode. No real API calls.

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
};

void TestClaude::init()
{
    Claude::resetSpent();
    Claude::setModel("claude-opus-5-5");
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
