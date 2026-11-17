#pragma once

// "Analyze with Claude" under a list an extension shows (the running
// services, the running processes): the conversation with Argonaut, which
// is the same for every such list.
//
//   the list's "show" carries Analysis::offer(): a button under it, with a
//       line to type beside it;
//   pressed ("action": "analyze"), the extension asks Argonaut whether there
//       is a key (free), then sends one Messages request through it (the
//       extension never has the key);
//   the answer is shown as Markdown *under* the list, which stays on screen;
//       where the list is no longer shown, and in the terminal interface,
//       it is a result of its own, with "Back to the List".
//
// A list of this kind is its table and its instructions to Claude (request)
// plus one of these: hear() takes the messages that are its.

#include "Conversation.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <functional>

struct Analysis
{
    QString listId;   // the list's result id ("services")
    QString reportId; // the report's ("services-report")
    QString title;    // "Running Services"
    QString things;   // "services", in a sentence
    // Said under every report: what it is, and what it is not.
    QString caution = QStringLiteral("Claude's reading of the list, not a security scan: it sees names and paths, not what the programs do");

    // How many there are to ask about now (0: none yet).
    std::function<int()> count;
    // The Messages request for the list as it is now, with what the user typed.
    std::function<QJsonObject(const QString &wish)> request;
    // The list is on screen. And: show it again (from the report, or after an error that had taken the pane).
    std::function<bool()> listShown;
    std::function<void()> showList;

    // What the list's "show" says to get the button and its field.
    static QJsonObject offer()
    {
        return {{QStringLiteral("id"), QStringLiteral("analyze")},
                {QStringLiteral("label"), QStringLiteral("Analyze with Claude")},
                {QStringLiteral("input"), QStringLiteral("Anything in particular? (optional)")}};
    }

    // The body of the request: the model, the key and how it is carried are
    // Argonaut's and the user's. `table` goes as data, apart from what the
    // user asked.
    static QJsonObject requestFor(const QString &system, const QString &tag, const QString &table, const QString &wish, const QString &plainAsk)
    {
        QString user = QStringLiteral("<%1>\n%2</%1>\n\n").arg(tag, table);
        user += wish.isEmpty() ? plainAsk : QStringLiteral("What I would like to know in particular:\n<request>\n%1\n</request>").arg(wish);
        return {{QStringLiteral("max_tokens"), 16000},
                {QStringLiteral("thinking"), QJsonObject{{QStringLiteral("type"), QStringLiteral("adaptive")}}},
                {QStringLiteral("system"), system},
                {QStringLiteral("messages"),
                 QJsonArray{QJsonObject{{QStringLiteral("role"), QStringLiteral("user")}, {QStringLiteral("content"), user}}}}};
    }
    // A cell of the table: one line, no tabs, not endless.
    static QString cell(QString text)
    {
        return text.replace(QLatin1Char('\t'), QLatin1Char(' ')).replace(QLatin1Char('\n'), QLatin1Char(' ')).left(200);
    }

    // True: the message was this conversation's.
    bool hear(const QJsonObject &message)
    {
        const QString type = message.value(QLatin1String("type")).toString();
        if (type == QLatin1String("action")) {
            const QString action = message.value(QLatin1String("action")).toString();
            if (action == QLatin1String("back")) {
                showList();
                return true;
            }
            if (action != QLatin1String("analyze"))
                return false;
            // The button under the list. Is there a key at all? (Free to ask.) The request follows the answer.
            if (m_busy) {
                tell(QStringLiteral("Claude is still reading the %1").arg(things));
            } else if (count() <= 0) {
                tell(QStringLiteral("There are no %1 to analyze yet").arg(things));
            } else {
                m_busy = true;
                m_wish = message.value(QLatin1String("text")).toString().trimmed().left(1000);
                m_count = count();
                m_request = request(m_wish); // the list as it is at this moment
                send({{QStringLiteral("type"), QStringLiteral("claude")}, {QStringLiteral("id"), QStringLiteral("info")}, {QStringLiteral("ask"), QStringLiteral("info")}});
            }
            return true;
        }
        if (type != QLatin1String("claude"))
            return false;
        if (!m_busy)
            return true;
        const QString which = message.value(QLatin1String("id")).toString();
        if (message.contains(QLatin1String("error"))) {
            m_busy = false;
            tell(QStringLiteral("Analyze with Claude: %1").arg(message.value(QLatin1String("error")).toObject().value(QLatin1String("message")).toString()));
            if (!listShown()) // the wait had the pane: the list comes back
                showList();
        } else if (which == QLatin1String("info")) {
            if (!message.value(QLatin1String("hasKey")).toBool()) {
                m_busy = false;
                tell(QStringLiteral("Analyze with Claude needs your Claude API key: Preferences → Claude (in the terminal: Edit → Claude API Key…)"));
            } else {
                tell(QStringLiteral("Asking Claude about %1 %2…").arg(m_count).arg(things));
                send({{QStringLiteral("type"), QStringLiteral("claude")}, {QStringLiteral("id"), QStringLiteral("analysis")}, {QStringLiteral("request"), m_request}});
            }
        } else if (which == QLatin1String("analysis")) {
            m_busy = false;
            QString report = message.value(QLatin1String("text")).toString().trimmed();
            if (report.isEmpty()) {
                tell(QStringLiteral("Analyze with Claude: Claude sent no report"));
                return true;
            }
            if (message.value(QLatin1String("stop_reason")).toString() == QLatin1String("max_tokens"))
                report += QStringLiteral("\n\n*The report was cut short: it reached the length limit.*");
            QStringList notes{caution};
            const double dollars = message.value(QLatin1String("dollars")).toDouble();
            if (dollars > 0)
                notes << QStringLiteral("this cost about $%1").arg(QString::number(dollars, 'f', 3));
            // Argonaut keeps every report, with what was sent: where it is.
            const QString kept = message.value(QLatin1String("report_file")).toString();
            if (!kept.isEmpty())
                notes << QStringLiteral("kept as %1 in claudeReports").arg(kept.section(QLatin1Char('/'), -1));
            // Under the list, which stays on screen (where it no longer is, or in the
            // terminal, the report is a result of its own, with the way back).
            send({{QStringLiteral("type"), QStringLiteral("show")},
                  {QStringLiteral("id"), reportId},
                  {QStringLiteral("under"), listId},
                  {QStringLiteral("title"), m_wish.isEmpty() ? QStringLiteral("%1: Claude's Report").arg(title) : QStringLiteral("%1: Claude's Answer").arg(title)},
                  {QStringLiteral("kind"), QStringLiteral("markdown")},
                  {QStringLiteral("text"), report},
                  {QStringLiteral("note"), notes.join(QStringLiteral(" · "))},
                  {QStringLiteral("action"), QJsonObject{{QStringLiteral("id"), QStringLiteral("back")}, {QStringLiteral("label"), QStringLiteral("Back to the List")}}}});
        }
        return true;
    }

private:
    static void tell(const QString &text) { send({{QStringLiteral("type"), QStringLiteral("message")}, {QStringLiteral("text"), text}}); }

    bool m_busy = false;  // Claude has been asked and hasn't answered
    int m_count = 0;
    QString m_wish;       // what the user typed beside the button
    QJsonObject m_request;
};
