// The Git extension: Argonaut's first extension, and the example of how one
// is written (docs/extensions.md). It reads Argonaut's messages on standard
// input, one JSON object per line, keeps a GitStatus for every folder a view
// shows, and writes that folder's marks and branch back on standard output.

#include "GitStatus.h"

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

QJsonObject command(const QString &id, const QString &label, const QString &group = QStringLiteral("Git"))
{
    return {{QStringLiteral("id"), id}, {QStringLiteral("label"), label}, {QStringLiteral("group"), group}};
}

// The Git menu. On marked items: Stage, Unstage, Show Changes, Discard. On
// the folder (its empty space, or the branch in the path bar): the changes,
// committing and the branches.
QJsonArray commandsFor(const GitStatus *git, const QList<GitStatus::Mark> &marks)
{
    QJsonArray commands;
    QJsonArray marked, modified;
    for (const GitStatus::Mark &mark : marks) {
        if (mark.relativePath.isEmpty())
            continue;
        marked.append(mark.relativePath);
        if (mark.state == GitStatus::Modified)
            modified.append(mark.relativePath);
    }
    if (!marked.isEmpty()) {
        QJsonObject diff = command(QStringLiteral("diff"), QStringLiteral("Show Changes"));
        diff.insert(QStringLiteral("items"), marked);
        QJsonObject stage = command(QStringLiteral("stage"), QStringLiteral("Stage"));
        stage.insert(QStringLiteral("items"), marked);
        QJsonObject unstage = command(QStringLiteral("unstage"), QStringLiteral("Unstage"));
        unstage.insert(QStringLiteral("items"), marked);
        commands.append(diff);
        commands.append(stage);
        commands.append(unstage);
    }
    if (!modified.isEmpty()) {
        // Only files that have a committed version to go back to; folders,
        // new files and conflicts are left to the command line.
        QJsonObject discard = command(QStringLiteral("discard"), QStringLiteral("Discard Changes…"));
        discard.insert(QStringLiteral("items"), modified);
        discard.insert(QStringLiteral("kind"), QStringLiteral("files"));
        discard.insert(QStringLiteral("confirm"),
                       QStringLiteral("Discard the changes in the selected files?\n\nThey go back to how they were at "
                                      "the last commit. This can't be undone, and the changes are not put in the Trash."));
        commands.append(discard);
    }

    auto onFolder = [](QJsonObject c) {
        c.insert(QStringLiteral("target"), QStringLiteral("folder"));
        return c;
    };
    if (git->changeCount() > 0)
        commands.append(onFolder(command(QStringLiteral("diff-all"), QStringLiteral("Show All Changes"))));
    if (git->stagedCount() > 0 || git->unstagedCount() > 0) {
        // What is staged is what gets committed; with nothing staged, every
        // changed file git already tracks (new files have to be staged).
        const bool staged = git->stagedCount() > 0;
        const int n = staged ? git->stagedCount() : git->unstagedCount();
        QJsonObject commit = command(QStringLiteral("commit"),
                                     staged ? QStringLiteral("Commit Staged Changes (%1)…").arg(n)
                                            : QStringLiteral("Commit All Changed Files (%1)…").arg(n));
        commit.insert(QStringLiteral("ask"), QJsonObject{{QStringLiteral("label"), QStringLiteral("Commit message:")},
                                                         {QStringLiteral("multiline"), true}});
        commands.append(onFolder(commit));
    }
    const QStringList branches = git->branches().mid(0, 50);
    for (const QString &branch : branches) {
        QJsonObject entry = command(QStringLiteral("switch:") + branch, branch, QStringLiteral("Git/Switch Branch"));
        if (branch == git->branch())
            entry.insert(QStringLiteral("checked"), true);
        commands.append(onFolder(entry));
    }
    QJsonObject create = command(QStringLiteral("new-branch"), QStringLiteral("New Branch…"));
    create.insert(QStringLiteral("ask"), QJsonObject{{QStringLiteral("label"), QStringLiteral("Name of the new branch:")}});
    commands.append(onFolder(create));
    return commands;
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
        if (git->ready() && !git->blocked() && !git->tooSlow())
            message.insert(QStringLiteral("commands"), commandsFor(git, marks));
    }
    const QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    std::fwrite(line.constData(), 1, size_t(line.size()), stdout);
    std::fflush(stdout);
}

void say(const QString &text)
{
    const QByteArray line = QJsonDocument(QJsonObject{{QStringLiteral("type"), QStringLiteral("message")},
                                                      {QStringLiteral("text"), text}})
                                .toJson(QJsonDocument::Compact)
                            + '\n';
    std::fwrite(line.constData(), 1, size_t(line.size()), stdout);
    std::fflush(stdout);
}

void show(const QString &title, const QString &diff, const QString &note)
{
    const QByteArray line = QJsonDocument(QJsonObject{{QStringLiteral("type"), QStringLiteral("show")},
                                                      {QStringLiteral("title"), title},
                                                      {QStringLiteral("kind"), QStringLiteral("diff")},
                                                      {QStringLiteral("text"), diff},
                                                      {QStringLiteral("note"), note}})
                                .toJson(QJsonDocument::Compact)
                            + '\n';
    std::fwrite(line.constData(), 1, size_t(line.size()), stdout);
    std::fflush(stdout);
}

// The changes against the last commit, of `paths` or of everything. Files
// git doesn't track yet have no diff: they are named at the end.
void showChanges(GitStatus *git, const QString &folder, const QStringList &paths)
{
    constexpr qsizetype MaxBytes = 600 * 1000;
    const QStringList diffArguments = {QStringLiteral("diff"), QStringLiteral("--no-ext-diff"), QStringLiteral("--no-textconv"),
                                       QStringLiteral("--no-color"), QStringLiteral("-M")};
    bool ok = false;
    QByteArray out = git->read(diffArguments + QStringList{QStringLiteral("HEAD")}, paths, &ok, MaxBytes);
    if (!ok) // no commit yet: what is staged, then what isn't
        out = git->read(diffArguments + QStringList{QStringLiteral("--cached")}, paths, &ok, MaxBytes)
              + git->read(diffArguments, paths, &ok, MaxBytes);
    QString text = QString::fromUtf8(out);
    int files = int(text.count(QStringLiteral("\ndiff --git "))) + (text.startsWith(QLatin1String("diff --git ")) ? 1 : 0);
    QStringList fresh;
    const QString root = git->root() + QLatin1Char('/');
    const QList<GitStatus::Mark> marks = git->marks(git->root());
    for (const GitStatus::Mark &mark : marks) {
        if (mark.state != GitStatus::Untracked || mark.relativePath.isEmpty())
            continue;
        const QString full = root + mark.relativePath;
        bool wanted = paths.isEmpty();
        for (const QString &path : paths)
            wanted = wanted || full == path || full.startsWith(path + QLatin1Char('/')) || path.startsWith(full + QLatin1Char('/'));
        if (wanted)
            fresh << mark.relativePath + (mark.deep ? QStringLiteral("/") : QString());
    }
    fresh.sort();
    for (const QString &name : std::as_const(fresh))
        text += (text.isEmpty() ? QString() : QStringLiteral("\n")) + QStringLiteral("# New, not tracked yet: ") + name;
    if (text.trimmed().isEmpty()) {
        say(QStringLiteral("No changes to show"));
        return;
    }
    QStringList note;
    if (files > 0)
        note << (files == 1 ? QStringLiteral("1 changed file") : QStringLiteral("%1 changed files").arg(files));
    if (!fresh.isEmpty())
        note << (fresh.size() == 1 ? QStringLiteral("1 new") : QStringLiteral("%1 new").arg(fresh.size()));
    if (out.size() >= MaxBytes)
        note << QStringLiteral("showing the first 600 kB");
    const QString title = paths.size() == 1 ? QStringLiteral("Changes in %1").arg(QFileInfo(paths.first()).fileName())
                                            : QStringLiteral("Changes on %1").arg(git->branch());
    Q_UNUSED(folder);
    show(title, text, note.join(QStringLiteral(" · ")));
}

void invoke(const QJsonObject &message)
{
    const QString folder = message.value(QLatin1String("folder")).toString();
    GitStatus *git = g_folders.value(folder);
    if (!git)
        return;
    QStringList paths;
    const QJsonArray selection = message.value(QLatin1String("selection")).toArray();
    for (const QJsonValue &path : selection)
        paths << path.toString();
    const QString command = message.value(QLatin1String("command")).toString();
    const QString input = message.value(QLatin1String("input")).toString().trimmed();
    QString error;
    bool ok = false;
    QString done;
    if (command == QLatin1String("diff") && !paths.isEmpty()) {
        showChanges(git, folder, paths);
        return;
    } else if (command == QLatin1String("diff-all")) {
        showChanges(git, folder, {});
        return;
    } else if (command == QLatin1String("stage") && !paths.isEmpty()) {
        ok = git->run({QStringLiteral("add")}, paths, &error);
        done = QStringLiteral("Staged");
    } else if (command == QLatin1String("unstage") && !paths.isEmpty()) {
        ok = git->run({QStringLiteral("restore"), QStringLiteral("--staged")}, paths, &error);
        done = QStringLiteral("Unstaged");
    } else if (command == QLatin1String("discard") && !paths.isEmpty()) {
        ok = git->run({QStringLiteral("restore"), QStringLiteral("--source=HEAD"), QStringLiteral("--staged"),
                       QStringLiteral("--worktree")}, paths, &error);
        done = QStringLiteral("Changes discarded");
    } else if (command == QLatin1String("commit")) {
        if (input.isEmpty())
            return;
        // What is staged; with nothing staged, all changed files git tracks.
        QStringList arguments{QStringLiteral("commit")};
        if (git->stagedCount() == 0)
            arguments << QStringLiteral("-a");
        arguments << QStringLiteral("-m") << input;
        ok = git->run(arguments, {}, &error, 60000); // signing may ask for a passphrase
        done = QStringLiteral("Committed");
    } else if (command == QLatin1String("new-branch")) {
        if (input.isEmpty() || input.startsWith(QLatin1Char('-')) || input.contains(QLatin1Char(' '))) {
            say(QStringLiteral("That can't be a branch's name."));
            return;
        }
        ok = git->run({QStringLiteral("switch"), QStringLiteral("-c"), input}, {}, &error);
        done = QStringLiteral("On the new branch %1").arg(input);
    } else if (command.startsWith(QLatin1String("switch:"))) {
        const QString branch = command.mid(7);
        if (!git->branches().contains(branch) || branch.startsWith(QLatin1Char('-')))
            return; // only to a branch this repository really has
        if (branch == git->branch())
            return;
        ok = git->run({QStringLiteral("switch"), branch}, {}, &error);
        done = QStringLiteral("Switched to %1").arg(branch);
    } else {
        return;
    }
    say(ok ? done : error);
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
    } else if (type == QLatin1String("invoke")) {
        invoke(message);
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
