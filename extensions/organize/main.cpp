// Organize with Claude: an extension that proposes folders and better names
// for the items of the open folder.
//
// It has neither the user's API key nor the files' fate in its hands:
//  - What it wants Claude to be asked goes to Argonaut ("claude" messages),
//    which asks with the user's key and model and counts the cost.
//  - What Claude proposes goes to Argonaut as a plan ("plan"): Argonaut
//    checks every move, shows them in the preview pane to be ticked, and
//    does what is kept itself, so Ctrl+Z undoes it.
// Its own part is the conversation: describing the folder (names, sizes,
// dates and real types; never contents unless the user allows it), and
// reading the beginning of the files Claude asks about. That is the
// Organizer of Argonaut's core, used as it is.
//
//   argonaut-organize-extension              the extension (talks on stdin/stdout)

#include "../shared/Conversation.h"
#include "Claude.h"
#include "Format.h"
#include "Organizer.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSocketNotifier>

#include <cstdio>
#include <unistd.h>

namespace {

// (Argonaut is told things with send(); "say" reads better here, where Claude is asked things too.)
void say(const QJsonObject &message)
{
    ::send(message);
}

void tell(const QString &text)
{
    say({{QStringLiteral("type"), QStringLiteral("message")}, {QStringLiteral("text"), text}});
}

// The Claude client the Organizer talks to: its questions go to Argonaut.
class ThroughArgonaut : public Claude::Client
{
public:
    using Claude::Client::Client;

    void countTokens(QJsonObject request, std::function<void(qint64, const Claude::Error &)> done) override
    {
        const QString id = QStringLiteral("count%1").arg(++m_next);
        m_counts.insert(id, std::move(done));
        say({{QStringLiteral("type"), QStringLiteral("claude")}, {QStringLiteral("id"), id}, {QStringLiteral("ask"), QStringLiteral("count")},
             {QStringLiteral("request"), request}});
    }

    void send(QJsonObject request, std::function<void(const Claude::Response &, const Claude::Error &)> done) override
    {
        const QString id = QStringLiteral("ask%1").arg(++m_next);
        m_sends.insert(id, std::move(done));
        say({{QStringLiteral("type"), QStringLiteral("claude")}, {QStringLiteral("id"), id}, {QStringLiteral("request"), request}});
    }

    // Argonaut's answer. False: not one of ours.
    bool answered(const QJsonObject &message)
    {
        const QString id = message.value(QLatin1String("id")).toString();
        Claude::Error error;
        if (message.contains(QLatin1String("error"))) {
            const QJsonObject e = message.value(QLatin1String("error")).toObject();
            error.message = e.value(QLatin1String("message")).toString();
            error.status = e.value(QLatin1String("status")).toInt();
            if (error.message.isEmpty())
                error.message = QStringLiteral("Claude could not be asked");
        }
        if (m_counts.contains(id)) {
            const auto done = m_counts.take(id);
            done(qint64(message.value(QLatin1String("tokens")).toDouble()), error);
            return true;
        }
        if (m_sends.contains(id)) {
            const auto done = m_sends.take(id);
            Claude::Response response;
            response.message = message.value(QLatin1String("response")).toObject();
            response.text = message.value(QLatin1String("text")).toString();
            response.stopReason = message.value(QLatin1String("stop_reason")).toString();
            response.dollars = message.value(QLatin1String("dollars")).toDouble();
            const QJsonObject usage = response.message.value(QLatin1String("usage")).toObject();
            response.inputTokens = qint64(usage.value(QLatin1String("input_tokens")).toDouble());
            response.outputTokens = qint64(usage.value(QLatin1String("output_tokens")).toDouble());
            done(response, error);
            return true;
        }
        return false;
    }

    void forget()
    {
        m_counts.clear();
        m_sends.clear();
    }

private:
    int m_next = 0;
    QHash<QString, std::function<void(qint64, const Claude::Error &)>> m_counts;
    QHash<QString, std::function<void(const Claude::Response &, const Claude::Error &)>> m_sends;
};

QString nameOf(const QString &folder)
{
    const QString name = QFileInfo(folder).fileName();
    return name.isEmpty() ? QDir::toNativeSeparators(folder) : name;
}

QJsonArray commands()
{
    return QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("organize")},
                                  {QStringLiteral("label"), QStringLiteral("Organize with Claude…")},
                                  {QStringLiteral("menu"), QStringLiteral("file")}},
                      QJsonObject{{QStringLiteral("id"), QStringLiteral("organize-here")},
                                  {QStringLiteral("label"), QStringLiteral("Organize with Claude…")},
                                  {QStringLiteral("target"), QStringLiteral("folder")}}};
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    // Argonaut's own settings: the mode chosen in Preferences → Claude.
    QCoreApplication::setOrganizationName(QStringLiteral("Argonaut"));
    QCoreApplication::setApplicationName(QStringLiteral("Argonaut"));

    ThroughArgonaut client;
    Organizer *organizer = nullptr; // one folder at a time
    const QList<Claude::Mode> modes{Claude::Mode::NamesOnly, Claude::Mode::Ask, Claude::Mode::Auto};

    auto finish = [&] {
        if (organizer) {
            organizer->deleteLater();
            organizer = nullptr;
        }
        client.forget();
    };

    // The first form: what will be sent, the user's wishes, what Claude may
    // see for this folder, and about what it will cost. Nothing is sent to
    // Claude before Start.
    auto askToStart = [&](const QString &estimate) {
        const int items = int(organizer->items().size());
        QString what = items == 1 ? QStringLiteral("1 item") : QStringLiteral("%1 items").arg(items);
        if (organizer->skipped() > 0)
            what += QStringLiteral(" (the %1 most recent; %2 older ones are left out)").arg(Organizer::MaxItems).arg(organizer->skipped());
        QJsonArray choices;
        for (Claude::Mode mode : modes)
            choices.append(Claude::modeName(mode));
        say({{QStringLiteral("type"), QStringLiteral("form")},
             {QStringLiteral("id"), QStringLiteral("start")},
             {QStringLiteral("title"), QStringLiteral("Organize “%1”").arg(nameOf(organizer->folder()))},
             {QStringLiteral("heading"), QStringLiteral("Organize “%1” with Claude").arg(nameOf(organizer->folder()))},
             {QStringLiteral("text"),
              QStringLiteral("Claude will look at %1 here and propose folders and better names, with a short report. Nothing changes "
                             "until you have looked at the plan and applied it, and Ctrl+Z undoes it.\n\n"
                             "Sent to Anthropic: the names, sizes, dates and types of these items, and (if you allow it) the "
                             "beginning of some files. Programs are never read.%2")
                  .arg(what, estimate.isEmpty() ? QString() : QStringLiteral("\n\n") + estimate)},
             {QStringLiteral("fields"),
              QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("wishes")},
                                     {QStringLiteral("label"), QStringLiteral("Your wishes (optional)")},
                                     {QStringLiteral("placeholder"), QStringLiteral("e.g. invoices by year, keep the photos together")}},
                         QJsonObject{{QStringLiteral("id"), QStringLiteral("mode")},
                                     {QStringLiteral("kind"), QStringLiteral("choice")},
                                     {QStringLiteral("label"), QStringLiteral("What Claude may see")},
                                     {QStringLiteral("choices"), choices},
                                     {QStringLiteral("value"), Claude::modeName(Claude::mode())}}}},
             {QStringLiteral("buttons"), QJsonArray{QStringLiteral("Start")}}});
    };

    auto begin = [&](const QString &folder) {
        if (organizer) {
            tell(QStringLiteral("Still organizing “%1”: finish that first").arg(nameOf(organizer->folder())));
            return;
        }
        const QFileInfo info(folder);
        if (!QDir::isAbsolutePath(folder) || !info.isDir()) {
            tell(QStringLiteral("Organize needs a folder: open one first"));
            return;
        }
        if (!info.isWritable()) {
            tell(QStringLiteral("“%1” can't be changed, so there is nothing to organize").arg(nameOf(folder)));
            return;
        }
        organizer = new Organizer(QDir::cleanPath(folder), &client, &app);
        organizer->scan();
        if (organizer->items().isEmpty()) {
            tell(QStringLiteral("“%1” is empty").arg(nameOf(folder)));
            finish();
            return;
        }
        QObject::connect(organizer, &Organizer::progress, &app, [](const QString &text) { tell(text); });
        // Ask mode: Claude would like to read the beginning of some files.
        QObject::connect(organizer, &Organizer::readRequest, &app, [&](const QStringList &files, const QString &reason) {
            QString list = files.mid(0, 12).join(QStringLiteral(", "));
            if (files.size() > 12)
                list += QStringLiteral(" and %1 more").arg(files.size() - 12);
            say({{QStringLiteral("type"), QStringLiteral("form")},
                 {QStringLiteral("id"), QStringLiteral("read")},
                 {QStringLiteral("title"), QStringLiteral("Organize “%1”").arg(nameOf(organizer->folder()))},
                 {QStringLiteral("heading"), files.size() == 1 ? QStringLiteral("Claude would like to read the beginning of 1 file")
                                                               : QStringLiteral("Claude would like to read the beginning of %1 files").arg(files.size())},
                 {QStringLiteral("text"), (reason.isEmpty() ? QString() : reason + QStringLiteral("\n\n")) + list},
                 {QStringLiteral("buttons"), QJsonArray{QStringLiteral("Allow"), QStringLiteral("Don't share")}}});
        });
        QObject::connect(organizer, &Organizer::failed, &app, [&](const QString &error) {
            tell(QStringLiteral("Organize: %1").arg(error));
            finish();
        });
        // Claude's proposal: Argonaut's to check, show and carry out.
        QObject::connect(organizer, &Organizer::finished, &app, [&](const QString &report, const QList<Organizer::Move> &moves) {
            QJsonArray list;
            for (const Organizer::Move &move : moves) {
                QJsonObject one{{QStringLiteral("item"), move.item}, {QStringLiteral("reason"), move.reason}};
                if (!move.folder.isEmpty())
                    one.insert(QStringLiteral("folder"), move.folder);
                if (!move.newName.isEmpty())
                    one.insert(QStringLiteral("new_name"), move.newName);
                list.append(one);
            }
            say({{QStringLiteral("type"), QStringLiteral("plan")},
                 {QStringLiteral("id"), QStringLiteral("plan")},
                 {QStringLiteral("title"), QStringLiteral("Organize “%1”").arg(nameOf(organizer->folder()))},
                 {QStringLiteral("folder"), organizer->folder()},
                 {QStringLiteral("report"), report},
                 {QStringLiteral("note"), organizer->dollars() > 0 ? QStringLiteral("This cost about $%1").arg(QString::number(organizer->dollars(), 'f', 3)) : QString()},
                 {QStringLiteral("moves"), list}});
        });
        // Is there a key at all? (Free to ask.)
        say({{QStringLiteral("type"), QStringLiteral("claude")}, {QStringLiteral("id"), QStringLiteral("info")}, {QStringLiteral("ask"), QStringLiteral("info")}});
    };

    // Argonaut's messages, each as it comes.
    Conversation conversation([&](const QJsonObject &message) {
        const QString type = message.value(QLatin1String("type")).toString();
        const QString id = message.value(QLatin1String("id")).toString();
        if (type == QLatin1String("hello")) {
            say({{QStringLiteral("type"), QStringLiteral("commands")}, {QStringLiteral("commands"), commands()}});
        } else if (type == QLatin1String("invoke")) {
            begin(message.value(QLatin1String("folder")).toString());
        } else if (type == QLatin1String("claude")) {
            if (id == QLatin1String("info")) {
                if (!organizer)
                    return;
                if (message.contains(QLatin1String("error"))) {
                    tell(QStringLiteral("Organize: %1").arg(message.value(QLatin1String("error")).toObject().value(QLatin1String("message")).toString()));
                    finish();
                } else if (!message.value(QLatin1String("hasKey")).toBool()) {
                    tell(QStringLiteral("Organize with Claude needs your Claude API key: Preferences → Claude"));
                    finish();
                } else {
                    // About what it will cost (counting is free), then the question.
                    organizer->setMode(Claude::mode());
                    organizer->estimate([&](qint64 tokens, double dollars, const QString &error) {
                        if (!organizer)
                            return;
                        askToStart(error.isEmpty() && tokens > 0
                                       ? QStringLiteral("About %1 tokens to send: roughly $%2 on %3.")
                                             .arg(QLocale().toString(tokens), QString::number(dollars, 'f', 2), Claude::modelInfo(Claude::model()).name)
                                       : QString());
                    });
                }
                return;
            }
            client.answered(message);
        } else if (type == QLatin1String("form")) {
            if (!organizer)
                return;
            if (message.value(QLatin1String("cancelled")).toBool()) {
                organizer->cancel();
                if (id == QLatin1String("read"))
                    tell(QStringLiteral("Organize: cancelled"));
                finish();
            } else if (id == QLatin1String("start")) {
                const QJsonObject values = message.value(QLatin1String("values")).toObject();
                organizer->setWishes(values.value(QLatin1String("wishes")).toString());
                for (Claude::Mode mode : modes) {
                    if (Claude::modeName(mode) == values.value(QLatin1String("mode")).toString())
                        organizer->setMode(mode);
                }
                tell(QStringLiteral("Organize: asking Claude…"));
                organizer->start();
            } else if (id == QLatin1String("read")) {
                organizer->allowReading(message.value(QLatin1String("button")).toInt() == 0);
            }
        } else if (type == QLatin1String("plan")) {
            finish(); // applied or dismissed: Argonaut has said what happened
        }
    });
    return app.exec();
}
