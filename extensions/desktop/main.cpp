// Two small extensions in one program, each a right-click command:
//   argonaut-desktop-extension email       "Send by Email…" for files
//   argonaut-desktop-extension background  "Set as Background" for one image
// Both hand the work to the desktop's own tools (DesktopActions).

#include "DesktopActions.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

#include <cstdio>

namespace {

void send(const QJsonObject &message)
{
    const QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    std::fwrite(line.constData(), 1, size_t(line.size()), stdout);
    std::fflush(stdout);
}

void say(const QString &text)
{
    send({{QStringLiteral("type"), QStringLiteral("message")}, {QStringLiteral("text"), text}});
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const bool background = app.arguments().value(1) == QLatin1String("background");

    QTextStream in(stdin);
    QString line;
    while (in.readLineInto(&line)) { // ends when Argonaut closes the pipe
        const QJsonObject message = QJsonDocument::fromJson(line.toUtf8()).object();
        const QString type = message.value(QLatin1String("type")).toString();
        if (type == QLatin1String("hello")) {
            // What this extension offers, said once.
            QJsonObject command;
            if (background) {
                // Changing the background is GNOME's settings: only offered on a desktop.
                if (qEnvironmentVariable("XDG_CURRENT_DESKTOP").isEmpty())
                    continue;
                command = {{QStringLiteral("id"), QStringLiteral("set")},
                           {QStringLiteral("label"), QStringLiteral("Set as Background")},
                           {QStringLiteral("kind"), QStringLiteral("files")},
                           {QStringLiteral("multiple"), false},
                           {QStringLiteral("types"), QJsonArray{QStringLiteral("image/*")}}};
            } else {
                command = {{QStringLiteral("id"), QStringLiteral("send")},
                           {QStringLiteral("label"), QStringLiteral("Send by Email…")},
                           {QStringLiteral("kind"), QStringLiteral("files")}};
            }
            send({{QStringLiteral("type"), QStringLiteral("commands")}, {QStringLiteral("commands"), QJsonArray{command}}});
        } else if (type == QLatin1String("invoke")) {
            QStringList paths;
            const QJsonArray selection = message.value(QLatin1String("selection")).toArray();
            for (const QJsonValue &path : selection)
                paths << path.toString();
            if (paths.isEmpty())
                continue;
            if (background) {
                QString error;
                say(DesktopActions::setAsBackground(paths.first(), &error) ? QStringLiteral("Background changed") : error);
            } else if (!DesktopActions::sendByEmail(paths)) {
                say(QStringLiteral("Couldn't open the email app (xdg-email is missing)"));
            }
        }
    }
    return 0;
}
