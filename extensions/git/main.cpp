// The Git extension: Argonaut's first extension, and the example of how one
// is written (docs/extensions.md). It reads Argonaut's messages on standard
// input, one JSON object per line, keeps a GitStatus for every folder a view
// shows, and writes that folder's marks and branch back on standard output.

#include "GitStatus.h"

#include <QCoreApplication>
#include <QDir>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSocketNotifier>

#include <cstdio>
#include <unistd.h>

namespace {

QHash<QString, GitStatus *> g_folders;

QJsonObject badgeFor(const GitStatus::Mark &mark)
{
    QString text, color = QStringLiteral("warning"), tooltip;
    switch (mark.state) {
    case GitStatus::Modified:
        text = QStringLiteral("M");
        tooltip = QStringLiteral("Git: modified");
        break;
    case GitStatus::Added:
        text = QStringLiteral("A");
        color = QStringLiteral("success");
        tooltip = QStringLiteral("Git: added");
        break;
    case GitStatus::Untracked:
        text = QStringLiteral("U");
        color = QStringLiteral("success");
        tooltip = QStringLiteral("Git: not tracked yet");
        break;
    case GitStatus::Conflicted:
        text = QStringLiteral("C");
        color = QStringLiteral("error");
        tooltip = QStringLiteral("Git: merge conflict");
        break;
    case GitStatus::Changed: // a folder with changes inside: a dot
        tooltip = QStringLiteral("Git: changes inside");
        break;
    case GitStatus::None:
        break;
    }
    QJsonObject badge{{QStringLiteral("text"), text}, {QStringLiteral("color"), color}, {QStringLiteral("tooltip"), tooltip}};
    if (mark.deep)
        badge.insert(QStringLiteral("deep"), true);
    return badge;
}

void report(const QString &folder, const GitStatus *git)
{
    QJsonObject message{{QStringLiteral("type"), QStringLiteral("folder")}, {QStringLiteral("folder"), folder}};
    if (git->inRepository()) {
        QString tip = QStringLiteral("Git repository: %1").arg(QDir::toNativeSeparators(git->root()));
        QString text = git->branch();
        if (git->blocked()) {
            text = QStringLiteral("git: off");
            tip += QStringLiteral("\nNot checked: this repository's settings would run programs.");
        } else if (git->tooSlow()) {
            text = QStringLiteral("git: too big");
            tip += QStringLiteral("\nNot checked: git took too long for this repository.");
        } else if (!git->ready()) {
            text = QStringLiteral("…");
        } else {
            const int n = git->changeCount();
            tip += n == 0 ? QStringLiteral("\nNo changes")
                   : n == 1 ? QStringLiteral("\n1 changed item") : QStringLiteral("\n%1 changed items").arg(n);
            if (n > 0)
                text += QStringLiteral(" · %1").arg(n);
        }
        message.insert(QStringLiteral("info"), QJsonObject{{QStringLiteral("text"), text},
                                                           {QStringLiteral("tooltip"), tip},
                                                           {QStringLiteral("icon"), QStringLiteral("branch")}});
        QJsonObject badges;
        const QList<GitStatus::Mark> marks = git->marks(folder);
        for (const GitStatus::Mark &mark : marks)
            badges.insert(mark.relativePath, badgeFor(mark));
        message.insert(QStringLiteral("badges"), badges);
    }
    const QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    std::fwrite(line.constData(), 1, size_t(line.size()), stdout);
    std::fflush(stdout);
}

void handle(const QByteArray &line)
{
    const QJsonObject message = QJsonDocument::fromJson(line).object();
    const QString type = message.value(QLatin1String("type")).toString();
    const QString folder = message.value(QLatin1String("folder")).toString();
    if (type == QLatin1String("watch")) {
        if (folder.isEmpty() || g_folders.contains(folder))
            return;
        auto *git = new GitStatus(qApp);
        g_folders.insert(folder, git);
        QObject::connect(git, &GitStatus::changed, qApp, [folder, git] { report(folder, git); });
        git->setFolder(folder);
        if (!git->inRepository())
            report(folder, git); // nothing to show: said once
    } else if (type == QLatin1String("refresh")) {
        if (GitStatus *git = g_folders.value(folder))
            git->refresh();
    } else if (type == QLatin1String("unwatch")) {
        delete g_folders.take(folder);
    }
    // "hello" and anything newer than this program: nothing to do.
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QByteArray buffer;
    QSocketNotifier input(STDIN_FILENO, QSocketNotifier::Read);
    QObject::connect(&input, &QSocketNotifier::activated, &app, [&] {
        char chunk[65536];
        const ssize_t n = ::read(STDIN_FILENO, chunk, sizeof chunk);
        if (n <= 0) { // Argonaut closed the pipe (or is gone): leave
            input.setEnabled(false);
            QCoreApplication::quit();
            return;
        }
        buffer.append(chunk, n);
        qsizetype end;
        while ((end = buffer.indexOf('\n')) >= 0) {
            const QByteArray line = buffer.left(end);
            buffer.remove(0, end + 1);
            handle(line);
        }
    });
    return app.exec();
}
