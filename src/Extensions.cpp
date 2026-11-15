#include "Extensions.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

namespace Extensions {

namespace {

// Limits on what an extension may send: it is someone else's program.
constexpr int MaxLineBytes = 8 * 1024 * 1024;
constexpr int MaxBadgesPerFolder = 50000;
constexpr int MaxTooltipChars = 300;
constexpr int MaxInfoChars = 60;
constexpr int MaxCommands = 20;   // per extension, everywhere and per folder
constexpr int MaxLabelChars = 60;
constexpr int MaxMessageChars = 200;
constexpr int MaxFailures = 3; // then it stays off until switched on again or restart

const QStringList &colors()
{
    static const QStringList names = {QStringLiteral("warning"), QStringLiteral("success"), QStringLiteral("error"),
                                      QStringLiteral("accent"), QStringLiteral("dim")};
    return names;
}

// One line of plain text, cut to size.
QString tidy(const QString &text, int maxChars)
{
    QString out = text.left(maxChars);
    for (QChar &c : out) {
        if (c == QLatin1Char('\r') || c == QLatin1Char('\t') || (c.unicode() < 0x20 && c != QLatin1Char('\n')))
            c = QLatin1Char(' ');
    }
    return out.trimmed();
}

// A path inside a folder: relative, no "..", no leading or trailing '/'.
bool isInsidePath(const QString &key)
{
    return !(key.startsWith(QLatin1Char('/')) || key.endsWith(QLatin1Char('/')) || key == QLatin1String("..")
             || key.startsWith(QLatin1String("../")) || key.contains(QLatin1String("/../"))
             || key.endsWith(QLatin1String("/..")));
}

bool typeMatches(const QString &mimeType, const QStringList &patterns)
{
    if (patterns.isEmpty())
        return true;
    static const QMimeDatabase db;
    for (const QString &pattern : patterns) {
        if (pattern == mimeType || pattern == QLatin1String("*/*"))
            return true;
        if (pattern.endsWith(QLatin1String("/*")) && mimeType.startsWith(QStringView(pattern).left(pattern.size() - 1)))
            return true;
        if (!pattern.contains(QLatin1Char('*')) && db.mimeTypeForName(mimeType).inherits(pattern))
            return true;
    }
    return false;
}

QString enabledKey(const QString &id)
{
    return QStringLiteral("extensions/%1/enabled").arg(id);
}

// Folders that hold extensions: (folder, came with Argonaut).
QList<QPair<QString, bool>> searchFolders()
{
    QList<QPair<QString, bool>> folders;
    // Tests (and people trying an extension) name the folders themselves;
    // nothing else is looked at then. Those extensions start switched off.
    if (qEnvironmentVariableIsSet("ARGONAUT_EXTENSIONS_PATH")) {
        const QStringList given = qEnvironmentVariable("ARGONAUT_EXTENSIONS_PATH").split(QLatin1Char(':'), Qt::SkipEmptyParts);
        for (const QString &folder : given)
            folders.append({folder, false});
        return folders;
    }
    const QString user = userFolder();
    folders.append({user, false});
    // Next to the program (a build folder), then the system's data folders.
    folders.append({QCoreApplication::applicationDirPath() + QStringLiteral("/extensions"), true});
    const QStringList system = QStandardPaths::locateAll(QStandardPaths::GenericDataLocation,
                                                         QStringLiteral("argonaut/extensions"),
                                                         QStandardPaths::LocateDirectory);
    for (const QString &folder : system) {
        if (QDir::cleanPath(folder) != QDir::cleanPath(user))
            folders.append({folder, true});
    }
    return folders;
}

} // namespace

QString userFolder()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/argonaut/extensions");
}

Manifest readManifest(const QString &folder, bool builtIn)
{
    Manifest m;
    m.folder = QDir::cleanPath(folder);
    m.builtIn = builtIn;
    m.id = QFileInfo(m.folder).fileName();
    m.name = m.id;
    QFile file(m.folder + QStringLiteral("/extension.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        m.problem = QCoreApplication::translate("Extensions", "It has no extension.json.");
        return m;
    }
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(file.read(256 * 1024), &error);
    if (!doc.isObject()) {
        m.problem = QCoreApplication::translate("Extensions", "Its extension.json can't be read: %1").arg(error.errorString());
        return m;
    }
    const QJsonObject o = doc.object();
    const QString id = o.value(QLatin1String("id")).toString();
    static const QRegularExpression goodId(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$"));
    if (!goodId.match(id).hasMatch()) {
        m.problem = QCoreApplication::translate("Extensions", "Its \"id\" is missing or has characters other than letters, digits, '.', '-' and '_'.");
        return m;
    }
    m.id = id;
    m.name = tidy(o.value(QLatin1String("name")).toString(id), 80);
    if (m.name.isEmpty())
        m.name = id;
    m.description = tidy(o.value(QLatin1String("description")).toString(), 400);
    m.version = tidy(o.value(QLatin1String("version")).toString(), 40);
    m.author = tidy(o.value(QLatin1String("author")).toString(), 80);
    const int protocol = o.value(QLatin1String("protocol")).toInt(0);
    if (protocol != ProtocolVersion) {
        m.problem = QCoreApplication::translate("Extensions", "It is made for another version of Argonaut (protocol %1, this is %2).")
                        .arg(protocol)
                        .arg(ProtocolVersion);
        return m;
    }
    // "exec": the program and its arguments. A relative program is inside
    // the extension's folder.
    QStringList exec;
    const QJsonValue execValue = o.value(QLatin1String("exec"));
    if (execValue.isString()) {
        exec << execValue.toString();
    } else {
        const QJsonArray parts = execValue.toArray();
        for (const QJsonValue &part : parts)
            exec << part.toString();
    }
    if (exec.isEmpty() || exec.first().isEmpty()) {
        m.problem = QCoreApplication::translate("Extensions", "Its \"exec\" (the program to start) is missing.");
        return m;
    }
    QString program = exec.takeFirst();
    if (!QDir::isAbsolutePath(program))
        program = QDir::cleanPath(m.folder + QLatin1Char('/') + program);
    m.program = program;
    m.arguments = exec;
    const QFileInfo info(program);
    if (!info.isFile() || !info.isExecutable())
        m.problem = QCoreApplication::translate("Extensions", "Its program is missing or not executable: %1").arg(program);
    return m;
}

Host *Host::instance()
{
    // Never deleted: models ask it while they are taken down, in any order.
    // Its programs are stopped when the application quits.
    static Host *host = [] {
        auto *h = new Host;
        QObject::connect(qApp, &QCoreApplication::aboutToQuit, h, &Host::shutdown);
        return h;
    }();
    return host;
}

Host::Host(QObject *parent) : QObject(parent)
{
    qRegisterMetaType<Extensions::Badge>();
    rescan();
}

Host::~Host()
{
    for (Running *ext : std::as_const(m_running)) {
        stop(ext);
        delete ext;
    }
}

void Host::shutdown()
{
    m_quitting = true;
    for (Running *ext : std::as_const(m_running))
        stop(ext);
}

QList<Manifest> Host::extensions() const
{
    return m_manifests;
}

void Host::rescan()
{
    QList<Manifest> found;
    QSet<QString> ids;
    for (const auto &[folder, builtIn] : searchFolders()) {
        const QFileInfoList entries = QDir(folder).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo &entry : entries) {
            Manifest m = readManifest(entry.absoluteFilePath(), builtIn);
            if (ids.contains(m.id))
                continue; // the first one found wins (the user's copy before ours)
            ids.insert(m.id);
            found.append(m);
        }
    }
    m_manifests = found;

    // An extension that is gone is no longer switched on: if a folder with
    // its name turns up again (maybe another program), it starts off and has
    // to be switched on again. "Off" is kept: it can only make things safer.
    {
        QSettings settings;
        settings.beginGroup(QStringLiteral("extensions"));
        const QStringList known = settings.childGroups();
        for (const QString &id : known) {
            if (!ids.contains(id) && settings.value(id + QStringLiteral("/enabled")).toBool())
                settings.remove(id + QStringLiteral("/enabled"));
        }
    }

    // Stop what is gone, switched off or changed; start what is new.
    QList<Running *> keep;
    for (Running *ext : std::as_const(m_running)) {
        bool still = false;
        for (const Manifest &m : std::as_const(m_manifests)) {
            if (m.id == ext->manifest.id && m.program == ext->manifest.program
                && m.arguments == ext->manifest.arguments && m.isUsable() && isEnabled(m.id)) {
                still = true;
                break;
            }
        }
        if (still) {
            keep.append(ext);
        } else {
            stop(ext);
            forget(ext);
            delete ext;
        }
    }
    m_running = keep;
    for (const Manifest &m : std::as_const(m_manifests)) {
        if (!m.isUsable() || !isEnabled(m.id))
            continue;
        bool have = false;
        for (Running *ext : std::as_const(m_running))
            have = have || ext->manifest.id == m.id;
        if (!have) {
            auto *ext = new Running;
            ext->manifest = m;
            m_running.append(ext);
        }
    }
    startAsNeeded();
    emit extensionsChanged();
}

bool Host::isEnabled(const QString &id) const
{
    bool builtIn = false;
    for (const Manifest &m : m_manifests) {
        if (m.id == id)
            builtIn = m.builtIn;
    }
    QSettings settings;
    // What came with Argonaut is on until switched off; anything else is
    // off until switched on. (Git badges had their own switch before.)
    bool fallback = builtIn;
    if (builtIn && id == QLatin1String("git"))
        fallback = settings.value(QStringLiteral("git/badges"), true).toBool();
    return settings.value(enabledKey(id), fallback).toBool();
}

void Host::setEnabled(const QString &id, bool on)
{
    QSettings().setValue(enabledKey(id), on);
    for (Running *ext : std::as_const(m_running)) {
        if (ext->manifest.id == id)
            ext->failures = 0; // switched on again: another try
    }
    rescan();
}

void Host::watch(const QString &folder)
{
    if (folder.isEmpty())
        return;
    if (++m_watched[folder] > 1)
        return;
    startAsNeeded();
    for (Running *ext : std::as_const(m_running)) {
        if (ext->process)
            send(ext, {{QStringLiteral("type"), QStringLiteral("watch")}, {QStringLiteral("folder"), folder}});
    }
}

void Host::unwatch(const QString &folder)
{
    auto it = m_watched.find(folder);
    if (it == m_watched.end())
        return;
    if (--it.value() > 0)
        return;
    m_watched.erase(it);
    for (Running *ext : std::as_const(m_running)) {
        ext->folders.remove(folder);
        if (ext->process)
            send(ext, {{QStringLiteral("type"), QStringLiteral("unwatch")}, {QStringLiteral("folder"), folder}});
    }
}

void Host::refresh(const QString &folder)
{
    if (!m_watched.contains(folder))
        return;
    for (Running *ext : std::as_const(m_running)) {
        if (ext->process)
            send(ext, {{QStringLiteral("type"), QStringLiteral("refresh")}, {QStringLiteral("folder"), folder}});
    }
}

void Host::refreshAll()
{
    for (auto it = m_watched.constBegin(); it != m_watched.constEnd(); ++it)
        refresh(it.key());
}

Badge Host::badge(const QString &folder, const QString &path) const
{
    if (m_running.isEmpty())
        return {};
    const int prefix = folder.endsWith(QLatin1Char('/')) ? int(folder.size()) : int(folder.size()) + 1;
    if (path.size() <= prefix || !path.startsWith(folder))
        return {};
    const QString relative = path.mid(prefix);
    for (const Running *ext : m_running) {
        const auto data = ext->folders.constFind(folder);
        if (data == ext->folders.constEnd() || data->badges.isEmpty())
            continue;
        if (const auto it = data->badges.constFind(relative); it != data->badges.constEnd())
            return *it;
        if (!data->anyDeep)
            continue;
        // A mark on a folder above that is for everything inside it.
        QStringView above(relative);
        while (true) {
            const qsizetype slash = above.lastIndexOf(QLatin1Char('/'));
            above = slash < 0 ? QStringView() : above.left(slash);
            const auto it = data->badges.constFind(above.toString());
            if (it != data->badges.constEnd() && it->deep)
                return *it;
            if (above.isEmpty())
                break;
        }
    }
    return {};
}

Info Host::info(const QString &folder) const
{
    for (const Running *ext : m_running) {
        const auto data = ext->folders.constFind(folder);
        if (data != ext->folders.constEnd() && !data->info.text.isEmpty())
            return data->info;
    }
    return {};
}

QList<Command> Host::readCommands(const Running *ext, const QJsonValue &value) const
{
    QList<Command> out;
    const QJsonArray list = value.toArray();
    for (const QJsonValue &entry : list) {
        if (out.size() >= MaxCommands)
            break;
        const QJsonObject o = entry.toObject();
        Command c;
        c.extension = ext->manifest.id;
        c.id = o.value(QLatin1String("id")).toString().left(64);
        c.label = tidy(o.value(QLatin1String("label")).toString(), MaxLabelChars).replace(QLatin1Char('\n'), QLatin1Char(' '));
        if (c.id.isEmpty() || c.label.isEmpty())
            continue;
        c.confirm = tidy(o.value(QLatin1String("confirm")).toString(), MaxTooltipChars);
        c.onFolder = o.value(QLatin1String("target")).toString() == QLatin1String("folder");
        const QString kind = o.value(QLatin1String("kind")).toString();
        c.kind = kind == QLatin1String("files") ? Command::Files
                 : kind == QLatin1String("folders") ? Command::Folders : Command::Any;
        c.multiple = o.value(QLatin1String("multiple")).toBool(true);
        const QJsonArray types = o.value(QLatin1String("types")).toArray();
        for (const QJsonValue &t : types) {
            if (c.types.size() < 50 && !t.toString().isEmpty())
                c.types << t.toString().left(100);
        }
        if (o.contains(QLatin1String("items"))) {
            c.limited = true;
            const QJsonArray items = o.value(QLatin1String("items")).toArray();
            for (const QJsonValue &item : items) {
                if (c.items.size() >= MaxBadgesPerFolder)
                    break;
                if (isInsidePath(item.toString()))
                    c.items.insert(item.toString());
            }
        }
        out.append(c);
    }
    return out;
}

QList<Command> Host::commands(const QString &folder, const QList<Item> &selection) const
{
    QList<Command> out;
    const QString prefix = folder.endsWith(QLatin1Char('/')) ? folder : folder + QLatin1Char('/');
    auto fits = [&](const Command &c) {
        if (c.onFolder)
            return selection.isEmpty() && !folder.isEmpty();
        if (selection.isEmpty() || (!c.multiple && selection.size() != 1))
            return false;
        for (const Item &item : selection) {
            if ((c.kind == Command::Files && item.isDir) || (c.kind == Command::Folders && !item.isDir))
                return false;
            if (!typeMatches(item.isDir ? QStringLiteral("inode/directory") : item.mimeType, c.types))
                return false;
            if (c.limited && (folder.isEmpty() || !item.path.startsWith(prefix) || !c.items.contains(item.path.mid(prefix.size()))))
                return false;
        }
        return true;
    };
    for (const Running *ext : m_running) {
        if (!ext->process)
            continue;
        for (const Command &c : ext->commands) {
            if (fits(c))
                out.append(c);
        }
        if (folder.isEmpty())
            continue;
        const auto data = ext->folders.constFind(folder);
        if (data == ext->folders.constEnd())
            continue;
        for (const Command &c : data->commands) {
            if (fits(c))
                out.append(c);
        }
    }
    return out;
}

void Host::invoke(const Command &command, const QString &folder, const QStringList &selection)
{
    for (Running *ext : std::as_const(m_running)) {
        if (ext->manifest.id != command.extension || !ext->process)
            continue;
        send(ext, {{QStringLiteral("type"), QStringLiteral("invoke")},
                   {QStringLiteral("command"), command.id},
                   {QStringLiteral("folder"), folder},
                   {QStringLiteral("selection"), QJsonArray::fromStringList(selection)}});
    }
}

QString Host::nameOf(const QString &id) const
{
    for (const Manifest &m : m_manifests) {
        if (m.id == id)
            return m.name;
    }
    return id;
}

bool Host::isRunning(const QString &id) const
{
    for (const Running *ext : m_running) {
        if (ext->manifest.id == id)
            return ext->process && ext->process->state() == QProcess::Running;
    }
    return false;
}

void Host::startAsNeeded()
{
    if (m_watched.isEmpty() || m_quitting)
        return; // nothing to ask yet: programs start with the first folder
    for (Running *ext : std::as_const(m_running)) {
        if (!ext->process && ext->failures < MaxFailures)
            start(ext);
    }
}

void Host::start(Running *ext)
{
    auto *process = new QProcess(this);
    ext->process = process;
    ext->buffer.clear();
    ext->stopping = false;
    process->setWorkingDirectory(ext->manifest.folder);
    // Our environment, without what is only ours to know.
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.remove(QStringLiteral("ANTHROPIC_API_KEY"));
    env.insert(QStringLiteral("ARGONAUT_EXTENSION_ID"), ext->manifest.id);
    process->setProcessEnvironment(env);
    process->setProcessChannelMode(QProcess::SeparateChannels);
    process->setStandardErrorFile(QProcess::nullDevice());
    connect(process, &QProcess::readyReadStandardOutput, this, [this, ext, process] {
        if (ext->process == process)
            readFrom(ext);
    });
    connect(process, &QProcess::finished, this, [this, ext, process] {
        if (ext->process == process)
            exited(ext);
        process->deleteLater();
    });
    connect(process, &QProcess::errorOccurred, this, [this, ext, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && ext->process == process) {
            exited(ext);
            process->deleteLater();
        }
    });
    process->start(ext->manifest.program, ext->manifest.arguments);
    send(ext, {{QStringLiteral("type"), QStringLiteral("hello")},
               {QStringLiteral("protocol"), ProtocolVersion},
               {QStringLiteral("app"), QStringLiteral("argonaut")},
               {QStringLiteral("version"), QCoreApplication::applicationVersion()}});
    for (auto it = m_watched.constBegin(); it != m_watched.constEnd(); ++it)
        send(ext, {{QStringLiteral("type"), QStringLiteral("watch")}, {QStringLiteral("folder"), it.key()}});
}

void Host::stop(Running *ext)
{
    if (!ext->process)
        return;
    ext->stopping = true;
    QProcess *process = ext->process;
    ext->process = nullptr;
    process->disconnect(this);
    process->closeWriteChannel(); // end of input: a well-behaved one leaves
    if (!process->waitForFinished(300)) {
        process->kill();
        process->waitForFinished(500);
    }
    process->deleteLater();
}

void Host::send(Running *ext, const QJsonObject &message)
{
    if (!ext->process)
        return;
    ext->process->write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
}

void Host::readFrom(Running *ext)
{
    ext->buffer += ext->process->readAllStandardOutput();
    while (ext->process) {
        const qsizetype end = ext->buffer.indexOf('\n');
        if (end < 0)
            break;
        const QByteArray line = ext->buffer.left(end);
        ext->buffer.remove(0, end + 1);
        if (!line.trimmed().isEmpty())
            handle(ext, line);
    }
    if (ext->process && ext->buffer.size() > MaxLineBytes) {
        // A line without end: not something to keep in memory.
        ext->failures = MaxFailures;
        stop(ext);
        forget(ext);
    }
}

void Host::handle(Running *ext, const QByteArray &line)
{
    const QJsonDocument doc = QJsonDocument::fromJson(line);
    if (!doc.isObject())
        return; // not ours to understand: skipped
    const QJsonObject message = doc.object();
    const QString type = message.value(QLatin1String("type")).toString();
    if (type == QLatin1String("commands")) { // what it offers everywhere
        ext->commands = readCommands(ext, message.value(QLatin1String("commands")));
        return;
    }
    if (type == QLatin1String("message")) { // something for the user to read
        const QString text = tidy(message.value(QLatin1String("text")).toString(), MaxMessageChars)
                                 .replace(QLatin1Char('\n'), QLatin1Char(' '));
        if (!text.isEmpty())
            emit this->message(ext->manifest.name, text);
        return;
    }
    if (type != QLatin1String("folder"))
        return;
    const QString folder = message.value(QLatin1String("folder")).toString();
    if (!m_watched.contains(folder))
        return; // about a folder nobody shows (any more)

    FolderData data;
    const QJsonObject info = message.value(QLatin1String("info")).toObject();
    data.info.text = tidy(info.value(QLatin1String("text")).toString(), MaxInfoChars).replace(QLatin1Char('\n'), QLatin1Char(' '));
    data.info.tooltip = tidy(info.value(QLatin1String("tooltip")).toString(), MaxTooltipChars);
    data.info.icon = info.value(QLatin1String("icon")).toString() == QLatin1String("branch") ? QStringLiteral("branch")
                                                                                             : QString();
    const QJsonObject badges = message.value(QLatin1String("badges")).toObject();
    int count = 0;
    for (auto it = badges.constBegin(); it != badges.constEnd() && count < MaxBadgesPerFolder; ++it) {
        const QString key = it.key();
        if (!isInsidePath(key))
            continue;
        const QJsonObject o = it.value().toObject();
        Badge badge;
        badge.valid = true;
        badge.text = tidy(o.value(QLatin1String("text")).toString(), 2).remove(QLatin1Char('\n'));
        badge.color = o.value(QLatin1String("color")).toString();
        if (!colors().contains(badge.color))
            badge.color = QStringLiteral("dim");
        badge.tooltip = tidy(o.value(QLatin1String("tooltip")).toString(), MaxTooltipChars);
        badge.deep = o.value(QLatin1String("deep")).toBool();
        data.anyDeep = data.anyDeep || badge.deep;
        data.badges.insert(key, badge);
        ++count;
    }
    data.commands = readCommands(ext, message.value(QLatin1String("commands")));
    ext->folders.insert(folder, data);
    emit folderChanged(folder);
}

void Host::exited(Running *ext)
{
    // It stopped by itself (a crash, or it couldn't start): what it said
    // goes, and it gets a few more tries.
    ext->process = nullptr;
    ext->buffer.clear();
    ext->commands.clear();
    forget(ext);
    if (ext->stopping)
        return;
    ++ext->failures;
    if (ext->failures < MaxFailures) {
        const QString id = ext->manifest.id;
        QTimer::singleShot(1000 * ext->failures, this, [this, id] {
            for (Running *running : std::as_const(m_running)) {
                if (running->manifest.id == id && !running->process && !m_watched.isEmpty() && !m_quitting)
                    start(running);
            }
        });
    }
}

void Host::forget(Running *ext)
{
    const QStringList folders = ext->folders.keys();
    ext->folders.clear();
    for (const QString &folder : folders)
        emit folderChanged(folder);
}

} // namespace Extensions
