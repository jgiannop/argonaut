#include "Extensions.h"
#include "Organizer.h"

#include <QCoreApplication>
#include <QDateTime>
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
#include <QUrl>

namespace Extensions {

namespace {

// Limits on what an extension may send: it is someone else's program.
constexpr int MaxLineBytes = 8 * 1024 * 1024;
constexpr int MaxBadgesPerFolder = 50000;
constexpr int MaxTooltipChars = 300;
constexpr int MaxInfoChars = 60;
constexpr int MaxCommands = 20;   // per extension, everywhere
constexpr int MaxFolderCommands = 80; // per extension and folder (a list of branches is commands)
constexpr int MaxLabelChars = 60;
constexpr int MaxMessageChars = 200;
constexpr int MaxPreviewChars = 1024 * 1024; // text, Markdown or HTML of a preview
constexpr int MaxListingFiles = 20000;
constexpr int MaxListings = 20;   // per extension
constexpr int MaxPlaces = 20;     // sidebar rows per extension
constexpr int MaxClaudePerAction = 12;      // questions to Claude for one thing the user chose
constexpr int MaxClaudeTokens = 16000;      // the most an extension may ask Claude to write in one answer
constexpr int OpenWithinMs = 5 * 60 * 1000; // "open" is an answer: only this long after the user chose something
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

QList<Command> commandsFrom(const QString &extension, const QJsonValue &value, int max);

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
    // "addresses": the kinds of address that are this extension's to open.
    // Never ours (folders, the places) and never the web's.
    static const QRegularExpression goodScheme(QStringLiteral("^[a-z][a-z0-9+.-]{1,30}$"));
    static const QStringList reserved{QStringLiteral("file"), QStringLiteral("recent"), QStringLiteral("starred"), QStringLiteral("listing"),
                                      QStringLiteral("extension-place"), QStringLiteral("trash"), QStringLiteral("connect"),
                                      QStringLiteral("http"), QStringLiteral("https"), QStringLiteral("topic")};
    const QJsonArray addresses = o.value(QLatin1String("addresses")).toArray();
    for (const QJsonValue &value : addresses) {
        const QString scheme = value.toString().toLower();
        if (m.addresses.size() < 30 && goodScheme.match(scheme).hasMatch() && !reserved.contains(scheme) && !m.addresses.contains(scheme))
            m.addresses << scheme;
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
    // "uses": what of Argonaut's it asks for beyond showing things.
    const QJsonArray uses = o.value(QLatin1String("uses")).toArray();
    for (const QJsonValue &use : uses)
        m.usesClaude = m.usesClaude || use.toString() == QLatin1String("claude");
    // "start": "when-needed", with what it offers named here.
    m.whenNeeded = o.value(QLatin1String("start")).toString() == QLatin1String("when-needed");
    m.commands = commandsFrom(m.id, o.value(QLatin1String("commands")), MaxCommands);
    const QJsonArray previews = o.value(QLatin1String("previews")).toArray();
    for (const QJsonValue &type : previews) {
        if (m.previewTypes.size() < 100 && !type.toString().isEmpty())
            m.previewTypes << type.toString().left(100);
    }
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
    qRegisterMetaType<Extensions::PreviewReply>();
    qRegisterMetaType<Extensions::Form>();
    // How long a when-needed extension may sit with nothing to do.
    bool ok = false;
    const int idle = qEnvironmentVariableIntValue("ARGONAUT_EXTENSION_IDLE_MS", &ok);
    if (ok && idle > 0)
        m_idleMs = idle;
    auto *idleTimer = new QTimer(this);
    idleTimer->setInterval(qBound(50, m_idleMs / 3, 20000));
    connect(idleTimer, &QTimer::timeout, this, &Host::stopIdle);
    idleTimer->start();
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

// The content of a "preview" or "show" message: text or a picture, checked.
PreviewReply Host::readContent(const QJsonObject &message)
{
    PreviewReply reply;
    const QString kind = message.value(QLatin1String("kind")).toString();
    reply.text = message.value(QLatin1String("text")).toString().left(MaxPreviewChars);
    reply.monospace = message.value(QLatin1String("monospace")).toBool();
    reply.note = tidy(message.value(QLatin1String("note")).toString(), MaxMessageChars).replace(QLatin1Char('\n'), QLatin1Char(' '));
    if (kind == QLatin1String("text")) {
        reply.kind = PreviewReply::Text;
    } else if (kind == QLatin1String("markdown")) {
        reply.kind = PreviewReply::Markdown;
    } else if (kind == QLatin1String("html")) {
        reply.kind = PreviewReply::Html;
    } else if (kind == QLatin1String("diff")) {
        reply.kind = PreviewReply::Diff;
    } else if (kind == QLatin1String("ansi")) {
        reply.kind = PreviewReply::Ansi;
    } else if (kind == QLatin1String("image")) {
        // A picture file of its own making: an absolute path to a plain file.
        const QString image = message.value(QLatin1String("image")).toString();
        const QFileInfo info(image);
        if (QDir::isAbsolutePath(image) && info.isFile() && !info.isSymLink()) {
            reply.kind = PreviewReply::Image;
            reply.image = image;
            reply.scroll = message.value(QLatin1String("scroll")).toBool();
        }
    }
    return reply;
}

QList<Command> Host::readCommands(const Running *ext, const QJsonValue &value, int max) const
{
    return commandsFrom(ext->manifest.id, value, max);
}

QList<Command> commandsFrom(const QString &extension, const QJsonValue &value, int max)
{
    QList<Command> out;
    const QJsonArray list = value.toArray();
    for (const QJsonValue &entry : list) {
        if (out.size() >= max)
            break;
        const QJsonObject o = entry.toObject();
        Command c;
        c.extension = extension;
        c.id = o.value(QLatin1String("id")).toString().left(64);
        c.label = tidy(o.value(QLatin1String("label")).toString(), MaxLabelChars).replace(QLatin1Char('\n'), QLatin1Char(' '));
        if (c.id.isEmpty() || c.label.isEmpty())
            continue;
        c.confirm = tidy(o.value(QLatin1String("confirm")).toString(), MaxTooltipChars);
        // "Git" or "Git/Switch Branch": at most two levels of submenu.
        const QStringList levels = o.value(QLatin1String("group")).toString().split(QLatin1Char('/'), Qt::SkipEmptyParts);
        QStringList group;
        for (const QString &level : levels.mid(0, 2)) {
            const QString name = tidy(level, MaxLabelChars).replace(QLatin1Char('\n'), QLatin1Char(' '));
            if (!name.isEmpty())
                group << name;
        }
        c.group = group.join(QLatin1Char('/'));
        c.checked = o.value(QLatin1String("checked")).toBool();
        if (o.value(QLatin1String("ask")).isObject()) {
            const QJsonObject ask = o.value(QLatin1String("ask")).toObject();
            c.asks = true;
            c.askLabel = tidy(ask.value(QLatin1String("label")).toString(), MaxTooltipChars);
            c.askText = ask.value(QLatin1String("text")).toString().left(2000);
            c.askMultiline = ask.value(QLatin1String("multiline")).toBool();
        }
        c.onFolder = o.value(QLatin1String("target")).toString() == QLatin1String("folder");
        if (o.contains(QLatin1String("menu"))) {
            // A place in the menu bar. One we don't have: the command is
            // left out, not put somewhere its extension didn't ask for.
            c.menu = o.value(QLatin1String("menu")).toString();
            static const QStringList known{QStringLiteral("file"), QStringLiteral("edit"), QStringLiteral("view"), QStringLiteral("go")};
            if (!known.contains(c.menu))
                continue;
        }
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
        if (!c.menu.isEmpty())
            return false; // in the menu bar, not here
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
        if (!isAvailable(ext))
            continue;
        const QList<Command> everywhere = commandsOf(ext);
        for (const Command &c : everywhere) {
            if (fits(c))
                out.append(c);
        }
        if (folder.isEmpty() || !ext->process)
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

QList<Command> Host::menuCommands(const QString &menu) const
{
    QList<Command> out;
    for (const Running *ext : m_running) {
        if (!isAvailable(ext))
            continue;
        const QList<Command> everywhere = commandsOf(ext);
        for (const Command &c : everywhere) {
            if (c.menu == menu)
                out.append(c);
        }
    }
    return out;
}

// ------------------------------------------------- started when needed ---

bool Host::isAvailable(const Running *ext) const
{
    return ext->process || (ext->manifest.whenNeeded && ext->failures < MaxFailures && !m_quitting);
}

QList<Command> Host::commandsOf(const Running *ext) const
{
    return ext->process && ext->commandsSaid ? ext->commands : ext->manifest.commands;
}

QStringList Host::previewTypesOf(const Running *ext) const
{
    return ext->process && ext->previewsSaid ? ext->previewTypes : ext->manifest.previewTypes;
}

// Something of a when-needed extension is used: its program is started if
// it isn't running (what is sent next waits in the pipe for it), and it
// isn't idle.
void Host::wake(Running *ext)
{
    ext->lastActive = QDateTime::currentMSecsSinceEpoch();
    if (!ext->process && ext->manifest.whenNeeded && ext->failures < MaxFailures && !m_quitting)
        start(ext);
}

// A when-needed extension with nothing on screen, nothing asked and nothing
// said for a while is stopped: it starts again with the next use.
void Host::stopIdle()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (Running *ext : std::as_const(m_running)) {
        if (!ext->process || !ext->manifest.whenNeeded)
            continue;
        if (!ext->resultsShown.isEmpty() || !ext->formPending.isEmpty() || !ext->places.isEmpty() || !ext->listings.isEmpty())
            continue; // it has something on screen
        if (ext->claudePending > 0 || !ext->planPending.isEmpty())
            continue; // it waits for an answer from Claude, or for the user to look at its plan
        if (now - ext->lastActive < m_idleMs)
            continue;
        stop(ext);
        forget(ext);
    }
}

namespace {

// "sftp" of "sftp://host/path"; empty for anything that isn't an address.
QString schemeOf(const QString &address)
{
    const qsizetype at = address.indexOf(QLatin1String("://"));
    return at > 0 ? address.left(at).toLower() : QString();
}

} // namespace

bool Host::handlesAddress(const QString &address) const
{
    const QString scheme = schemeOf(address);
    if (scheme.isEmpty())
        return false;
    for (const Running *ext : m_running) {
        if (ext->manifest.addresses.contains(scheme) && ext->failures < MaxFailures)
            return true;
    }
    return false;
}

bool Host::takePlan(const QString &extension, const QString &id)
{
    for (Running *ext : std::as_const(m_running)) {
        if (ext->manifest.id == extension && ext->process && ext->planPending == id && !ext->planTaken) {
            ext->planTaken = true;
            return true;
        }
    }
    return false;
}

void Host::answerPlan(const QString &extension, const QString &id, bool dismissed, int applied, int skipped)
{
    for (Running *ext : std::as_const(m_running)) {
        if (ext->manifest.id != extension || ext->planPending != id)
            continue;
        ext->planPending.clear();
        ext->planTaken = false;
        ext->lastActive = QDateTime::currentMSecsSinceEpoch();
        QJsonObject message{{QStringLiteral("type"), QStringLiteral("plan")}, {QStringLiteral("id"), id}};
        if (dismissed) {
            message.insert(QStringLiteral("dismissed"), true);
        } else {
            message.insert(QStringLiteral("applied"), applied);
            message.insert(QStringLiteral("skipped"), skipped);
        }
        send(ext, message);
    }
}

// The rules are the ones Organize always had (Organizer::validate): an item
// of this folder, into a folder inside it, never onto something that is
// there, never two to one place, never a folder into itself.
void Host::checkPlan(const QString &folder, QList<PlanMove> *moves)
{
    QList<Organizer::Item> items;
    const QStringList names = QDir(folder).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    for (const QString &name : names) {
        Organizer::Item item;
        item.name = name;
        items.append(item);
    }
    QList<Organizer::Move> checked;
    for (const PlanMove &move : std::as_const(*moves)) {
        Organizer::Move m;
        m.item = move.item;
        m.folder = move.folder;
        m.newName = move.newName;
        m.reason = move.reason;
        checked.append(m);
    }
    Organizer::validate(folder, items, &checked);
    for (int i = 0; i < moves->size(); ++i) {
        // A name is a name: nothing with a path in it reaches the file engine.
        const bool plain = !checked.at(i).item.contains(QLatin1Char('/')) && checked.at(i).item != QLatin1String("..");
        (*moves)[i].folder = checked.at(i).folder;
        (*moves)[i].newName = checked.at(i).newName;
        (*moves)[i].problem = plain ? checked.at(i).problem : QObject::tr("“%1” isn't in this folder").arg(checked.at(i).item);
    }
}

// An extension may open a folder, ask the user something, propose changes or
// have Claude asked only as part of something the user chose from it (a
// command, a sidebar row, an address, an answered form), and not long after:
// nothing of this kind comes out of the blue.
bool Host::isActingForUser(const Running *ext)
{
    return ext->userActed != 0 && QDateTime::currentMSecsSinceEpoch() - ext->userActed <= OpenWithinMs;
}

void Host::stopWaitingForClaude(const QString &extension)
{
    for (Running *ext : std::as_const(m_running)) {
        if (ext->manifest.id != extension || ext->claudeWaiting.isEmpty())
            continue;
        const QStringList ids = ext->claudeWaiting;
        ext->claudeWaiting.clear();
        for (const QString &id : ids) {
            send(ext, {{QStringLiteral("type"), QStringLiteral("claude")},
                       {QStringLiteral("id"), id},
                       {QStringLiteral("error"), QJsonObject{{QStringLiteral("message"), QStringLiteral("Stopped: you chose not to wait for Claude")}}}});
        }
        emit claudeWaiting(extension, ext->manifest.name, 0);
    }
}

double Host::claudeSpent(const QString &extension)
{
    return QSettings().value(QStringLiteral("extensions/%1/claudeSpent").arg(extension)).toDouble();
}

// What an extension asks Claude is its own; which model answers, how much
// it may write and how the request is carried are the user's and Argonaut's.
static void keepToWhatIsTheExtensions(QJsonObject *request)
{
    for (const char *key : {"model", "stream", "fallbacks", "service_tier", "metadata", "container", "mcp_servers"})
        request->remove(QLatin1String(key));
    if (request->contains(QLatin1String("max_tokens")))
        request->insert(QStringLiteral("max_tokens"), qBound(1, request->value(QLatin1String("max_tokens")).toInt(), MaxClaudeTokens));
}

// An extension never has the user's API key. It sends the request it wants
// made ({"type":"claude","id":…,"request":{…}}, the body of a Messages
// request) and Argonaut makes it: with the key, the model and the retries
// that are Argonaut's (Preferences → Claude), and it counts what it cost.
//   "ask": "info"   whether there is a key, and the model and mode chosen (free)
//   "ask": "count"  how many tokens the request is (free), and what they would cost
//   else            the request is sent; the answer comes back whole
// Only an extension that said "uses": ["claude"], and (but for "info") only
// while it is doing what the user asked of it, a few questions an action:
// an extension in the background can't spend the user's money.
void Host::askClaude(Running *ext, const QJsonObject &message)
{
    const QString id = message.value(QLatin1String("id")).toString().left(64);
    const QString extension = ext->manifest.id;
    auto refuse = [&](const QString &why) {
        send(ext, {{QStringLiteral("type"), QStringLiteral("claude")},
                   {QStringLiteral("id"), id},
                   {QStringLiteral("error"), QJsonObject{{QStringLiteral("message"), why}}}});
    };
    if (!ext->manifest.usesClaude) {
        refuse(QStringLiteral("This extension hasn't said that it uses Claude (\"uses\": [\"claude\"] in its extension.json)"));
        return;
    }
    if (!m_claude)
        m_claude = new Claude::Client(this);
    const QString ask = message.value(QLatin1String("ask")).toString();
    if (ask == QLatin1String("info")) {
        const Claude::ModelInfo model = Claude::modelInfo(Claude::model());
        const Claude::Mode mode = Claude::mode();
        send(ext, {{QStringLiteral("type"), QStringLiteral("claude")},
                   {QStringLiteral("id"), id},
                   {QStringLiteral("hasKey"), !m_claude->apiKey().isEmpty()},
                   {QStringLiteral("model"), model.id},
                   {QStringLiteral("modelName"), model.name},
                   {QStringLiteral("mode"), mode == Claude::Mode::NamesOnly ? QStringLiteral("names") : mode == Claude::Mode::Auto ? QStringLiteral("auto") : QStringLiteral("ask")}});
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!isActingForUser(ext)) {
        refuse(QStringLiteral("Claude is asked only while the extension is doing what the user chose"));
        return;
    }
    if (ext->claudeFor != ext->userActed) { // another action of the user's: its own count
        ext->claudeFor = ext->userActed;
        ext->claudeAsked = 0;
    }
    if (m_claude->apiKey().isEmpty()) {
        refuse(QStringLiteral("There is no Claude API key yet: Preferences → Claude"));
        return;
    }
    // The request is the extension's, the model and the limits are the user's.
    QJsonObject request = message.value(QLatin1String("request")).toObject();
    if (!request.value(QLatin1String("messages")).isArray()) {
        refuse(QStringLiteral("No request (\"request\": {\"messages\": […]})"));
        return;
    }
    keepToWhatIsTheExtensions(&request);
    auto running = [this, extension]() -> Running * { // it may be gone when the answer comes
        for (Running *each : std::as_const(m_running)) {
            if (each->manifest.id == extension && each->process)
                return each;
        }
        return nullptr;
    };
    if (ask == QLatin1String("count")) {
        request.remove(QLatin1String("max_tokens"));
        request.insert(QStringLiteral("model"), Claude::model());
        ++ext->claudePending;
        m_claude->countTokens(request, [this, running, id](qint64 tokens, const Claude::Error &error) {
            Running *asker = running();
            if (!asker)
                return;
            asker->claudePending = qMax(0, asker->claudePending - 1);
            QJsonObject answer{{QStringLiteral("type"), QStringLiteral("claude")}, {QStringLiteral("id"), id}};
            if (!error.message.isEmpty()) {
                answer.insert(QStringLiteral("error"), QJsonObject{{QStringLiteral("message"), error.message}, {QStringLiteral("status"), error.status}});
            } else {
                answer.insert(QStringLiteral("tokens"), double(tokens));
                answer.insert(QStringLiteral("dollars"), Claude::cost(Claude::model(), tokens, 0));
            }
            send(asker, answer);
        });
        return;
    }
    if (ext->claudeAsked >= MaxClaudePerAction) {
        refuse(QStringLiteral("That is many questions for one thing the user chose: Argonaut asks no more until they choose again"));
        return;
    }
    ++ext->claudeAsked;
    ++ext->claudePending;
    ext->lastActive = now;
    ext->claudeWaiting << id;
    emit claudeWaiting(extension, ext->manifest.name, int(ext->claudeWaiting.size()));
    m_claude->send(request, [this, running, id, extension](const Claude::Response &response, const Claude::Error &error) {
        if (response.dollars > 0) { // paid, whether or not the extension is still there to hear the answer
            QSettings settings;
            const QString key = QStringLiteral("extensions/%1/claudeSpent").arg(extension);
            settings.setValue(key, settings.value(key).toDouble() + response.dollars);
        }
        Running *asker = running();
        if (!asker)
            return;
        asker->claudePending = qMax(0, asker->claudePending - 1);
        asker->lastActive = QDateTime::currentMSecsSinceEpoch();
        if (!asker->claudeWaiting.removeOne(id))
            return; // the user stopped waiting for this one: it was answered then
        emit claudeWaiting(extension, asker->manifest.name, int(asker->claudeWaiting.size()));
        QJsonObject answer{{QStringLiteral("type"), QStringLiteral("claude")}, {QStringLiteral("id"), id}};
        if (!error.message.isEmpty()) {
            answer.insert(QStringLiteral("error"), QJsonObject{{QStringLiteral("message"), error.message}, {QStringLiteral("status"), error.status}});
        } else {
            answer.insert(QStringLiteral("response"), response.message);
            answer.insert(QStringLiteral("text"), response.text);
            answer.insert(QStringLiteral("stop_reason"), response.stopReason);
            answer.insert(QStringLiteral("dollars"), response.dollars);
        }
        send(asker, answer);
    });
}

bool Host::openAddress(const QString &address)
{
    const QString scheme = schemeOf(address);
    if (scheme.isEmpty() || m_quitting)
        return false;
    for (Running *ext : std::as_const(m_running)) {
        if (!ext->manifest.addresses.contains(scheme) || ext->failures >= MaxFailures)
            continue;
        if (!ext->process)
            start(ext); // an address can come before any folder is shown (the command line)
        ext->userActed = QDateTime::currentMSecsSinceEpoch();
        send(ext, {{QStringLiteral("type"), QStringLiteral("address")}, {QStringLiteral("address"), address.left(2000)}});
        return true;
    }
    return false;
}

bool Host::takeForm(const QString &extension, const QString &id)
{
    for (Running *ext : std::as_const(m_running)) {
        if (ext->manifest.id == extension && ext->process && ext->formPending == id && !ext->formTaken) {
            ext->formTaken = true;
            return true;
        }
    }
    return false;
}

void Host::answerForm(const QString &extension, const QString &id, bool cancelled, const QHash<QString, QString> &values, int button)
{
    for (Running *ext : std::as_const(m_running)) {
        if (ext->manifest.id != extension || ext->formPending != id)
            continue;
        ext->formPending.clear();
        ext->formTaken = false;
        QJsonObject message{{QStringLiteral("type"), QStringLiteral("form")}, {QStringLiteral("id"), id}};
        if (cancelled) {
            message.insert(QStringLiteral("cancelled"), true);
        } else {
            QJsonObject filled;
            for (auto it = values.constBegin(); it != values.constEnd(); ++it)
                filled.insert(it.key(), it.value());
            message.insert(QStringLiteral("values"), filled);
            message.insert(QStringLiteral("button"), button);
            ext->userActed = QDateTime::currentMSecsSinceEpoch(); // the conversation goes on
        }
        send(ext, message);
    }
}

QList<Place> Host::places() const
{
    QList<Place> out;
    for (const Running *ext : m_running) {
        if (ext->process)
            out.append(ext->places);
    }
    return out;
}

QString Host::placePath(const QString &extension, const QString &id)
{
    return QStringLiteral("extension-place:///") + extension + QLatin1Char('/') + QString::fromLatin1(QUrl::toPercentEncoding(id));
}

bool Host::isPlacePath(const QString &path)
{
    return path.startsWith(QLatin1String("extension-place:///"));
}

void Host::activatePlace(const QString &path, const QString &action)
{
    if (!isPlacePath(path) || (action != QLatin1String("open") && action != QLatin1String("eject")))
        return;
    for (Running *ext : std::as_const(m_running)) {
        if (!ext->process)
            continue;
        for (const Place &place : std::as_const(ext->places)) {
            if (placePath(place.extension, place.id) != path)
                continue;
            ext->userActed = QDateTime::currentMSecsSinceEpoch();
            send(ext, {{QStringLiteral("type"), QStringLiteral("place")},
                       {QStringLiteral("id"), place.id},
                       {QStringLiteral("action"), action}});
            return;
        }
    }
}

void Host::invoke(const Command &command, const QString &folder, const QStringList &selection, const QString &input)
{
    for (Running *ext : std::as_const(m_running)) {
        if (ext->manifest.id != command.extension)
            continue;
        wake(ext); // a when-needed one starts now
        if (!ext->process)
            continue;
        ext->userActed = QDateTime::currentMSecsSinceEpoch();
        send(ext, {{QStringLiteral("type"), QStringLiteral("invoke")},
                   {QStringLiteral("command"), command.id},
                   {QStringLiteral("folder"), folder},
                   {QStringLiteral("selection"), QJsonArray::fromStringList(selection)},
                   {QStringLiteral("input"), input}});
    }
}

bool Host::canPreview(const QString &mimeType) const
{
    for (const Running *ext : m_running) {
        if (isAvailable(ext) && typeMatches(mimeType, previewTypesOf(ext)) && !previewTypesOf(ext).isEmpty())
            return true;
    }
    return false;
}

int Host::requestPreview(const QString &path, const QString &mimeType, const QSize &pixels)
{
    for (Running *ext : std::as_const(m_running)) {
        if (!isAvailable(ext) || previewTypesOf(ext).isEmpty() || !typeMatches(mimeType, previewTypesOf(ext)))
            continue;
        wake(ext);
        if (!ext->process)
            continue;
        const int id = ++m_previewRequest;
        send(ext, {{QStringLiteral("type"), QStringLiteral("preview")},
                   {QStringLiteral("id"), id},
                   {QStringLiteral("path"), path},
                   {QStringLiteral("mime"), mimeType},
                   {QStringLiteral("width"), pixels.width()},
                   {QStringLiteral("height"), pixels.height()}});
        return id;
    }
    return 0;
}

QString Host::listingPath(const QString &extension, const QString &id)
{
    return QStringLiteral("listing:///") + extension + QLatin1Char('/') + QString::fromLatin1(QUrl::toPercentEncoding(id));
}

bool Host::isListingPath(const QString &path)
{
    return path.startsWith(QLatin1String("listing:///"));
}

Listing Host::listing(const QString &path) const
{
    if (!isListingPath(path))
        return {};
    const QString rest = path.mid(11);
    const qsizetype slash = rest.indexOf(QLatin1Char('/'));
    if (slash <= 0)
        return {};
    const QString extension = rest.left(slash);
    const QString id = QUrl::fromPercentEncoding(rest.mid(slash + 1).toLatin1());
    for (const Running *ext : m_running) {
        if (ext->manifest.id == extension)
            return ext->listings.value(id);
    }
    return {};
}

bool Host::listingPreviews(const QString &path) const
{
    return listing(path).previews;
}

int Host::requestListingPreview(const QString &path, const QString &file, const QString &mimeType, const QSize &pixels)
{
    const Listing list = listing(path);
    if (!list.valid || !list.previews || !list.files.contains(file))
        return 0;
    for (Running *ext : std::as_const(m_running)) {
        if (ext->manifest.id != list.extension || !ext->process)
            continue;
        const int id = ++m_previewRequest;
        send(ext, {{QStringLiteral("type"), QStringLiteral("preview")},
                   {QStringLiteral("id"), id},
                   {QStringLiteral("path"), file},
                   {QStringLiteral("mime"), mimeType},
                   {QStringLiteral("listing"), list.id},
                   {QStringLiteral("width"), pixels.width()},
                   {QStringLiteral("height"), pixels.height()}});
        return id;
    }
    return 0;
}

QString Host::nameOf(const QString &id) const
{
    for (const Manifest &m : m_manifests) {
        if (m.id == id)
            return m.name;
    }
    return id;
}

Host::State Host::state(const QString &id) const
{
    for (const Manifest &m : m_manifests) {
        if (m.id == id && !m.isUsable())
            return State::Unusable;
    }
    for (const Running *ext : m_running) {
        if (ext->manifest.id != id)
            continue;
        if (ext->process && ext->process->state() == QProcess::Running)
            return State::Running;
        return ext->failures >= MaxFailures ? State::Stopped : State::Waiting;
    }
    return State::Off;
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
        // Not the ones that asked to be started only when something of theirs is used.
        if (!ext->process && ext->failures < MaxFailures && !ext->manifest.whenNeeded)
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
               {QStringLiteral("version"), QCoreApplication::applicationVersion()},
               {QStringLiteral("interface"), m_interface}});
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

void Host::resultShown(const QString &extension, const QString &id, bool shown, const QSize &pixels, const ResultLook &look)
{
    for (Running *ext : std::as_const(m_running)) {
        if (ext->manifest.id != extension)
            continue;
        if (shown)
            ext->resultsShown.insert(id);
        else
            ext->resultsShown.remove(id);
        ext->lastActive = QDateTime::currentMSecsSinceEpoch();
        QJsonObject message{{QStringLiteral("type"), QStringLiteral("result")},
                            {QStringLiteral("id"), id},
                            {QStringLiteral("shown"), shown}};
        if (shown) {
            message.insert(QStringLiteral("width"), pixels.width());
            message.insert(QStringLiteral("height"), pixels.height());
            message.insert(QStringLiteral("dark"), look.dark);
            QJsonObject colors;
            const QPair<QString, QString> named[] = {{QStringLiteral("window"), look.window},   {QStringLiteral("surface"), look.surface},
                                                     {QStringLiteral("text"), look.text},       {QStringLiteral("dim"), look.dim},
                                                     {QStringLiteral("accent"), look.accent},   {QStringLiteral("border"), look.border}};
            for (const auto &color : named) {
                if (!color.second.isEmpty())
                    colors.insert(color.first, color.second);
            }
            if (!colors.isEmpty())
                message.insert(QStringLiteral("colors"), colors);
            if (!look.font.isEmpty())
                message.insert(QStringLiteral("font"), look.font);
            if (look.columns > 0 && look.rows > 0) {
                message.insert(QStringLiteral("columns"), look.columns);
                message.insert(QStringLiteral("rows"), look.rows);
            }
        }
        send(ext, message);
    }
}

void Host::searchResult(const QString &extension, const QString &id, const QString &text)
{
    for (Running *ext : std::as_const(m_running)) {
        if (ext->manifest.id != extension || !ext->resultsShown.contains(id))
            continue; // only for what is on screen
        ext->lastActive = QDateTime::currentMSecsSinceEpoch();
        send(ext, {{QStringLiteral("type"), QStringLiteral("search")}, {QStringLiteral("id"), id}, {QStringLiteral("text"), text.left(200)}});
    }
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
    if (type != QLatin1String("folder") && type != QLatin1String("commands") && type != QLatin1String("previews"))
        ext->lastActive = QDateTime::currentMSecsSinceEpoch(); // it is doing something for the user
    if (type == QLatin1String("commands")) { // what it offers everywhere
        ext->commands = readCommands(ext, message.value(QLatin1String("commands")), MaxCommands);
        ext->commandsSaid = true;
        return;
    }
    if (type == QLatin1String("previews")) { // the types it shows in the preview pane
        ext->previewsSaid = true;
        ext->previewTypes.clear();
        const QJsonArray types = message.value(QLatin1String("types")).toArray();
        for (const QJsonValue &t : types) {
            if (ext->previewTypes.size() < 100 && !t.toString().isEmpty())
                ext->previewTypes << t.toString().left(100);
        }
        return;
    }
    if (type == QLatin1String("preview")) { // its answer to a preview request
        const PreviewReply reply = readContent(message);
        emit previewReady(message.value(QLatin1String("id")).toInt(), reply);
        return;
    }
    if (type == QLatin1String("places")) { // its rows in the sidebar; replaces what it said before
        hearPlaces(ext, message);
        return;
    }
    if (type == QLatin1String("form")) { // it needs the user to fill something in, or to decide
        hearForm(ext, message);
        return;
    }
    if (type == QLatin1String("open")) { // show this folder: the answer to what the user chose
        const QString folder = message.value(QLatin1String("folder")).toString();
        // Never out of the blue: an extension can't move the user about.
        if (!isActingForUser(ext))
            return;
        if (!QDir::isAbsolutePath(folder) || !QFileInfo(folder).isDir())
            return;
        ext->userActed = 0; // one answer to one choice
        emit openRequested(ext->manifest.name, QDir::cleanPath(folder));
        return;
    }
    if (type == QLatin1String("listing")) { // a list of files it keeps
        hearListing(ext, message);
        return;
    }
    if (type == QLatin1String("show")) { // a result for the preview pane
        hearShow(ext, message);
        return;
    }
    if (type == QLatin1String("plan")) { // changes it proposes for a folder: Argonaut shows, checks and does them
        hearPlan(ext, message);
        return;
    }
    if (type == QLatin1String("claude")) { // a question for Claude: Argonaut asks it, with the user's key
        askClaude(ext, message);
        return;
    }
    if (type == QLatin1String("message")) { // something for the user to read
        const QString text = tidy(message.value(QLatin1String("text")).toString(), MaxMessageChars)
                                 .replace(QLatin1Char('\n'), QLatin1Char(' '));
        if (!text.isEmpty())
            emit this->message(ext->manifest.name, text);
        return;
    }
    if (type == QLatin1String("folder")) // its marks, its text and its commands for a folder that is shown
        hearFolder(ext, message);
}

// "places": its rows in the sidebar; replaces what it said before
void Host::hearPlaces(Running *ext, const QJsonObject &message)
{
    QList<Place> places;
    QSet<QString> ids;
    const QJsonArray list = message.value(QLatin1String("places")).toArray();
    for (const QJsonValue &entry : list) {
        if (places.size() >= MaxPlaces)
            break;
        const QJsonObject o = entry.toObject();
        Place place;
        place.extension = ext->manifest.id;
        place.id = o.value(QLatin1String("id")).toString().left(200);
        place.label = tidy(o.value(QLatin1String("label")).toString(), MaxLabelChars).replace(QLatin1Char('\n'), QLatin1Char(' '));
        if (place.id.isEmpty() || place.label.isEmpty() || ids.contains(place.id))
            continue;
        ids.insert(place.id);
        place.tooltip = tidy(o.value(QLatin1String("tooltip")).toString(), MaxTooltipChars);
        static const QStringList icons{QStringLiteral("network"), QStringLiteral("cloud"), QStringLiteral("drive"),
                                       QStringLiteral("removable"), QStringLiteral("folder")};
        place.icon = o.value(QLatin1String("icon")).toString();
        if (!icons.contains(place.icon))
            place.icon = QStringLiteral("folder");
        // A folder to open directly must be a real one, by its full path.
        const QString folder = o.value(QLatin1String("folder")).toString();
        if (!folder.isEmpty() && QDir::isAbsolutePath(folder) && QFileInfo(folder).isDir())
            place.folder = QDir::cleanPath(folder);
        place.eject = o.value(QLatin1String("eject")).toBool();
        places.append(place);
    }
    ext->places = places;
    emit placesChanged();
}

// "form": it needs the user to fill something in, or to decide
void Host::hearForm(Running *ext, const QJsonObject &message)
{
    const QString id = message.value(QLatin1String("id")).toString().left(64);
    if (id.isEmpty())
        return;
    auto refuse = [&] { send(ext, {{QStringLiteral("type"), QStringLiteral("form")}, {QStringLiteral("id"), id}, {QStringLiteral("cancelled"), true}}); };
    // Like "open": only while it is doing what the user asked of it. A
    // dialog out of the blue asking for a password is not something an
    // extension gets to show. And one at a time.
    if (!isActingForUser(ext) || !ext->formPending.isEmpty()) {
        refuse();
        return;
    }
    Form form;
    form.extension = ext->manifest.id;
    form.extensionName = ext->manifest.name;
    form.id = id;
    form.title = tidy(message.value(QLatin1String("title")).toString(), MaxLabelChars).replace(QLatin1Char('\n'), QLatin1Char(' '));
    form.heading = tidy(message.value(QLatin1String("heading")).toString(), 100).replace(QLatin1Char('\n'), QLatin1Char(' '));
    form.text = tidy(message.value(QLatin1String("text")).toString(), 1000);
    form.warning = tidy(message.value(QLatin1String("warning")).toString(), MaxTooltipChars);
    QSet<QString> ids;
    const QJsonArray fields = message.value(QLatin1String("fields")).toArray();
    for (const QJsonValue &entry : fields) {
        if (form.fields.size() >= 12)
            break;
        const QJsonObject o = entry.toObject();
        FormField field;
        field.id = o.value(QLatin1String("id")).toString().left(64);
        if (field.id.isEmpty() || ids.contains(field.id))
            continue;
        ids.insert(field.id);
        field.label = tidy(o.value(QLatin1String("label")).toString(), MaxLabelChars).replace(QLatin1Char('\n'), QLatin1Char(' '));
        const QString kind = o.value(QLatin1String("kind")).toString();
        field.kind = kind == QLatin1String("secret") ? FormField::Secret
                     : kind == QLatin1String("check") ? FormField::Check
                     : kind == QLatin1String("choice") ? FormField::Choice : FormField::Text;
        const QJsonValue value = o.value(QLatin1String("value"));
        field.value = value.isBool() ? (value.toBool() ? QStringLiteral("true") : QStringLiteral("false"))
                                     : value.toString().left(2000).replace(QLatin1Char('\n'), QLatin1Char(' '));
        field.placeholder = tidy(o.value(QLatin1String("placeholder")).toString(), 100).replace(QLatin1Char('\n'), QLatin1Char(' '));
        const QJsonArray choices = o.value(QLatin1String("choices")).toArray();
        for (const QJsonValue &choice : choices) {
            const QString text = choice.toString().left(300).replace(QLatin1Char('\n'), QLatin1Char(' '));
            if (field.choices.size() < 200 && !text.isEmpty())
                field.choices << text;
        }
        if (field.kind == FormField::Choice && field.choices.isEmpty())
            continue; // nothing to choose from
        field.alone = o.value(QLatin1String("alone")).toBool();
        form.fields.append(field);
    }
    const QJsonArray buttons = message.value(QLatin1String("buttons")).toArray();
    for (const QJsonValue &button : buttons) {
        const QString text = tidy(button.toString(), 40).replace(QLatin1Char('\n'), QLatin1Char(' '));
        if (form.buttons.size() < 6 && !text.isEmpty())
            form.buttons << text;
    }
    ext->formPending = id;
    ext->formTaken = false;
    emit formRequested(form);
}

// "listing": a list of files it keeps
void Host::hearListing(Running *ext, const QJsonObject &message)
{
    const QString id = message.value(QLatin1String("id")).toString().left(1000);
    if (id.isEmpty())
        return;
    if (message.value(QLatin1String("remove")).toBool()) {
        if (ext->listings.remove(id) > 0)
            emit listingChanged(listingPath(ext->manifest.id, id));
        return;
    }
    if (!ext->listings.contains(id) && ext->listings.size() >= MaxListings)
        return;
    Listing list;
    list.valid = true;
    list.extension = ext->manifest.id;
    list.id = id;
    list.title = tidy(message.value(QLatin1String("title")).toString(), MaxLabelChars).replace(QLatin1Char('\n'), QLatin1Char(' '));
    if (list.title.isEmpty())
        list.title = ext->manifest.name;
    const QString folder = message.value(QLatin1String("folder")).toString();
    if (QDir::isAbsolutePath(folder))
        list.folder = QDir::cleanPath(folder);
    list.previews = message.value(QLatin1String("previews")).toBool();
    const QJsonArray files = message.value(QLatin1String("files")).toArray();
    for (const QJsonValue &file : files) {
        if (list.files.size() >= MaxListingFiles)
            break;
        const QString path = file.toString();
        if (QDir::isAbsolutePath(path)) // a full path, as it is: no "..", no tricks
            list.files << QDir::cleanPath(path);
    }
    ext->listings.insert(id, list);
    emit listingChanged(listingPath(ext->manifest.id, id));
}

// "show": a result for the preview pane
void Host::hearShow(Running *ext, const QJsonObject &message)
{
    if (message.value(QLatin1String("kind")).toString() == QLatin1String("listing")) {
        const QString id = message.value(QLatin1String("listing")).toString();
        if (ext->listings.contains(id))
            emit showListingRequested(listingPath(ext->manifest.id, id));
        return;
    }
    PreviewReply content = readContent(message);
    content.extension = ext->manifest.id;
    content.resultId = message.value(QLatin1String("id")).toString().left(64);
    content.update = !content.resultId.isEmpty() && message.value(QLatin1String("update")).toBool();
    if (!content.resultId.isEmpty() && message.contains(QLatin1String("search"))) {
        content.searchable = true;
        content.searchHint = tidy(message.value(QLatin1String("search")).toString(), MaxLabelChars).replace(QLatin1Char('\n'), QLatin1Char(' '));
    }
    if (content.kind != PreviewReply::None)
        emit showRequested(ext->manifest.name,
                           tidy(message.value(QLatin1String("title")).toString(), MaxLabelChars)
                               .replace(QLatin1Char('\n'), QLatin1Char(' ')),
                           content);
}

// "plan": changes it proposes for a folder: Argonaut shows, checks and does them
void Host::hearPlan(Running *ext, const QJsonObject &message)
{
    const QString id = message.value(QLatin1String("id")).toString().left(64);
    if (id.isEmpty())
        return;
    auto refuse = [&] { send(ext, {{QStringLiteral("type"), QStringLiteral("plan")}, {QStringLiteral("id"), id}, {QStringLiteral("dismissed"), true}}); };
    // Like a form: only while it is doing what the user asked of it, and one at a time.
    Plan plan;
    plan.folder = QDir::cleanPath(message.value(QLatin1String("folder")).toString());
    if (!isActingForUser(ext) || !ext->planPending.isEmpty()
        || !QDir::isAbsolutePath(plan.folder) || !QFileInfo(plan.folder).isDir()) {
        refuse();
        return;
    }
    plan.extension = ext->manifest.id;
    plan.extensionName = ext->manifest.name;
    plan.id = id;
    plan.title = tidy(message.value(QLatin1String("title")).toString(), MaxLabelChars).replace(QLatin1Char('\n'), QLatin1Char(' '));
    plan.report = message.value(QLatin1String("report")).toString().left(20000);
    plan.note = tidy(message.value(QLatin1String("note")).toString(), MaxMessageChars).replace(QLatin1Char('\n'), QLatin1Char(' '));
    const QJsonArray moves = message.value(QLatin1String("moves")).toArray();
    for (const QJsonValue &value : moves) {
        if (plan.moves.size() >= MaxPlanMoves)
            break;
        const QJsonObject o = value.toObject();
        PlanMove move;
        move.item = o.value(QLatin1String("item")).toString().left(255);
        move.folder = o.value(QLatin1String("folder")).toString().left(1000);
        move.newName = o.value(QLatin1String("new_name")).toString().left(255);
        move.reason = tidy(o.value(QLatin1String("reason")).toString(), MaxMessageChars).replace(QLatin1Char('\n'), QLatin1Char(' '));
        if (!move.item.isEmpty())
            plan.moves.append(move);
    }
    checkPlan(plan.folder, &plan.moves);
    ext->planPending = id;
    ext->planTaken = false;
    emit planRequested(plan);
}

// "folder": what it has to say about a folder that is shown; replaces what it said before.
void Host::hearFolder(Running *ext, const QJsonObject &message)
{
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
    data.commands = readCommands(ext, message.value(QLatin1String("commands")), MaxFolderCommands);
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
    ext->previewTypes.clear();
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
    if (!ext->places.isEmpty()) {
        ext->places.clear();
        emit placesChanged();
    }
    ext->userActed = 0;
    ext->formPending.clear();
    ext->formTaken = false;
    ext->planPending.clear();
    ext->planTaken = false;
    ext->claudePending = 0;
    if (!ext->claudeWaiting.isEmpty()) {
        ext->claudeWaiting.clear();
        emit claudeWaiting(ext->manifest.id, ext->manifest.name, 0); // it is gone: nobody waits
    }
    ext->commands.clear();
    ext->commandsSaid = false;
    ext->previewTypes.clear();
    ext->previewsSaid = false;
    ext->resultsShown.clear();
    const QStringList lists = ext->listings.keys();
    ext->listings.clear();
    for (const QString &id : lists)
        emit listingChanged(listingPath(ext->manifest.id, id));
    const QStringList folders = ext->folders.keys();
    ext->folders.clear();
    for (const QString &folder : folders)
        emit folderChanged(folder);
}

} // namespace Extensions
