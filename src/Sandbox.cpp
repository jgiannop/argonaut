#include "Sandbox.h"

#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

namespace Sandbox {

bool available()
{
    static const bool found = !QStandardPaths::findExecutable(QStringLiteral("bwrap")).isEmpty();
    return found;
}

QByteArray run(const QString &program, const QStringList &args, const QString &file, bool *ok, int timeoutMs,
               qsizetype maxBytes)
{
    if (ok)
        *ok = false;
    const QString bwrap = QStandardPaths::findExecutable(QStringLiteral("bwrap"));
    const QString tool = QStandardPaths::findExecutable(program);
    if (bwrap.isEmpty() || tool.isEmpty() || !QFileInfo(file).isFile())
        return {};
    // Keep the real suffix: some tools look at it.
    const QString suffix = QFileInfo(file).suffix();
    const QString inside = QStringLiteral("/tmp/input") + (suffix.isEmpty() ? QString() : QLatin1Char('.') + suffix);
    QStringList box = {QStringLiteral("--ro-bind"), QStringLiteral("/usr"), QStringLiteral("/usr"),
                       QStringLiteral("--ro-bind-try"), QStringLiteral("/etc"), QStringLiteral("/etc"),
                       QStringLiteral("--proc"), QStringLiteral("/proc"), QStringLiteral("--dev"), QStringLiteral("/dev"),
                       QStringLiteral("--tmpfs"), QStringLiteral("/tmp"),
                       QStringLiteral("--unshare-all"), QStringLiteral("--die-with-parent"), QStringLiteral("--new-session"),
                       QStringLiteral("--ro-bind"), file, inside};
    // Merged-/usr systems: /bin, /lib… are links into /usr.
    for (const char *link : {"bin", "sbin", "lib", "lib32", "lib64", "libx32"}) {
        const QFileInfo info(QStringLiteral("/") + QLatin1String(link));
        if (info.isSymLink())
            box << QStringLiteral("--symlink") << info.symLinkTarget().mid(1) << info.filePath();
        else if (info.isDir())
            box << QStringLiteral("--ro-bind") << info.filePath() << info.filePath();
    }
    box << tool;
    for (const QString &a : args)
        box << (a == QLatin1String("@FILE@") ? inside : a);

    QProcess process;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C.UTF-8"));
    process.setProcessEnvironment(env);
    process.setStandardInputFile(QProcess::nullDevice());
    process.start(bwrap, box);
    if (!process.waitForStarted(5000))
        return {};
    if (!process.waitForFinished(timeoutMs)) {
        process.kill(); // --die-with-parent takes the rest down
        process.waitForFinished(1000);
        return {};
    }
    const QByteArray out = process.readAllStandardOutput().left(maxBytes);
    if (ok)
        *ok = process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    return out;
}

} // namespace Sandbox
