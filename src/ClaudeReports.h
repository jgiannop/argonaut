#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>

namespace Claude {
struct Response;
}

// Every answer Claude gives is kept as a file the user can read later:
// a Markdown page in the "claudeReports" folder of Argonaut's data
// ($XDG_DATA_HOME/argonaut/claudeReports, usually
// ~/.local/share/argonaut/claudeReports), named after who asked and when:
//
//     running_services_report_17.11.26_12.54.31.md
//
// It holds the answer and, under it, **everything that was sent**: the
// system instructions, the whole conversation (what the user typed with
// it), the tools that were offered and the settings. Pictures and documents
// that went along are named with their size, not copied. The API key is
// never part of a request's body, so it is never in a report.
//
// Claude::Client::send does this itself for every request that is answered,
// so nothing that asks Claude has to remember to (extensions ask through
// Argonaut's client; setAskedBy says for whom).
namespace ClaudeReports {

QString folder();

// The file's name for an answer to `askedBy` at `when` (without the folder).
QString fileName(const QString &askedBy, const QDateTime &when);

// The page itself.
QString markdown(const QString &askedBy, const QJsonObject &request, const Claude::Response &response, const QDateTime &when);

// Writes it (the folder is made when needed; only the user can read the
// file). Returns the file's path, or nothing when it couldn't be written:
// that never gets in the way of the answer.
QString save(const QString &askedBy, const QJsonObject &request, const Claude::Response &response,
             const QDateTime &when = QDateTime::currentDateTime());

} // namespace ClaudeReports
