// Project Explorer, as an extension: a folder opened as a project is shown
// as a tree under the sidebar's places, as in a code editor.
//
// It offers File → Open as Project (and the same on a folder's right-click
// menu), Open Recent Project… (a form to choose one) and Close Project. It
// is started only when needed: for one of those, and when Argonaut starts
// if a project was open when it quit (Extensions::Host remembers that). The tree itself is
// Argonaut's: this program only says which folder it is ("tree") and what
// in it is faint (what the project's Git ignores: build/, node_modules/) or
// left out (.git). The project is remembered, so it is there again the
// next time Argonaut starts.
//
// What Git ignores is asked with `git ls-files --others --ignored
// --exclude-standard --directory` through GitStatus::read: only once the
// repository was checked (a repository's config can name programs for git
// to run; a blocked one is asked nothing), with hooks and fsmonitor off.

#include "../shared/Conversation.h"
#include "GitStatus.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QSettings>
#include <QTimer>

namespace {

constexpr int MaxRecent = 10;
constexpr int MaxFaint = 20000;

// Folders that hold what a build or a package tool made, for a project
// without Git: faint when they are directly in it.
const QStringList kMadeFolders{QStringLiteral("build"), QStringLiteral("node_modules"), QStringLiteral("dist"),
                               QStringLiteral("target"), QStringLiteral("__pycache__"), QStringLiteral(".venv"),
                               QStringLiteral("venv"), QStringLiteral(".cache"), QStringLiteral(".tox")};

void say(const QString &text)
{
    send({{QStringLiteral("type"), QStringLiteral("message")}, {QStringLiteral("text"), text}});
}

QString clean(const QString &folder)
{
    return QDir::cleanPath(QFileInfo(folder).absoluteFilePath());
}

class Explorer : public QObject
{
public:
    explicit Explorer(QObject *parent = nullptr)
        : QObject(parent)
    {
        m_later.setSingleShot(true);
        m_later.setInterval(400); // a burst of refreshes is looked at once
        connect(&m_later, &QTimer::timeout, this, [this] {
            m_git.refresh(); // the branch, if it changed
            sendTree();      // what is ignored: a new build folder doesn't change Git's status
        });
        connect(&m_git, &GitStatus::changed, this, [this] { sendTree(); });
        const QSettings settings;
        m_recent = settings.value(QStringLiteral("recent")).toStringList();
        m_project = settings.value(QStringLiteral("project")).toString();
    }

    void handle(const QJsonObject &message)
    {
        const QString type = message.value(QLatin1String("type")).toString();
        if (type == QLatin1String("hello")) {
            sendCommands();
            if (!m_project.isEmpty() && QFileInfo(m_project).isDir())
                open(m_project, false); // where the user left it
            else
                m_project.clear();
        } else if (type == QLatin1String("invoke")) {
            invoke(message);
        } else if (type == QLatin1String("form")) {
            answered(message);
        } else if (type == QLatin1String("tree")) {
            if (message.value(QLatin1String("action")).toString() == QLatin1String("close"))
                close(false); // the ✕ on the tree: Argonaut has taken it away already
        } else if (type == QLatin1String("refresh") || type == QLatin1String("watch")) {
            // Something in the project may have changed (a .gitignore saved, a build made).
            const QString folder = message.value(QLatin1String("folder")).toString();
            if (!m_project.isEmpty() && (folder == m_project || folder.startsWith(m_project + QLatin1Char('/'))))
                m_later.start();
        }
    }

private:
    void invoke(const QJsonObject &message)
    {
        const QString command = message.value(QLatin1String("command")).toString();
        const QString folder = message.value(QLatin1String("folder")).toString();
        const QJsonArray selection = message.value(QLatin1String("selection")).toArray();
        if (command == QLatin1String("open")) {
            // The selected folder, or the open one.
            const QString chosen = selection.size() == 1 ? selection.first().toString() : folder;
            if (chosen.isEmpty() || !QFileInfo(chosen).isDir()) {
                say(QStringLiteral("Open a folder first: Recent and Starred aren't folders"));
                return;
            }
            open(chosen, true);
        } else if (command == QLatin1String("close")) {
            close(true);
        } else if (command == QLatin1String("recent")) {
            askRecent();
        }
    }

    // The recent projects, to choose one from (a form: the same whether this
    // program was running or was started for it).
    void askRecent()
    {
        m_choices.clear();
        QJsonArray labels;
        for (const QString &folder : std::as_const(m_recent)) {
            if (!QFileInfo(folder).isDir())
                continue;
            const QString label = QFileInfo(folder).fileName() + QStringLiteral("  (") + QDir::toNativeSeparators(folder) + QLatin1Char(')');
            m_choices.append({label, folder});
            labels.append(label);
        }
        if (m_choices.isEmpty()) {
            say(QStringLiteral("No recent projects yet: File → Open as Project opens the folder you are in"));
            return;
        }
        send({{QStringLiteral("type"), QStringLiteral("form")},
              {QStringLiteral("id"), QStringLiteral("recent")},
              {QStringLiteral("title"), QStringLiteral("Open Recent Project")},
              {QStringLiteral("heading"), QStringLiteral("Which project?")},
              {QStringLiteral("fields"), QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("project")},
                                                                {QStringLiteral("kind"), QStringLiteral("choice")},
                                                                {QStringLiteral("label"), QStringLiteral("Project")},
                                                                {QStringLiteral("choices"), labels},
                                                                {QStringLiteral("value"), labels.first()}}}},
              {QStringLiteral("buttons"), QJsonArray{QStringLiteral("Open")}}});
    }

    void answered(const QJsonObject &message)
    {
        if (message.value(QLatin1String("id")).toString() != QLatin1String("recent") || message.value(QLatin1String("cancelled")).toBool())
            return;
        const QString chosen = message.value(QLatin1String("values")).toObject().value(QLatin1String("project")).toString();
        for (const auto &choice : std::as_const(m_choices)) {
            if (choice.first == chosen && QFileInfo(choice.second).isDir()) {
                open(choice.second, true);
                return;
            }
        }
    }

    void open(const QString &folder, bool chosen)
    {
        m_project = clean(folder);
        m_recent.removeAll(m_project);
        m_recent.prepend(m_project);
        while (m_recent.size() > MaxRecent)
            m_recent.removeLast();
        save();
        sendCommands();
        m_git.setFolder(m_project); // the ignored files once the repository is checked (changed)
        sendTree(); // at once: the tree, with what is faint to come
        if (chosen)
            say(QStringLiteral("“%1” is open as a project, under the places").arg(QFileInfo(m_project).fileName()));
    }

    void close(bool tellArgonaut)
    {
        if (m_project.isEmpty())
            return;
        if (tellArgonaut) // no folder: the tree goes
            send({{QStringLiteral("type"), QStringLiteral("tree")}, {QStringLiteral("id"), QStringLiteral("project")}});
        m_project.clear();
        m_git.setFolder(QString());
        save();
        sendCommands();
    }

    void save()
    {
        QSettings settings;
        settings.setValue(QStringLiteral("project"), m_project);
        settings.setValue(QStringLiteral("recent"), m_recent);
    }

    // What is faint: what Git ignores, by paths relative to the project.
    QJsonArray faint() const
    {
        QJsonArray out;
        if (m_git.inRepository() && m_git.ready() && !m_git.blocked()) {
            bool ok = false;
            const QByteArray listed = m_git.read({QStringLiteral("ls-files"), QStringLiteral("--others"), QStringLiteral("--ignored"),
                                                  QStringLiteral("--exclude-standard"), QStringLiteral("--directory"), QStringLiteral("-z")},
                                                 // (the whole repository: no path; GitStatus takes only paths inside it)
                                                 m_project == m_git.root() ? QStringList() : QStringList{m_project}, &ok, 4 * 1024 * 1024);
            if (ok) {
                // Git names them from its top folder; the tree's are from the project.
                const QString root = m_git.root();
                const QString prefix = m_project == root ? QString() : QDir(root).relativeFilePath(m_project) + QLatin1Char('/');
                for (const QByteArray &entry : listed.split('\0')) {
                    if (out.size() >= MaxFaint)
                        break;
                    const QString path = QString::fromUtf8(entry);
                    if (path.isEmpty() || !path.startsWith(prefix))
                        continue;
                    out.append(path.mid(prefix.size()));
                }
                return out;
            }
        }
        if (!m_git.inRepository()) {
            for (const QString &name : kMadeFolders) {
                if (QFileInfo(m_project + QLatin1Char('/') + name).isDir())
                    out.append(name);
            }
        }
        return out;
    }

    void sendTree()
    {
        if (m_project.isEmpty())
            return;
        if (!QFileInfo(m_project).isDir()) { // it went away
            close(true);
            return;
        }
        QString tooltip = m_project;
        if (m_git.inRepository() && !m_git.branch().isEmpty())
            tooltip += QStringLiteral("\nGit: %1").arg(m_git.branch());
        if (m_git.blocked())
            tooltip += QStringLiteral("\nGit is off here (its settings would run programs): nothing is faint");
        send({{QStringLiteral("type"), QStringLiteral("tree")},
              {QStringLiteral("id"), QStringLiteral("project")},
              {QStringLiteral("title"), QFileInfo(m_project).fileName()},
              {QStringLiteral("tooltip"), tooltip},
              {QStringLiteral("folder"), m_project},
              {QStringLiteral("faint"), faint()},
              {QStringLiteral("hidden"), QJsonArray{QStringLiteral(".git")}}});
    }

    void sendCommands()
    {
        QJsonArray commands;
        auto add = [&](QJsonObject command) { commands.append(command); };
        add({{QStringLiteral("id"), QStringLiteral("open")}, {QStringLiteral("label"), QStringLiteral("Open as Project")},
             {QStringLiteral("menu"), QStringLiteral("file")}});
        // A folder's empty space, and one folder right-clicked.
        add({{QStringLiteral("id"), QStringLiteral("open")}, {QStringLiteral("label"), QStringLiteral("Open as Project")},
             {QStringLiteral("target"), QStringLiteral("folder")}});
        add({{QStringLiteral("id"), QStringLiteral("open")}, {QStringLiteral("label"), QStringLiteral("Open as Project")},
             {QStringLiteral("kind"), QStringLiteral("folders")}, {QStringLiteral("multiple"), false}});
        add({{QStringLiteral("id"), QStringLiteral("recent")}, {QStringLiteral("label"), QStringLiteral("Open Recent Project…")},
             {QStringLiteral("menu"), QStringLiteral("file")}});
        if (!m_project.isEmpty())
            add({{QStringLiteral("id"), QStringLiteral("close")}, {QStringLiteral("label"), QStringLiteral("Close Project")},
                 {QStringLiteral("menu"), QStringLiteral("file")}});
        send({{QStringLiteral("type"), QStringLiteral("commands")}, {QStringLiteral("commands"), commands}});
    }

    GitStatus m_git;
    QTimer m_later;
    QString m_project;
    QStringList m_recent;
    QList<QPair<QString, QString>> m_choices; // the form's labels and their folders
};

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    // Its own settings file: the project open now and the recent ones.
    QCoreApplication::setOrganizationName(QStringLiteral("Argonaut"));
    QCoreApplication::setApplicationName(QStringLiteral("project-explorer"));

    Explorer explorer;
    Conversation conversation([&](const QJsonObject &message) { explorer.handle(message); });
    return app.exec();
}
