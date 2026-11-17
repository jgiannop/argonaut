#include "FileClipboard.h"

#include "Tools.h"

#include <QDir>
#include <QProcess>
#include <QUrl>

namespace FileClipboard {

const char *const GnomeFormat = "x-special/gnome-copied-files";

namespace {

constexpr int TimeoutMs = 1500;   // a helper that doesn't answer isn't waited for
constexpr int MaxBytes = 4 << 20; // nor is a clipboard of any size read

void addLocal(const QByteArray &line, QStringList *paths)
{
    const QUrl url = QUrl::fromEncoded(line.trimmed());
    if (!url.isLocalFile())
        return;
    const QString path = QDir::cleanPath(url.toLocalFile());
    if (QDir::isAbsolutePath(path) && !paths->contains(path))
        paths->append(path);
}

enum class Helper { None, Xclip, Wayland };

struct Chosen
{
    Helper helper = Helper::None;
    QString copy, paste; // the programs
};

Chosen chosen()
{
    const QByteArray wanted = qgetenv("ARGONAUT_CLIPBOARD");
    if (wanted == "0" || (wanted.isEmpty() && qEnvironmentVariableIsSet("ARGONAUT_TRASH_HOME_ONLY")))
        return {};
    if (qEnvironmentVariableIsSet("ARGONAUT_CLIPBOARD_PROGRAM")) { // a test's own, spoken to as xclip
        const QString program = Tools::program("ARGONAUT_CLIPBOARD_PROGRAM", QStringLiteral("xclip"));
        return {Helper::Xclip, program, program};
    }
    if (qEnvironmentVariableIsSet("WAYLAND_DISPLAY")) {
        const QString copy = Tools::find("ARGONAUT_CLIPBOARD_PROGRAM", QStringLiteral("wl-copy"));
        const QString paste = Tools::find("ARGONAUT_CLIPBOARD_PROGRAM", QStringLiteral("wl-paste"));
        if (!copy.isEmpty() && !paste.isEmpty())
            return {Helper::Wayland, copy, paste};
    }
    if (qEnvironmentVariableIsSet("DISPLAY")) {
        const QString xclip = Tools::find("ARGONAUT_CLIPBOARD_PROGRAM", QStringLiteral("xclip"));
        if (!xclip.isEmpty())
            return {Helper::Xclip, xclip, xclip};
    }
    return {};
}

// Runs a helper to the end. `input` goes to its standard input; its output
// is returned through `output` when that is asked for. A helper that keeps
// the clipboard stays behind as a process of its own: what it would write
// goes nowhere, so nothing waits on it.
bool run(const QString &program, const QStringList &arguments, const QByteArray &input = {}, QByteArray *output = nullptr)
{
    QProcess process;
    if (!output)
        process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
    process.start(program, arguments);
    if (!process.waitForStarted(TimeoutMs))
        return false;
    process.write(input);
    process.closeWriteChannel();
    if (!process.waitForFinished(TimeoutMs)) {
        process.kill();
        process.waitForFinished(500);
        return false;
    }
    if (output)
        *output = process.readAllStandardOutput().left(MaxBytes);
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

QStringList copyArguments(Helper helper, const QString &type)
{
    return helper == Helper::Wayland ? QStringList{QStringLiteral("--type"), type}
                                     : QStringList{QStringLiteral("-selection"), QStringLiteral("clipboard"), QStringLiteral("-t"), type, QStringLiteral("-i")};
}

bool read(const Chosen &with, const QString &type, QByteArray *data)
{
    return with.helper == Helper::Wayland
               ? run(with.paste, {QStringLiteral("--no-newline"), QStringLiteral("--type"), type}, {}, data)
               : run(with.paste, {QStringLiteral("-selection"), QStringLiteral("clipboard"), QStringLiteral("-t"), type, QStringLiteral("-o")}, {}, data);
}

} // namespace

QByteArray encode(const QStringList &paths, bool cut)
{
    QByteArray data = cut ? "cut" : "copy";
    for (const QString &path : paths)
        data += '\n' + QUrl::fromLocalFile(path).toEncoded();
    return data;
}

bool decode(const QByteArray &data, QStringList *paths, bool *cut)
{
    paths->clear();
    *cut = false;
    const QList<QByteArray> lines = data.split('\n');
    const QByteArray verb = lines.value(0).trimmed();
    if (verb != "cut" && verb != "copy")
        return false;
    *cut = verb == "cut";
    for (int i = 1; i < lines.size(); ++i)
        addLocal(lines.at(i), paths);
    return !paths->isEmpty();
}

bool decodeUriList(const QByteArray &data, QStringList *paths)
{
    paths->clear();
    for (const QByteArray &line : data.split('\n')) {
        if (!line.startsWith('#')) // a comment, by the format
            addLocal(line, paths);
    }
    return !paths->isEmpty();
}

bool desktopAvailable()
{
    return chosen().helper != Helper::None;
}

QString missingHelper()
{
    const QByteArray wanted = qgetenv("ARGONAUT_CLIPBOARD");
    if (wanted == "0" || (wanted.isEmpty() && qEnvironmentVariableIsSet("ARGONAUT_TRASH_HOME_ONLY")) || chosen().helper != Helper::None)
        return {};
    if (qEnvironmentVariableIsSet("WAYLAND_DISPLAY"))
        return QStringLiteral("wl-clipboard");
    return qEnvironmentVariableIsSet("DISPLAY") ? QStringLiteral("xclip") : QString();
}

bool copyToDesktop(const QStringList &paths, bool cut)
{
    const Chosen with = chosen();
    if (with.helper == Helper::None)
        return false;
    return run(with.copy, copyArguments(with.helper, QString::fromLatin1(GnomeFormat)), encode(paths, cut));
}

Read readDesktop(QStringList *paths, bool *cut)
{
    paths->clear();
    *cut = false;
    const Chosen with = chosen();
    if (with.helper == Helper::None)
        return Read::Unavailable;
    // What kinds the clipboard has. Nothing there at all is an error of the
    // helper's, and "empty" to us.
    QByteArray kinds;
    const bool listed = with.helper == Helper::Wayland ? run(with.paste, {QStringLiteral("--list-types")}, {}, &kinds)
                                                       : read(with, QStringLiteral("TARGETS"), &kinds);
    if (!listed)
        return Read::Empty;
    const QList<QByteArray> have = kinds.split('\n');
    auto has = [&have](const char *type) {
        for (const QByteArray &one : have) {
            if (one.trimmed() == type)
                return true;
        }
        return false;
    };
    QByteArray data;
    if (has(GnomeFormat) && read(with, QString::fromLatin1(GnomeFormat), &data) && decode(data, paths, cut))
        return Read::Files;
    if (has("text/uri-list") && read(with, QStringLiteral("text/uri-list"), &data) && decodeUriList(data, paths))
        return Read::Files;
    return Read::Empty;
}

void clearDesktop()
{
    const Chosen with = chosen();
    if (with.helper == Helper::Wayland)
        run(with.copy, {QStringLiteral("--clear")});
    else if (with.helper == Helper::Xclip)
        run(with.copy, copyArguments(with.helper, QStringLiteral("text/plain")), QByteArray());
}

} // namespace FileClipboard
