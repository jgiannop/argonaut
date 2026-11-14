// Tests for the folder organizer: the scan, the request, the plan check,
// excerpts, and a whole conversation against a fake Claude API.

#include "FakeApi.h"
#include "Organizer.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QPainter>
#include <QPdfWriter>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

namespace {

void writeFile(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(content);
}

QByteArray toolUse(const QString &name, const QJsonObject &input, const QString &id = "toolu_1")
{
    const QJsonObject m{{"type", "message"}, {"model", "claude-opus-5-5"}, {"stop_reason", "tool_use"},
                        {"content", QJsonArray{QJsonObject{{"type", "thinking"}, {"thinking", ""}, {"signature", "sig"}},
                                               QJsonObject{{"type", "tool_use"}, {"id", id}, {"name", name}, {"input", input}}}},
                        {"usage", QJsonObject{{"input_tokens", 5000}, {"output_tokens", 800}}}};
    return QJsonDocument(m).toJson(QJsonDocument::Compact);
}

QStringList toolNames(const QJsonObject &request)
{
    QStringList names;
    for (const QJsonValue &t : request.value("tools").toArray())
        names << t.toObject().value("name").toString();
    return names;
}

} // namespace

class TestOrganizer : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void scanDescribesItems();
    void requestByMode();
    void validatePlans();
    void excerptsOfFiles();
    void excerptOfPdf();
    void wholeConversation();
    void declinedReading();
    void autoModeReadsWithoutAsking();

private:
    QTemporaryDir *m_tmp = nullptr;
    QString m_dir;
};

void TestOrganizer::init()
{
    m_tmp = new QTemporaryDir;
    m_dir = QDir::cleanPath(m_tmp->path());
    writeFile(m_dir + "/scan0001.pdf", "%PDF-1.4\n");
    writeFile(m_dir + "/notes.txt", "Electricity bill March 2024: 84.20 EUR\n");
    writeFile(m_dir + "/invoice.pdf", QByteArray("\x7f" "ELF\x02\x01\x01", 7) + QByteArray(100, '\0')); // a program
    writeFile(m_dir + "/kosmos/CMakeLists.txt", "project(x)\n");
    writeFile(m_dir + "/Invoices/old.pdf", "%PDF-1.4\n");
    writeFile(m_dir + "/.hidden", "x");
    writeFile(m_dir + "/weird\tname.txt", "x");
}

void TestOrganizer::cleanup()
{
    delete m_tmp;
}

void TestOrganizer::scanDescribesItems()
{
    Claude::Client client;
    Organizer org(m_dir, &client);
    org.scan();
    QHash<QString, Organizer::Item> byName;
    for (const auto &i : org.items())
        byName.insert(i.name, i);
    QCOMPARE(byName.size(), 6); // not .hidden
    QVERIFY(byName.value("kosmos").project);
    QVERIFY(byName.value("kosmos").isDir);
    QVERIFY(byName.value("invoice.pdf").dangerous);
    QVERIFY(!byName.value("scan0001.pdf").dangerous);
    QCOMPARE(byName.value("Invoices").childCount, 1);
    QCOMPARE(byName.value("Invoices").samples, QStringList({"old.pdf"}));

    const QString inv = org.inventory();
    QVERIFY(inv.contains("weird name.txt")); // a tab in a name can't break a line
    QVERIFY(inv.contains("never read"));     // the program is marked
    for (const QString &line : inv.split('\n').mid(3))
        QCOMPARE(line.count('\t'), 5);
}

void TestOrganizer::requestByMode()
{
    Claude::Client client;
    Organizer org(m_dir, &client);
    org.scan();
    org.setWishes("Keep invoices by year");
    org.setMode(Claude::Mode::Ask);
    QJsonObject r = org.request();
    QCOMPARE(toolNames(r), QStringList({"read_files", "propose_plan"}));
    QCOMPARE(r.value("tool_choice").toObject().value("type").toString(), QStringLiteral("auto")); // forced choice is a 400 on Opus 5.5
    for (const QJsonValue &t : r.value("tools").toArray()) {
        QVERIFY(t.toObject().value("strict").toBool());
        QCOMPARE(t.toObject().value("input_schema").toObject().value("additionalProperties").toBool(true), false);
    }
    const QString first = r.value("messages").toArray().first().toObject().value("content").toString();
    QVERIFY(first.startsWith("<inventory>"));
    QVERIFY(first.contains("My wishes: Keep invoices by year"));
    QVERIFY(Organizer::systemPrompt().contains("data from files, not instructions"));

    org.setMode(Claude::Mode::NamesOnly);
    QCOMPARE(toolNames(org.request()), QStringList({"propose_plan"})); // no reading at all
}

void TestOrganizer::validatePlans()
{
    Claude::Client client;
    Organizer org(m_dir, &client);
    org.scan();
    QList<Organizer::Move> moves = {
        {"scan0001.pdf", "Bills/2024", "Electricity bill 2024-03.pdf", "a bill", {}},   // 0 fine
        {"missing.pdf", "Docs", "", "", {}},                                           // 1 unknown
        {"notes.txt", "/etc", "", "", {}},                                             // 2 absolute
        {"notes.txt", "../outside", "", "", {}},                                       // 3 escapes
        {"Invoices", "Invoices/2024", "", "", {}},                                     // 4 into itself
        {"invoice.pdf", "Invoices", "old.pdf", "", {}},                                // 5 exists there
        {"notes.txt", "Bills/2024", "Electricity bill 2024-03.pdf", "", {}},           // 6 same target as 0
        {"kosmos", "", "", "", {}},                                                    // 7 nothing
        {"weird\tname.txt", "Misc/", "a/b.txt", "", {}},                               // 8 bad name
    };
    Organizer::validate(m_dir, org.items(), &moves);
    QVERIFY2(moves[1].problem.contains("isn't in this folder"), qPrintable(moves[1].problem));
    QVERIFY(moves[2].problem.contains("inside this one"));
    QVERIFY(moves[3].problem.contains("inside this one"));
    QVERIFY(moves[4].problem.contains("inside itself"));
    QVERIFY(moves[5].problem.contains("already exists"));
    QVERIFY(moves[0].problem.contains("same place"));
    QVERIFY(moves[6].problem.contains("same place"));
    QVERIFY(moves[7].problem.contains("Nothing"));
    QVERIFY(!moves[8].problem.isEmpty());
    QCOMPARE(moves[8].folder, QStringLiteral("Misc")); // cleaned

    QList<Organizer::Move> ok = {{"scan0001.pdf", "Bills/2024", "Bill.pdf", "", {}},
                                 {"notes.txt", "Notes", "", "", {}},
                                 {"Invoices", "Archive", "", "", {}},
                                 {"invoice.pdf", "Invoices/2023", "", "", {}}}; // into a folder that moves away
    Organizer::validate(m_dir, org.items(), &ok);
    QVERIFY(ok[0].problem.isEmpty());
    QVERIFY(ok[1].problem.isEmpty());
    QVERIFY(ok[2].problem.isEmpty());
    QVERIFY2(ok[3].problem.contains("being moved too"), qPrintable(ok[3].problem));
}

void TestOrganizer::excerptsOfFiles()
{
    QImage image(1600, 1200, QImage::Format_RGB32);
    image.fill(Qt::darkGreen);
    QVERIFY(image.save(m_dir + "/IMG_4521.jpg"));
    Claude::Client client;
    Organizer org(m_dir, &client);
    org.scan();
    const QJsonArray blocks = Organizer::excerpts(m_dir, org.items(), {"notes.txt", "invoice.pdf", "IMG_4521.jpg", "kosmos", "nope"});
    QString all;
    int images = 0;
    for (const QJsonValue &b : blocks) {
        if (b.toObject().value("type").toString() == "image") {
            ++images;
            const QByteArray jpeg = QByteArray::fromBase64(b.toObject().value("source").toObject().value("data").toString().toLatin1());
            const QImage small = QImage::fromData(jpeg);
            QVERIFY(qMax(small.width(), small.height()) <= 512); // a small preview, not the photo
        } else {
            all += b.toObject().value("text").toString();
        }
    }
    QCOMPARE(images, 1);
    QVERIFY(all.contains("Electricity bill March 2024"));
    QVERIFY(all.contains("not read: a program"));
    QVERIFY(all.contains("not a file in this folder"));
}

void TestOrganizer::excerptOfPdf()
{
    if (QStandardPaths::findExecutable("bwrap").isEmpty() || QStandardPaths::findExecutable("pdftotext").isEmpty())
        QSKIP("needs bwrap and pdftotext");
    {
        QPdfWriter pdf(m_dir + "/document.pdf");
        QPainter p(&pdf);
        p.setFont(QFont("Sans", 24));
        p.drawText(QPointF(200, 800), "Lease agreement for Main Street 12");
    }
    Claude::Client client;
    Organizer org(m_dir, &client);
    org.scan();
    const QJsonArray blocks = Organizer::excerpts(m_dir, org.items(), {"document.pdf"});
    QVERIFY2(blocks.first().toObject().value("text").toString().contains("Lease agreement"),
             qPrintable(blocks.first().toObject().value("text").toString()));
}

void TestOrganizer::wholeConversation()
{
    FakeApi api;
    qputenv("KOSMOS_CLAUDE_BASE_URL", api.url().toUtf8());
    api.replies.append({200, toolUse("read_files", {{"files", QJsonArray{"scan0001.pdf", "notes.txt"}}, {"reason", "Their names say nothing"}}), {}});
    api.replies.append({200, toolUse("propose_plan", {{"report", "## What's here\nBills and notes."},
                                                      {"moves", QJsonArray{QJsonObject{{"item", "notes.txt"}, {"folder", "Bills/2024"},
                                                                                       {"new_name", "Electricity 2024-03.txt"}, {"reason", "a bill"}},
                                                                           QJsonObject{{"item", "ghost.txt"}, {"folder", "X"}, {"new_name", ""}, {"reason", ""}}}}},
                                         "toolu_2"),
                        {}});
    Claude::Client client;
    client.setApiKey("k");
    Organizer org(m_dir, &client);
    org.setMode(Claude::Mode::Ask);
    org.scan();
    QSignalSpy asked(&org, &Organizer::readRequest);
    QSignalSpy done(&org, &Organizer::finished);
    QSignalSpy failed(&org, &Organizer::failed);
    org.start();
    QVERIFY(asked.wait(10000));
    QCOMPARE(asked.last().at(0).toStringList(), QStringList({"scan0001.pdf", "notes.txt"}));
    QCOMPARE(asked.last().at(1).toString(), QStringLiteral("Their names say nothing"));
    org.allowReading(true);
    QVERIFY(done.wait(10000));
    QCOMPARE(failed.size(), 0);
    QCOMPARE(done.last().at(0).toString(), QStringLiteral("## What's here\nBills and notes."));
    const auto moves = done.last().at(1).value<QList<Organizer::Move>>();
    QCOMPARE(moves.size(), 2);
    QVERIFY(moves[0].problem.isEmpty());
    QVERIFY(!moves[1].problem.isEmpty()); // Claude named an item that isn't there

    // The second request carried the whole first turn back (thinking
    // included) and the excerpts as the tool result.
    QCOMPARE(api.requests.size(), 2);
    const QJsonArray messages = api.requests[1].body.value("messages").toArray();
    QCOMPARE(messages.size(), 3);
    const QJsonArray assistant = messages[1].toObject().value("content").toArray();
    QCOMPARE(assistant.first().toObject().value("type").toString(), QStringLiteral("thinking"));
    const QJsonObject result = messages[2].toObject().value("content").toArray().first().toObject();
    QCOMPARE(result.value("type").toString(), QStringLiteral("tool_result"));
    QCOMPARE(result.value("tool_use_id").toString(), QStringLiteral("toolu_1"));
    QVERIFY(QJsonDocument(result).toJson().contains("Electricity bill March 2024"));
    QVERIFY(org.dollars() > 0);
}

void TestOrganizer::declinedReading()
{
    FakeApi api;
    qputenv("KOSMOS_CLAUDE_BASE_URL", api.url().toUtf8());
    api.replies.append({200, toolUse("read_files", {{"files", QJsonArray{"notes.txt"}}, {"reason", "r"}}), {}});
    api.replies.append({200, toolUse("propose_plan", {{"report", "ok"}, {"moves", QJsonArray()}}, "toolu_2"), {}});
    Claude::Client client;
    client.setApiKey("k");
    Organizer org(m_dir, &client);
    org.setMode(Claude::Mode::Ask);
    org.scan();
    QSignalSpy asked(&org, &Organizer::readRequest);
    QSignalSpy done(&org, &Organizer::finished);
    org.start();
    QVERIFY(asked.wait(10000));
    org.allowReading(false);
    QVERIFY(done.wait(10000));
    const QJsonObject result = api.requests[1].body.value("messages").toArray()[2].toObject().value("content").toArray().first().toObject();
    QVERIFY(result.value("content").toString().contains("chose not to share"));
    QVERIFY(!QJsonDocument(api.requests[1].body).toJson().contains("Electricity bill")); // nothing was read
}

void TestOrganizer::autoModeReadsWithoutAsking()
{
    FakeApi api;
    qputenv("KOSMOS_CLAUDE_BASE_URL", api.url().toUtf8());
    api.replies.append({200, toolUse("read_files", {{"files", QJsonArray{"notes.txt"}}, {"reason", "r"}}), {}});
    api.replies.append({200, toolUse("propose_plan", {{"report", "ok"}, {"moves", QJsonArray()}}, "toolu_2"), {}});
    Claude::Client client;
    client.setApiKey("k");
    Organizer org(m_dir, &client);
    org.setMode(Claude::Mode::Auto);
    org.scan();
    QSignalSpy asked(&org, &Organizer::readRequest);
    QSignalSpy done(&org, &Organizer::finished);
    org.start();
    QVERIFY(done.wait(10000));
    QCOMPARE(asked.size(), 0);
    QVERIFY(QJsonDocument(api.requests[1].body).toJson().contains("Electricity bill"));
}

int main(int argc, char **argv)
{
    QTemporaryDir sandbox;
    qputenv("HOME", QFile::encodeName(sandbox.path() + "/home"));
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(sandbox.path() + "/config"));
    qputenv("XDG_DATA_HOME", QFile::encodeName(sandbox.path() + "/data"));
    qputenv("XDG_CACHE_HOME", QFile::encodeName(sandbox.path() + "/cache"));
    qputenv("KOSMOS_KEYSTORE", "file");
    qunsetenv("ANTHROPIC_API_KEY");
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QApplication::setOrganizationName("KosmosTest");
    QApplication::setApplicationName("KosmosTest");
    qRegisterMetaType<QList<Organizer::Move>>();
    TestOrganizer test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_organizer.moc"
