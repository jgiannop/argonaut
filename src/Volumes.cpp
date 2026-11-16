#include "Volumes.h"

#include "Tools.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QTimer>

namespace {

const QLatin1String kScheme("volume://");
constexpr int kStaleMs = 15000;      // asked again after this long, when someone looks
constexpr int kMountTimeoutMs = 180000; // the desktop may be asking for a password meanwhile

QProcessEnvironment plainEnvironment()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C")); // gio's words as they are parsed here
    return env;
}

// A device name that is one: nothing else is ever handed to gio.
bool isDeviceName(const QString &device)
{
    static const QRegularExpression plain(QStringLiteral("^/dev/[A-Za-z0-9/_.:+-]+$"));
    return plain.match(device).hasMatch() && !device.contains(QLatin1String(".."));
}

// "  uuid: 'abc'" → abc
QString quoted(const QString &line)
{
    const qsizetype from = line.indexOf(QLatin1Char('\''));
    const qsizetype to = line.lastIndexOf(QLatin1Char('\''));
    return from >= 0 && to > from ? line.mid(from + 1, to - from - 1) : QString();
}

} // namespace

Volumes *Volumes::instance()
{
    static Volumes *volumes = new Volumes;
    return volumes;
}

bool Volumes::isActive()
{
#if defined(Q_OS_LINUX)
    const QByteArray wanted = qgetenv("ARGONAUT_VOLUMES");
    if (wanted == "0" || (wanted.isEmpty() && qEnvironmentVariableIsSet("ARGONAUT_TRASH_HOME_ONLY")))
        return false;
    if (qEnvironmentVariableIsSet("ARGONAUT_GIO_PROGRAM"))
        return true; // a test's own
    static const bool there = !Tools::find("ARGONAUT_GIO_PROGRAM", QStringLiteral("gio")).isEmpty();
    return there;
#else
    return false;
#endif
}

Volumes::Volumes()
    : QObject(QCoreApplication::instance())
{
    m_soon = new QTimer(this);
    m_soon->setSingleShot(true);
    m_soon->setInterval(700); // a disk plugged in shows its partitions one after another
    connect(m_soon, &QTimer::timeout, this, &Volumes::refresh);
    if (!isActive())
        return;
    m_watcher = new QFileSystemWatcher(this);
    for (const QString &folder : {QStringLiteral("/dev/disk/by-uuid"), QStringLiteral("/dev/disk/by-label")}) {
        if (QFileInfo(folder).isDir())
            m_watcher->addPath(folder);
    }
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, m_soon, qOverload<>(&QTimer::start));
}

QString Volumes::pathFor(const QString &device)
{
    return kScheme + device; // "volume:///dev/sdb1"
}

bool Volumes::isVolumePath(const QString &path)
{
    return path.startsWith(kScheme);
}

QString Volumes::deviceOf(const QString &path)
{
    const QString device = isVolumePath(path) ? path.mid(kScheme.size()) : QString();
    return isDeviceName(device) ? device : QString();
}

QString Volumes::nameOf(const QString &device) const
{
    for (const Volume &volume : m_volumes) {
        if (volume.device == device)
            return volume.name;
    }
    return device;
}

QList<Volumes::Volume> Volumes::parse(const QByteArray &output)
{
    // An item is a line "Drive(0): name", "Volume(0): name" or
    // "Mount(0): name -> where", then its properties, indented under it; a
    // drive's volumes and a volume's mount are indented under theirs.
    QList<Volume> list;
    Volume volume;
    int volumeIndent = -1; // -1: no volume is being read
    bool hasMount = false, canMount = true, driveRemovable = false;
    auto finish = [&] {
        if (volumeIndent >= 0 && !hasMount && canMount && isDeviceName(volume.device) && !volume.name.isEmpty())
            list.append(volume);
        volumeIndent = -1;
    };
    static const QRegularExpression item(QStringLiteral("^(Drive|Volume|Mount)\\(\\d+\\): (.*)$"));
    const QStringList lines = QString::fromUtf8(output).split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        int indent = 0;
        while (indent < line.size() && line.at(indent) == QLatin1Char(' '))
            ++indent;
        const QString text = line.mid(indent);
        const QRegularExpressionMatch match = item.match(text);
        if (match.hasMatch()) {
            const QString kind = match.captured(1);
            if (kind == QLatin1String("Mount") && volumeIndent >= 0 && indent > volumeIndent) {
                hasMount = true; // this volume's own mount
                continue;
            }
            finish();
            if (kind == QLatin1String("Drive")) {
                driveRemovable = false;
            } else if (kind == QLatin1String("Volume")) {
                if (indent == 0)
                    driveRemovable = false; // a volume on no drive
                volume = Volume();
                volume.name = match.captured(2).simplified().left(100);
                volume.removable = driveRemovable;
                volumeIndent = indent;
                hasMount = false;
                canMount = true;
            }
            continue;
        }
        if (volumeIndent < 0) { // a drive's properties
            if (text == QLatin1String("is_removable=1") || text == QLatin1String("is_media_removable=1"))
                driveRemovable = true;
            continue;
        }
        if (indent <= volumeIndent || hasMount)
            continue;
        if (text.startsWith(QLatin1String("unix-device:")))
            volume.device = quoted(text);
        else if (text.startsWith(QLatin1String("uuid:")))
            volume.uuid = quoted(text);
        else if (text == QLatin1String("can_mount=0"))
            canMount = false;
    }
    finish();
    return list;
}

QList<Volumes::Volume> Volumes::unmounted(const QStringList &mounted)
{
    if (!isActive())
        return {};
    if (!m_asked.isValid() || m_asked.elapsed() > kStaleMs || mounted != m_mountedBefore) {
        m_mountedBefore = mounted;
        refresh();
    }
    return m_volumes;
}

void Volumes::refresh()
{
    if (!isActive())
        return;
    if (m_listing) {
        m_again = true; // once this one is done
        return;
    }
    m_asked.start();
    m_listing = new QProcess(this);
    m_listing->setProcessEnvironment(plainEnvironment());
    m_listing->setProgram(Tools::gio());
    m_listing->setArguments({QStringLiteral("mount"), QStringLiteral("-li")});
    auto done = [this](bool worked) {
        QProcess *process = m_listing;
        if (!process)
            return;
        m_listing = nullptr;
        const QList<Volume> now = worked ? parse(process->readAllStandardOutput()) : QList<Volume>();
        process->deleteLater();
        if (now != m_volumes) {
            m_volumes = now;
            emit changed();
        }
        if (m_again) {
            m_again = false;
            refresh();
        }
    };
    connect(m_listing, &QProcess::finished, this, [done](int code, QProcess::ExitStatus status) { done(status == QProcess::NormalExit && code == 0); });
    connect(m_listing, &QProcess::errorOccurred, this, [done](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            done(false);
    });
    QTimer::singleShot(8000, m_listing, [process = m_listing] { process->kill(); }); // a volume monitor that hangs
    m_listing->start(QIODevice::ReadOnly);
}

void Volumes::mount(const QString &device)
{
    if (!isActive() || !isDeviceName(device)) {
        emit failed(device, tr("That isn't a drive that can be mounted"));
        return;
    }
    if (m_mounting.contains(device))
        return;
    m_mounting.insert(device);
    auto *process = new QProcess(this);
    process->setProcessEnvironment(plainEnvironment());
    process->setProgram(Tools::gio());
    process->setArguments({QStringLiteral("mount"), QStringLiteral("-d"), device});
    process->setStandardInputFile(QProcess::nullDevice()); // nothing to type into: a password is the desktop's to ask
    auto done = [this, process, device](bool worked) {
        if (!m_mounting.remove(device))
            return;
        const QString said = QString::fromUtf8(process->readAllStandardOutput()).trimmed();
        QString problem = QString::fromUtf8(process->readAllStandardError()).trimmed();
        process->deleteLater();
        // "Mounted /dev/sdb1 at /media/ann/HITACHI"; else where the device is mounted now.
        QString folder;
        const qsizetype at = said.lastIndexOf(QLatin1String(" at "));
        if (worked && at > 0)
            folder = said.mid(at + 4).section(QLatin1Char('\n'), 0, 0).trimmed();
        if (worked && !QFileInfo(folder).isDir()) {
            folder.clear();
            const QList<QStorageInfo> mounts = QStorageInfo::mountedVolumes();
            for (const QStorageInfo &each : mounts) {
                if (QString::fromLocal8Bit(each.device()) == device)
                    folder = each.rootPath();
            }
        }
        refresh();
        if (!folder.isEmpty()) {
            emit mounted(device, folder);
            return;
        }
        if (problem.startsWith(QLatin1String("gio: ")))
            problem = problem.mid(5);
        if (problem.startsWith(device + QLatin1String(": ")))
            problem = problem.mid(device.size() + 2);
        emit failed(device, problem.isEmpty() ? tr("It could not be mounted") : problem.section(QLatin1Char('\n'), 0, 0).left(200));
    };
    connect(process, &QProcess::finished, this, [done](int code, QProcess::ExitStatus status) { done(status == QProcess::NormalExit && code == 0); });
    connect(process, &QProcess::errorOccurred, this, [done](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            done(false);
    });
    QTimer::singleShot(kMountTimeoutMs, process, [process] { process->kill(); });
    process->start(QIODevice::ReadOnly);
}
