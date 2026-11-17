// System Dashboard: an extension that shows how busy the computer is in the
// preview pane: processor, memory and disk as gauges, its health (load,
// swap, temperature, what the drives say of themselves), the space on every
// disk, what the system is, and the network's download and upload.
//
// It offers one command, "System Dashboard", in the View menu (and so in
// the command palette). The answer is a picture it draws itself, sent as a named
// result ("show" with an "id"): Argonaut says how much room there is and
// whether the window is dark, the picture is drawn for that, and drawn again
// every two seconds until Argonaut says the result is no longer shown.
//
//   argonaut-dashboard-extension             the extension (talks on stdin/stdout)
//   argonaut-dashboard-extension --png FILE [WIDTH HEIGHT [dark|light]]
//                                            draws once into FILE (to look at it)
//   argonaut-dashboard-extension --text [COLUMNS]
//                                            prints it once as the terminal interface shows it

#include "../shared/ResultDrawing.h"
#include "Format.h"
#include "Tools.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QSaveFile>
#include <QSocketNotifier>
#include <QStorageInfo>
#include <QSysInfo>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cstdio>
#include <unistd.h>

namespace {

// ------------------------------------------------------------ numbers ---

struct Numbers
{
    double cpu = 0;        // 0…1
    qint64 memoryUsed = 0, memoryTotal = 0;
    qint64 diskUsed = 0, diskTotal = 0;
    QString diskName;      // where the disk is mounted
    qint64 downTotal = 0, upTotal = 0;
    double downRate = 0, upRate = 0;       // bytes a second
    double downPeak = 125000, upPeak = 125000; // the most seen (at least 1 Mbit/s), for the bars
    // Health: how much there is to do against the processors there are,
    // swap, and how warm it is (where the machine has sensors).
    double load[3] = {0, 0, 0}; // the average over 1, 5 and 15 minutes
    int cores = 1;
    qint64 swapUsed = 0, swapTotal = 0;
    double cpuTemperature = -1;     // °C; -1: no sensor
    double hottest = -1;            // the warmest sensor of all…
    QString hottestName;            // …and what it is ("nvme")
    // Every disk that is mounted (not only the open folder's).
    struct Disk
    {
        QString path; // where it is mounted
        qint64 used = 0, total = 0;
    };
    QList<Disk> disks;
};

// What a drive says of itself (SMART), as the desktop's disk service has it.
struct Drive
{
    QString model;
    qint64 size = 0;
    bool known = false;      // it has health data at all
    bool failing = false;    // the drive's own verdict, or an attribute over its limit
    qint64 badSectors = 0;   // a hard disk's
    int wear = -1;           // an NVMe drive's "percentage used"; -1: not told
    double temperature = -1; // °C
};

// 0: fine, 1: worth a look (orange), 2: bad (red).
int levelOf(const Drive &drive)
{
    if (drive.failing || drive.wear >= 100)
        return 2;
    return drive.badSectors > 0 || drive.wear >= 90 ? 1 : 0;
}

QString verdictOf(const Drive &drive)
{
    QStringList parts;
    if (!drive.known)
        return QStringLiteral("no health data");
    if (drive.failing)
        parts << QStringLiteral("FAILING: back it up now");
    else if (drive.badSectors > 0)
        parts << QStringLiteral("%1 bad sectors").arg(QLocale(QLocale::English).toString(drive.badSectors));
    else
        parts << QStringLiteral("OK");
    if (drive.wear >= 0)
        parts << QStringLiteral("%1% worn").arg(drive.wear);
    if (drive.temperature > 0)
        parts << QStringLiteral("%1 °C").arg(qRound(drive.temperature));
    return parts.join(QStringLiteral(" · "));
}

// `udisksctl dump`: a block an object, "Key:   value" lines under the name
// of an interface. Only real drives with a size are kept (not an empty
// card reader or a DVD drive).
QList<Drive> parseDrives(const QByteArray &dump)
{
    QList<Drive> drives;
    Drive drive;
    bool inDrive = false, removable = false, updated = false;
    auto finish = [&] {
        if (inDrive && drive.size > 0 && !(removable && !updated)) {
            drive.known = updated;
            drives.append(drive);
        }
        drive = Drive();
        inDrive = removable = updated = false;
    };
    for (const QByteArray &raw : dump.split('\n')) {
        if (raw.startsWith('/')) { // an object's path: the last one is over
            finish();
            inDrive = raw.startsWith("/org/freedesktop/UDisks2/drives/");
            continue;
        }
        if (!inDrive)
            continue;
        const qsizetype colon = raw.indexOf(':');
        if (colon < 0)
            continue;
        const QByteArray key = raw.left(colon).trimmed();
        const QByteArray value = raw.mid(colon + 1).trimmed();
        if (key == "Model")
            drive.model = QString::fromUtf8(value).simplified().left(60);
        else if (key == "Size")
            drive.size = qMax(drive.size, value.toLongLong());
        else if (key == "Removable")
            removable = value == "true";
        else if (key == "SmartUpdated")
            updated = value.toLongLong() > 0;
        else if (key == "SmartFailing")
            drive.failing = drive.failing || value == "true";
        else if (key == "SmartNumAttributesFailing")
            drive.failing = drive.failing || value.toInt() > 0;
        else if (key == "SmartNumBadSectors")
            drive.badSectors = qMax<qint64>(0, value.toLongLong());
        else if (key == "SmartTemperature" && value.toDouble() > 200) // in kelvin
            drive.temperature = value.toDouble() - 273.15;
        else if (key == "SmartPercentageUsed")
            drive.wear = value.toInt();
        else if (key == "SmartCriticalWarning" && !value.isEmpty() && value != "[]")
            drive.failing = true;
    }
    finish();
    return drives;
}

// Asks the disk service now and then (it reads the drives itself only every
// ten minutes), without waiting for it.
class DriveAsker : public QObject
{
public:
    DriveAsker()
    {
        m_process.setProgram(Tools::program("ARGONAUT_UDISKSCTL_PROGRAM", QStringLiteral("udisksctl")));
        m_process.setArguments({QStringLiteral("dump")});
        m_process.setStandardErrorFile(QProcess::nullDevice());
        QObject::connect(&m_process, &QProcess::finished, this, [this] { m_drives = parseDrives(m_process.readAllStandardOutput()); });
    }
    const QList<Drive> &drives()
    {
        if (m_process.state() == QProcess::NotRunning && (!m_asked.isValid() || m_asked.elapsed() > 60000)) {
            m_asked.start();
            m_process.start(QIODevice::ReadOnly);
        }
        return m_drives;
    }
    QList<Drive> askNow() // waited for: for --png and --text
    {
        m_process.blockSignals(true);
        m_process.start(QIODevice::ReadOnly);
        if (!m_process.waitForFinished(4000))
            m_process.kill();
        return parseDrives(m_process.readAllStandardOutput());
    }

private:
    QProcess m_process;
    QList<Drive> m_drives;
    QElapsedTimer m_asked;
};

// Reads /proc (Linux). Rates need two readings: the first comes when the
// program starts, so the first picture already has them.
class Sampler
{
public:
    Sampler() { read(m_numbers); }

    const Numbers &sample(const QString &folder)
    {
        read(m_numbers);
        QStorageInfo disk(folder.isEmpty() ? QStringLiteral("/") : folder);
        if (!disk.isValid() || disk.bytesTotal() <= 0)
            disk = QStorageInfo::root();
        m_numbers.diskTotal = disk.bytesTotal();
        m_numbers.diskUsed = disk.bytesTotal() - disk.bytesFree();
        m_numbers.diskName = disk.rootPath();
        readDisks(m_numbers);
        return m_numbers;
    }

private:
    static QByteArray fileText(const char *path)
    {
        QFile file(QString::fromLatin1(path));
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

    // Every mounted disk that is one: a real device, not a snap's image
    // or something the system keeps in memory. A device mounted twice is
    // said once.
    static void readDisks(Numbers &n)
    {
        n.disks.clear();
        QStringList seen;
        for (const QStorageInfo &volume : QStorageInfo::mountedVolumes()) {
            const QString device = QString::fromLocal8Bit(volume.device());
            const QString path = volume.rootPath();
            if (!volume.isValid() || !volume.isReady() || volume.bytesTotal() <= 0 || !device.startsWith(QLatin1String("/dev/"))
                || device.startsWith(QLatin1String("/dev/loop")) || volume.fileSystemType() == "squashfs" || path.startsWith(QLatin1String("/boot"))
                || path.startsWith(QLatin1String("/snap/")) || path.startsWith(QLatin1String("/var/lib/")) || seen.contains(device))
                continue;
            seen << device;
            n.disks.append({path, volume.bytesTotal() - volume.bytesFree(), volume.bytesTotal()});
        }
        std::sort(n.disks.begin(), n.disks.end(), [](const Numbers::Disk &a, const Numbers::Disk &b) { return a.path < b.path; });
        while (n.disks.size() > 10)
            n.disks.removeLast();
    }

    // How warm: the kernel's sensors, in thousandths of a degree. The
    // processor's own sensor is told apart by its driver's name.
    static void readTemperatures(Numbers &n)
    {
        n.cpuTemperature = -1;
        n.hottest = -1;
        n.hottestName.clear();
        const QDir sensors(QStringLiteral("/sys/class/hwmon"));
        for (const QString &entry : sensors.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QDir one(sensors.filePath(entry));
            const QString name = QString::fromLatin1(fileText(qPrintable(one.filePath(QStringLiteral("name")))).trimmed());
            const bool processor = name == QLatin1String("coretemp") || name == QLatin1String("k10temp") || name == QLatin1String("zenpower")
                                   || name.startsWith(QLatin1String("cpu"));
            for (const QString &file : one.entryList({QStringLiteral("temp*_input")}, QDir::Files)) {
                const double degrees = fileText(qPrintable(one.filePath(file))).trimmed().toDouble() / 1000.0;
                if (degrees <= 0 || degrees > 150)
                    continue; // a sensor that isn't connected
                if (processor)
                    n.cpuTemperature = qMax(n.cpuTemperature, degrees);
                if (degrees > n.hottest) {
                    n.hottest = degrees;
                    n.hottestName = name;
                }
            }
        }
    }

    void read(Numbers &n)
    {
        // How much there is to do: the load averages, against the processors there are.
        const QList<QByteArray> load = fileText("/proc/loadavg").simplified().split(' ');
        for (int i = 0; i < 3; ++i)
            n.load[i] = load.value(i).toDouble();
        n.cores = qMax(1, QThread::idealThreadCount());
        readTemperatures(n);
        // Processor: the share of time not idle since the last reading.
        const QList<QByteArray> cpu = fileText("/proc/stat").split('\n').value(0).simplified().split(' ');
        if (cpu.size() >= 6 && cpu.first() == "cpu") {
            quint64 total = 0;
            for (int i = 1; i < cpu.size() && i <= 8; ++i)
                total += cpu.at(i).toULongLong();
            const quint64 idle = cpu.at(4).toULongLong() + cpu.at(5).toULongLong(); // idle + waiting for the disk
            if (m_cpuTotal > 0 && total > m_cpuTotal)
                n.cpu = qBound(0.0, 1.0 - double(idle - m_cpuIdle) / double(total - m_cpuTotal), 1.0);
            m_cpuTotal = total;
            m_cpuIdle = idle;
        }
        // Memory: what programs could not get back (as `free` counts "used").
        qint64 total = 0, available = 0;
        for (const QByteArray &line : fileText("/proc/meminfo").split('\n')) {
            if (line.startsWith("MemTotal:"))
                total = line.mid(9).simplified().split(' ').value(0).toLongLong() * 1024;
            else if (line.startsWith("MemAvailable:"))
                available = line.mid(13).simplified().split(' ').value(0).toLongLong() * 1024;
            else if (line.startsWith("SwapTotal:"))
                n.swapTotal = line.mid(10).simplified().split(' ').value(0).toLongLong() * 1024;
            else if (line.startsWith("SwapFree:"))
                n.swapUsed = n.swapTotal - line.mid(9).simplified().split(' ').value(0).toLongLong() * 1024;
        }
        n.swapUsed = qBound<qint64>(0, n.swapUsed, n.swapTotal);
        n.memoryTotal = total;
        n.memoryUsed = qMax<qint64>(0, total - available);
        // Network: every interface but the loopback.
        qint64 down = 0, up = 0;
        const QList<QByteArray> lines = fileText("/proc/net/dev").split('\n');
        for (const QByteArray &line : lines) {
            const qsizetype colon = line.indexOf(':');
            if (colon < 0 || line.left(colon).trimmed() == "lo")
                continue;
            const QList<QByteArray> fields = line.mid(colon + 1).simplified().split(' ');
            if (fields.size() >= 9) {
                down += fields.at(0).toLongLong();
                up += fields.at(8).toLongLong();
            }
        }
        const double seconds = m_clock.isValid() ? m_clock.restart() / 1000.0 : 0;
        if (!m_clock.isValid())
            m_clock.start();
        if (seconds > 0.05) {
            n.downRate = qMax(0.0, double(down - n.downTotal) / seconds);
            n.upRate = qMax(0.0, double(up - n.upTotal) / seconds);
            n.downPeak = qMax(n.downPeak, n.downRate);
            n.upPeak = qMax(n.upPeak, n.upRate);
        }
        n.downTotal = down;
        n.upTotal = up;
    }

    Numbers m_numbers;
    quint64 m_cpuTotal = 0, m_cpuIdle = 0;
    QElapsedTimer m_clock;
};

// What the system is: read once.
QList<QPair<QString, QString>> systemInfo()
{
    QString model, speed;
    QFile cpuinfo(QStringLiteral("/proc/cpuinfo"));
    if (cpuinfo.open(QIODevice::ReadOnly)) {
        const QList<QByteArray> lines = cpuinfo.readAll().split('\n');
        for (const QByteArray &line : lines) {
            if (line.startsWith("model name")) {
                model = QString::fromUtf8(line.mid(line.indexOf(':') + 1)).simplified();
                break;
            }
        }
    }
    const qsizetype at = model.indexOf(QLatin1String(" @ "));
    if (at > 0) { // "Intel(R) Core(TM) i7-3770 CPU @ 3.40GHz"
        speed = model.mid(at + 3);
        model = model.left(at);
    }
    QList<QPair<QString, QString>> info{
        {QStringLiteral("Hostname"), QSysInfo::machineHostName()},
        {QStringLiteral("Platform"), QSysInfo::kernelType() + QLatin1Char(' ') + QSysInfo::currentCpuArchitecture()},
        {QStringLiteral("Distribution"), QSysInfo::prettyProductName()},
        {QStringLiteral("Kernel"), QSysInfo::kernelVersion()},
        {QStringLiteral("Processor"), model},
        {QStringLiteral("Cores"), QString::number(QThread::idealThreadCount())},
        {QStringLiteral("Speed"), speed},
    };
    QList<QPair<QString, QString>> known;
    for (const auto &row : info) {
        if (!row.second.isEmpty())
            known.append(row);
    }
    return known;
}

// A line of the health card: what, how it is, and whether that is fine (0),
// worth a look (1) or bad (2).
struct HealthRow
{
    QString label, value;
    int level = 0;
};

QList<HealthRow> healthRows(const Numbers &n, const QList<Drive> &drives)
{
    QList<HealthRow> rows;
    const double perCore = n.load[0] / n.cores;
    rows.append({QStringLiteral("Load"),
                 QStringLiteral("%1 · %2 · %3 (%4)")
                     .arg(QString::number(n.load[0], 'f', 2), QString::number(n.load[1], 'f', 2), QString::number(n.load[2], 'f', 2),
                          n.cores == 1 ? QStringLiteral("1 core") : QStringLiteral("%1 cores").arg(n.cores)),
                 perCore >= 2 ? 2 : perCore >= 1 ? 1 : 0});
    const double swapShare = n.swapTotal > 0 ? double(n.swapUsed) / double(n.swapTotal) : 0;
    rows.append({QStringLiteral("Swap"),
                 n.swapTotal > 0 ? QStringLiteral("%1 of %2").arg(formatSize(n.swapUsed), formatSize(n.swapTotal)) : QStringLiteral("none"),
                 swapShare >= 0.9 ? 2 : swapShare >= 0.5 ? 1 : 0});
    if (n.hottest > 0) {
        const double shown = n.cpuTemperature > 0 ? n.cpuTemperature : n.hottest;
        QString text = n.cpuTemperature > 0 ? QStringLiteral("processor %1 °C").arg(qRound(n.cpuTemperature))
                                            : QStringLiteral("%1 °C (%2)").arg(qRound(n.hottest)).arg(n.hottestName);
        if (n.cpuTemperature > 0 && n.hottest > n.cpuTemperature + 0.5)
            text += QStringLiteral(" · warmest %1 °C (%2)").arg(qRound(n.hottest)).arg(n.hottestName);
        const double worst = qMax(shown, n.hottest);
        rows.append({QStringLiteral("Temperature"), text, worst >= 95 ? 2 : worst >= 85 ? 1 : 0});
    }
    for (const Drive &drive : drives)
        rows.append({QStringLiteral("%1 (%2)").arg(drive.model.isEmpty() ? QStringLiteral("Drive") : drive.model, formatSize(drive.size)), verdictOf(drive),
                     levelOf(drive)});
    return rows;
}

// ------------------------------------------------------------ picture ---

// The health card: a line a thing, its value in orange or red when it is worth a look.
void healthCard(QPainter &p, const QRectF &r, const Look &look, double u, const QList<HealthRow> &rows)
{
    card(p, r, look, u);
    p.setPen(look.text);
    p.setFont(fontOf(u * 1.05));
    p.drawText(QRectF(r.left() + u, r.top() + u * 0.6, r.width() - 2 * u, u * 1.6), Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("HEALTH"));
    p.setFont(fontOf(u * 0.95));
    double y = r.top() + u * 2.5;
    for (const HealthRow &row : rows) {
        // The label, and under it the value: a drive's name is long, and so may what is said of it be.
        const QRectF line(r.left() + u, y, r.width() - 2 * u, u * 1.5);
        p.setPen(look.dim);
        p.drawText(line, Qt::AlignVCenter | Qt::AlignLeft, p.fontMetrics().elidedText(row.label, Qt::ElideMiddle, int(line.width())));
        p.setPen(row.level == 0 ? look.text : levelColor(look, row.level == 2 ? 1.0 : 0.8));
        p.drawText(line.translated(0, u * 1.4), Qt::AlignVCenter | Qt::AlignLeft, p.fontMetrics().elidedText(row.value, Qt::ElideRight, int(line.width())));
        y += u * 3.3;
    }
}

// The disks card: where each is mounted, a bar, and how much of it is used.
void disksCard(QPainter &p, const QRectF &r, const Look &look, double u, const QList<Numbers::Disk> &disks)
{
    card(p, r, look, u);
    p.setPen(look.text);
    p.setFont(fontOf(u * 1.05));
    p.drawText(QRectF(r.left() + u, r.top() + u * 0.6, r.width() - 2 * u, u * 1.6), Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("DISKS"));
    double y = r.top() + u * 2.5;
    for (const Numbers::Disk &disk : disks) {
        const QRectF line(r.left() + u, y, r.width() - 2 * u, u * 1.5);
        const double share = disk.total > 0 ? double(disk.used) / double(disk.total) : 0;
        const QString sizes = QStringLiteral("%1 / %2").arg(formatSize(disk.used), formatSize(disk.total));
        p.setFont(fontOf(u * 0.95));
        p.setPen(look.dim);
        p.drawText(line, Qt::AlignVCenter | Qt::AlignRight, sizes);
        p.setPen(look.text);
        const int sizesWidth = p.fontMetrics().horizontalAdvance(sizes);
        p.drawText(line, Qt::AlignVCenter | Qt::AlignLeft, p.fontMetrics().elidedText(disk.path, Qt::ElideMiddle, qMax(10, int(line.width() - sizesWidth - u))));
        const QRectF bar(line.left(), y + u * 1.75, line.width(), u * 0.5);
        p.setPen(Qt::NoPen);
        p.setBrush(look.track);
        p.drawRoundedRect(bar, bar.height() / 2, bar.height() / 2);
        p.setBrush(levelColor(look, share));
        p.drawRoundedRect(QRectF(bar.left(), bar.top(), qMax(bar.height(), bar.width() * qBound(0.0, share, 1.0)), bar.height()), bar.height() / 2,
                          bar.height() / 2);
        y += u * 3.0;
    }
}

// A gauge card: a name, an arc of three quarters of a circle with `share`
// of it filled, and a line of text under it.
void gauge(QPainter &p, const QRectF &r, const Look &look, double u, const QString &name, double share, const QString &value)
{
    const QColor color = levelColor(look, share);
    card(p, r, look, u);
    p.setPen(look.text);
    p.setFont(fontOf(u * 1.05));
    p.drawText(QRectF(r.left(), r.top() + u * 0.6, r.width(), u * 1.6), Qt::AlignCenter, name);
    const double room = qMin(r.width() - u * 1.6, r.height() - u * 5.2);
    const double thickness = qMax(4.0, room * 0.17);
    const QRectF circle(r.center().x() - room / 2 + thickness / 2, r.top() + u * 2.6 + thickness / 2, room - thickness, room - thickness);
    QPen pen(look.track, thickness, Qt::SolidLine, Qt::FlatCap);
    p.setBrush(Qt::NoBrush);
    p.setPen(pen);
    // From the lower left, clockwise, three quarters round (angles in 1/16°).
    p.drawArc(circle, 225 * 16, -270 * 16);
    pen.setColor(color);
    p.setPen(pen);
    p.drawArc(circle, 225 * 16, -int(270 * 16 * qBound(0.0, share, 1.0)));
    p.setPen(look.text);
    p.setFont(fontOf(u));
    const QRectF line(r.left() + u * 0.3, r.bottom() - u * 2.3, r.width() - u * 0.6, u * 1.6);
    p.drawText(line, Qt::AlignCenter, p.fontMetrics().elidedText(value, Qt::ElideMiddle, int(line.width())));
}

// A network card: a name, a bar (the rate against the most seen), the total
// and the rate.
void network(QPainter &p, const QRectF &r, const Look &look, double u, const QString &name, double rate, double peak,
             qint64 total, const QColor &color)
{
    card(p, r, look, u);
    p.setPen(look.text);
    p.setFont(fontOf(u * 1.05));
    p.drawText(QRectF(r.left() + u, r.top() + u * 0.6, r.width() - 2 * u, u * 1.6), Qt::AlignVCenter | Qt::AlignLeft, name);
    const QRectF bar(r.left() + u, r.top() + u * 2.7, r.width() - 2 * u, u * 0.9);
    p.setPen(Qt::NoPen);
    p.setBrush(look.track);
    p.drawRect(bar);
    p.setBrush(color);
    p.drawRect(QRectF(bar.left(), bar.top(), bar.width() * qBound(0.0, peak > 0 ? rate / peak : 0.0, 1.0), bar.height()));
    const QRectF line(r.left() + u, r.top() + u * 4.1, r.width() - 2 * u, u * 1.6);
    p.setFont(fontOf(u * 0.9));
    p.setPen(look.dim);
    p.drawText(line, Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("Total: %1").arg(formatSize(total)));
    p.setPen(look.text);
    p.setFont(fontOf(u));
    p.drawText(line, Qt::AlignVCenter | Qt::AlignRight, formatSize(qint64(rate)) + QStringLiteral("/s"));
}

void systemCard(QPainter &p, const QRectF &r, const Look &look, double u, const QList<QPair<QString, QString>> &info)
{
    card(p, r, look, u);
    p.setPen(look.text);
    p.setFont(fontOf(u * 1.05));
    p.drawText(QRectF(r.left() + u, r.top() + u * 0.6, r.width() - 2 * u, u * 1.6), Qt::AlignVCenter | Qt::AlignLeft,
               QStringLiteral("SYSTEM INFO"));
    p.setFont(fontOf(u * 0.95));
    double y = r.top() + u * 2.5;
    for (const auto &row : info) {
        const QRectF line(r.left() + u, y, r.width() - 2 * u, u * 1.65);
        const QString label = row.first + QStringLiteral(": ");
        p.setPen(look.dim);
        p.drawText(line, Qt::AlignVCenter | Qt::AlignLeft, label);
        const int used = p.fontMetrics().horizontalAdvance(label);
        p.setPen(look.text);
        p.drawText(line.adjusted(used, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft,
                   p.fontMetrics().elidedText(row.second, Qt::ElideRight, qMax(10, int(line.width()) - used)));
        y += u * 1.65;
    }
}

// The whole dashboard, for a pane `width` pixels wide. It is as long as what
// it has to show: Argonaut scrolls it where it is taller than the pane. A
// wide pane gets cards side by side; a narrow one (the usual preview pane)
// gets them one under the other.
QImage draw(const Numbers &n, const QList<QPair<QString, QString>> &info, const QList<Drive> &drives, int width, int height, const Look &look)
{
    const QList<HealthRow> health = healthRows(n, drives);
    width = qBound(240, width, 4000);
    height = qBound(240, height, 4000);
    g_font = look.font;
    const bool wide = width >= 760 && width > height;
    // One unit: the size of ordinary text. Everything is laid out in units.
    double u = qBound(10.0, width / (wide ? 52.0 : 34.0), 26.0);
    const int rows = int(info.size());
    const double gaugeHeight = qMin((width - 4 * u) / 3, u * 15) + u * 4.4;
    const double infoHeight = u * (3.2 + 1.65 * rows);
    const double netHeight = u * 6.3;
    const double healthHeight = u * (3.0 + 3.3 * health.size());
    const double disksHeight = n.disks.isEmpty() ? 0 : u * (3.0 + 3.0 * n.disks.size());
    // Under the gauges: health and the disks (side by side when wide), then the system and the network.
    const double healthBlock = wide ? qMax(healthHeight, disksHeight) + u : healthHeight + u + (disksHeight > 0 ? disksHeight + u : 0);
    const double total = wide ? u * 3 + gaugeHeight + healthBlock + qMax(infoHeight, 2 * netHeight + u)
                              : u * 5 + gaugeHeight + healthBlock + infoHeight + 2 * netHeight;
    Q_UNUSED(height);

    QImage image(width, int(total + 0.5), QImage::Format_RGB32);
    image.fill(look.window);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);

    const double gaugeWidth = (width - 4 * u) / 3;
    const double top = u;
    gauge(p, QRectF(u, top, gaugeWidth, gaugeHeight), look, u, QStringLiteral("CPU"), n.cpu,
          QStringLiteral("%1%").arg(qRound(n.cpu * 100)));
    gauge(p, QRectF(2 * u + gaugeWidth, top, gaugeWidth, gaugeHeight), look, u, QStringLiteral("MEMORY"),
          n.memoryTotal > 0 ? double(n.memoryUsed) / double(n.memoryTotal) : 0,
          QStringLiteral("%1 / %2").arg(formatSize(n.memoryUsed), formatSize(n.memoryTotal)));
    gauge(p, QRectF(3 * u + 2 * gaugeWidth, top, gaugeWidth, gaugeHeight), look, u, QStringLiteral("DISK"),
          n.diskTotal > 0 ? double(n.diskUsed) / double(n.diskTotal) : 0,
          QStringLiteral("%1 / %2").arg(formatSize(n.diskUsed), formatSize(n.diskTotal)));

    double below = top + gaugeHeight + u;
    if (wide) {
        const double half = (width - 3 * u) / 2;
        const double tall = qMax(healthHeight, disksHeight);
        healthCard(p, QRectF(u, below, disksHeight > 0 ? half : width - 2 * u, tall), look, u, health);
        if (disksHeight > 0)
            disksCard(p, QRectF(2 * u + half, below, half, tall), look, u, n.disks);
    } else {
        healthCard(p, QRectF(u, below, width - 2 * u, healthHeight), look, u, health);
        if (disksHeight > 0)
            disksCard(p, QRectF(u, below + healthHeight + u, width - 2 * u, disksHeight), look, u, n.disks);
    }
    below += healthBlock;
    if (wide) {
        const double infoWidth = (width - 3 * u) * 0.46;
        const double netWidth = width - 3 * u - infoWidth;
        systemCard(p, QRectF(u, below, infoWidth, qMax(infoHeight, 2 * netHeight + u)), look, u, info);
        network(p, QRectF(2 * u + infoWidth, below, netWidth, netHeight), look, u, QStringLiteral("DOWNLOAD"), n.downRate,
                n.downPeak, n.downTotal, look.accent);
        network(p, QRectF(2 * u + infoWidth, below + netHeight + u, netWidth, netHeight), look, u, QStringLiteral("UPLOAD"),
                n.upRate, n.upPeak, n.upTotal, look.accent);
    } else {
        systemCard(p, QRectF(u, below, width - 2 * u, infoHeight), look, u, info);
        network(p, QRectF(u, below + infoHeight + u, width - 2 * u, netHeight), look, u, QStringLiteral("DOWNLOAD"), n.downRate,
                n.downPeak, n.downTotal, look.accent);
        network(p, QRectF(u, below + infoHeight + netHeight + 2 * u, width - 2 * u, netHeight), look, u, QStringLiteral("UPLOAD"),
                n.upRate, n.upPeak, n.upTotal, look.accent);
    }
    return image;
}

// ---------------------------------------------- the same, in a terminal ---
// Argonaut's terminal interface has character cells, not pixels: a picture
// there would be a blur. So for it the dashboard is text with colour codes
// ("ansi"): the same three gauges, drawn as rings of half blocks (two
// "pixels" to the cell), the same system lines, the same network bars.

// A gauge's ring, `cells` wide: one string a screen line. Painted small
// (a pixel to half a cell) with the colours as numbers, then read back.
QStringList ring(int cells, double share)
{
    const int rows = qMax(2, cells / 2);
    QImage image(cells, rows * 2, QImage::Format_RGB32);
    image.fill(Qt::black); // black: nothing here
    {
        QPainter p(&image);
        const double thickness = qMax(1.6, cells * 0.17);
        const QRectF circle(thickness / 2 + 0.5, thickness / 2 + 0.5, cells - thickness - 1, rows * 2 - thickness - 1);
        QPen pen(QColor(0, 0, 1), thickness, Qt::SolidLine, Qt::FlatCap); // blue 1: the track
        p.setPen(pen);
        p.drawArc(circle, 225 * 16, -270 * 16);
        pen.setColor(QColor(0, 0, 2)); // blue 2: the filled part
        p.setPen(pen);
        p.drawArc(circle, 225 * 16, -int(270 * 16 * qBound(0.0, share, 1.0)));
    }
    const int value = levelIndex(share);
    auto colorAt = [&](int x, int y) {
        const int mark = qBlue(image.pixel(x, y));
        return mark == 2 ? value : mark == 1 ? kTrack : -1;
    };
    QStringList lines;
    for (int row = 0; row < rows; ++row) {
        QString line;
        for (int x = 0; x < cells; ++x) {
            const int top = colorAt(x, row * 2);
            const int bottom = colorAt(x, row * 2 + 1);
            if (top < 0 && bottom < 0)
                line += kReset + QLatin1Char(' ');
            else if (top >= 0 && bottom >= 0)
                line += sgr(top, bottom) + QChar(0x2580);
            else if (top >= 0)
                line += sgr(top) + QChar(0x2580); // the upper half
            else
                line += sgr(bottom) + QChar(0x2584); // the lower half
        }
        lines << line + kReset;
    }
    return lines;
}

QString centred(const QString &text, int width)
{
    const QString cut = text.left(width);
    const int left = (width - int(cut.size())) / 2;
    return QString(left, QLatin1Char(' ')) + cut + QString(width - left - int(cut.size()), QLatin1Char(' '));
}

// "6.9 / 33.6 GB" where both are in the same unit, and "579 / 983 GB"
// where that is still too long: it has to fit under a gauge.
QString usedOfTotal(qint64 used, qint64 total, int width)
{
    const QString a = formatSize(used), b = formatSize(total);
    const QString unitA = a.section(QLatin1Char(' '), -1), unitB = b.section(QLatin1Char(' '), -1);
    const QString text = (unitA == unitB ? a.section(QLatin1Char(' '), 0, 0) : a) + QStringLiteral(" / ") + b;
    if (text.size() <= width)
        return text;
    auto whole = [](const QString &size) {
        return QString::number(qRound(size.section(QLatin1Char(' '), 0, 0).toDouble())) + QLatin1Char(' ') + size.section(QLatin1Char(' '), -1);
    };
    const QString shortA = whole(a), shortB = whole(b);
    return (unitA == unitB ? shortA.section(QLatin1Char(' '), 0, 0) : shortA) + QStringLiteral(" / ") + shortB;
}

QString drawAnsi(const Numbers &n, const QList<QPair<QString, QString>> &info, const QList<Drive> &drives, int columns)
{
    columns = qBound(24, columns, 200);
    const int gauge = qBound(6, (columns - 2) / 3, 18);
    const int gap = qMax(1, qMin(3, (columns - 3 * gauge) / 2));
    const QString between(gap, QLatin1Char(' '));
    const double shares[3] = {n.cpu, n.memoryTotal > 0 ? double(n.memoryUsed) / double(n.memoryTotal) : 0,
                              n.diskTotal > 0 ? double(n.diskUsed) / double(n.diskTotal) : 0};
    const QString names[3] = {QStringLiteral("CPU"), QStringLiteral("MEMORY"), QStringLiteral("DISK")};
    const QString values[3] = {QStringLiteral("%1%").arg(qRound(n.cpu * 100)), usedOfTotal(n.memoryUsed, n.memoryTotal, gauge),
                               usedOfTotal(n.diskUsed, n.diskTotal, gauge)};
    QStringList out;
    QString line;
    for (int i = 0; i < 3; ++i)
        line += kBold + centred(names[i], gauge) + kReset + (i < 2 ? between : QString());
    out << line;
    const QStringList rings[3] = {ring(gauge, shares[0]), ring(gauge, shares[1]), ring(gauge, shares[2])};
    for (int row = 0; row < rings[0].size(); ++row)
        out << rings[0].at(row) + between + rings[1].at(row) + between + rings[2].at(row);
    line.clear();
    for (int i = 0; i < 3; ++i)
        line += centred(values[i], gauge) + (i < 2 ? between : QString());
    out << line << QString();

    // Health: a line a thing; its value in orange or red when it is worth a look.
    out << kBold + QStringLiteral("HEALTH") + kReset;
    for (const HealthRow &row : healthRows(n, drives)) {
        const QString colour = row.level == 2 ? sgr(kFull) : row.level == 1 ? sgr(kNearlyFull) : kReset;
        const QString label = row.label + QStringLiteral(": ");
        if (label.size() + row.value.size() <= columns) {
            out << kDim + label + colour + row.value + kReset;
        } else { // a drive's long name: its verdict on the next line
            out << kDim + row.label.left(columns) + kReset;
            out << colour + QStringLiteral("  ") + row.value.left(qMax(0, columns - 2)) + kReset;
        }
    }
    out << QString();
    if (!n.disks.isEmpty()) {
        out << kBold + QStringLiteral("DISKS") + kReset;
        const int whole = qMax(10, gauge * 3 + gap * 2);
        for (const Numbers::Disk &disk : n.disks) {
            const double share = disk.total > 0 ? double(disk.used) / double(disk.total) : 0;
            const QString sizes = QStringLiteral("%1 / %2").arg(formatSize(disk.used), formatSize(disk.total));
            const QString path = disk.path.size() > whole - sizes.size() - 1 ? QStringLiteral("…") + disk.path.right(qMax(1, whole - int(sizes.size()) - 2)) : disk.path;
            out << path + QString(qMax(1, whole - int(path.size()) - int(sizes.size())), QLatin1Char(' ')) + kDim + sizes + kReset;
            const int filled = int(whole * qBound(0.0, share, 1.0) + 0.5);
            out << sgr(share >= 0.9 ? kFull : share >= 0.75 ? kNearlyFull : kAccent) + QString(filled, QChar(0x2588)) + sgr(kTrack)
                       + QString(whole - filled, QChar(0x2588)) + kReset;
        }
        out << QString();
    }

    out << kBold + QStringLiteral("SYSTEM INFO") + kReset;
    for (const auto &row : info)
        out << kDim + row.first + QStringLiteral(": ") + kReset + row.second;
    out << QString();

    const int width = qMax(10, gauge * 3 + gap * 2);
    auto network = [&](const QString &name, double rate, double peak, qint64 total) {
        const QString speed = formatSize(qint64(rate)) + QStringLiteral("/s");
        out << kBold + name + kReset + QString(qMax(1, width - int(name.size()) - int(speed.size())), QLatin1Char(' ')) + speed;
        const int filled = int(width * qBound(0.0, peak > 0 ? rate / peak : 0.0, 1.0) + 0.5);
        out << sgr(kAccent) + QString(filled, QChar(0x2588)) + sgr(kTrack) + QString(width - filled, QChar(0x2588)) + kReset;
        out << kDim + QStringLiteral("Total: %1").arg(formatSize(total)) + kReset;
    };
    network(QStringLiteral("DOWNLOAD"), n.downRate, n.downPeak, n.downTotal);
    out << QString();
    network(QStringLiteral("UPLOAD"), n.upRate, n.upPeak, n.upTotal);
    return out.join(QLatin1Char('\n'));
}

} // namespace

int main(int argc, char **argv)
{
    // Pictures with text need Qt's drawing, but no window is ever shown.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    Sampler sampler;
    DriveAsker driveAsker; // what the drives say of themselves, asked now and then
    const QList<QPair<QString, QString>> info = systemInfo();

    if (app.arguments().value(1) == QLatin1String("--text")) { // as the terminal interface shows it
        QThread::msleep(400);
        std::printf("%s\n", qPrintable(drawAnsi(sampler.sample(QDir::homePath()), info, driveAsker.askNow(),
                                                app.arguments().value(2, QStringLiteral("44")).toInt())));
        return 0;
    }
    if (app.arguments().value(1) == QLatin1String("--png")) {
        QThread::msleep(400); // two readings, for the rates
        const QImage image = draw(sampler.sample(QDir::homePath()), info, driveAsker.askNow(), app.arguments().value(3, QStringLiteral("900")).toInt(),
                                  app.arguments().value(4, QStringLiteral("700")).toInt(),
                                  lookFor(app.arguments().value(5) != QLatin1String("light")));
        return save(image, app.arguments().value(2)) ? 0 : 1;
    }

    const QString id = QStringLiteral("dashboard");
    const QString picture = pictureFile("dashboard");
    QString folder;           // whose disk is shown
    Room room;             // what Argonaut says there is room for, and how the window looks
    bool shown = false;       // Argonaut has it on screen
    bool terminal = false;    // it is Argonaut's terminal interface: text, not a picture

    auto show = [&](bool update) {
        const Numbers &numbers = sampler.sample(folder);
        QJsonObject message{{QStringLiteral("type"), QStringLiteral("show")},
                            {QStringLiteral("id"), id},
                            {QStringLiteral("title"), QStringLiteral("System Dashboard")},
                            {QStringLiteral("note"), QStringLiteral("Disk: %1 · every 2 seconds").arg(numbers.diskName)}};
        if (terminal) {
            message.insert(QStringLiteral("kind"), QStringLiteral("ansi"));
            message.insert(QStringLiteral("text"), drawAnsi(numbers, info, driveAsker.drives(), room.columns));
        } else {
            if (!save(draw(numbers, info, driveAsker.drives(), room.width, room.height, room.look), picture))
                return;
            message.insert(QStringLiteral("kind"), QStringLiteral("image"));
            message.insert(QStringLiteral("image"), picture);
            message.insert(QStringLiteral("scroll"), true); // as long as what there is to show: Argonaut scrolls it
        }
        if (update)
            message.insert(QStringLiteral("update"), true);
        send(message);
    };

    QTimer timer;
    timer.setInterval(2000);
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        if (shown)
            show(true);
        else
            timer.stop();
    });

    // Argonaut's messages, each as it comes.
    Conversation conversation([&](const QJsonObject &message) {
        const QString type = message.value(QLatin1String("type")).toString();
        if (type == QLatin1String("hello")) {
            terminal = message.value(QLatin1String("interface")).toString() == QLatin1String("terminal");
            offerViewCommand(QStringLiteral("dashboard"), QStringLiteral("System Dashboard"));
        } else if (type == QLatin1String("invoke")) {
            folder = message.value(QLatin1String("folder")).toString();
            show(false); // Argonaut answers with the room there is
        } else if (type == QLatin1String("result") && message.value(QLatin1String("id")).toString() == id) {
            shown = message.value(QLatin1String("shown")).toBool();
            if (!shown) {
                timer.stop();
                return;
            }
            room.read(message);
            show(true); // drawn for this room, in this theme
            timer.start();
        }
    });

    const int code = app.exec();
    QFile::remove(picture);
    return code;
}
