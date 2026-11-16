// System Dashboard: an extension that shows how busy the computer is in the
// preview pane: processor, memory and disk as gauges, what the system is,
// and the network's download and upload.
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

#include "Format.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPath>
#include <QSaveFile>
#include <QSocketNotifier>
#include <QStorageInfo>
#include <QSysInfo>
#include <QThread>
#include <QTimer>

#include <cstdio>
#include <unistd.h>

namespace {

void send(const QJsonObject &message)
{
    const QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    std::fwrite(line.constData(), 1, size_t(line.size()), stdout);
    std::fflush(stdout);
}

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
        return m_numbers;
    }

private:
    static QByteArray fileText(const char *path)
    {
        QFile file(QString::fromLatin1(path));
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

    void read(Numbers &n)
    {
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
        }
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

// ------------------------------------------------------------ picture ---

// The colours and font of the picture: Argonaut's theme, as it tells them
// with "result" (so the dashboard looks like part of the window, in every
// theme), or a dark or light look of its own until it has.
struct Look
{
    QColor window, card, track, text, dim, accent;
    QString font;
};

QColor mix(const QColor &a, const QColor &b, double share)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * share, a.greenF() + (b.greenF() - a.greenF()) * share,
                            a.blueF() + (b.blueF() - a.blueF()) * share);
}

Look lookFor(bool dark, const QJsonObject &colors = {}, const QString &font = {})
{
    Look look = dark ? Look{QColor(0x1e, 0x1e, 0x1e), QColor(0x2a, 0x2a, 0x2a), {}, QColor(0xf0, 0xf0, 0xf0), QColor(0x9a, 0x9a, 0x9a), QColor(0x35, 0x84, 0xe4), {}}
                     : Look{QColor(0xfa, 0xfa, 0xfa), QColor(0xeb, 0xeb, 0xeb), {}, QColor(0x24, 0x24, 0x24), QColor(0x6a, 0x6a, 0x6a), QColor(0x1c, 0x71, 0xd8), {}};
    auto take = [&colors](const char *name, QColor *into) {
        const QColor color(colors.value(QLatin1String(name)).toString());
        if (color.isValid())
            *into = color;
    };
    take("window", &look.window);
    take("surface", &look.card);
    take("text", &look.text);
    take("dim", &look.dim);
    take("accent", &look.accent);
    // A theme whose panels are the background's colour still gets panels.
    if (look.card == look.window)
        look.card = mix(look.window, look.text, 0.06);
    look.track = mix(look.card, look.text, 0.10); // the empty part of a gauge
    look.font = font;
    return look;
}

// A gauge is in the theme's accent colour, and warns when it is nearly full.
QColor levelColor(const Look &look, double share)
{
    if (share >= 0.9)
        return QColor(0xe0, 0x1b, 0x24);
    if (share >= 0.75)
        return QColor(0xe6, 0x61, 0x00);
    return look.accent;
}

QString g_font; // the family every text is drawn in (empty: Qt's own)

void card(QPainter &p, const QRectF &r, const Look &look, double u)
{
    p.setPen(Qt::NoPen);
    p.setBrush(look.card);
    p.drawRoundedRect(r, u * 0.5, u * 0.5);
}

QFont fontOf(double pixels, bool bold = false)
{
    QFont font;
    if (!g_font.isEmpty())
        font.setFamily(g_font);
    font.setPixelSize(qMax(8, int(pixels + 0.5)));
    font.setBold(bold);
    return font;
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

// The whole dashboard, for a pane `width` pixels wide and at most `height`
// tall. A wide pane gets the system and the network side by side; a narrow
// one (the usual preview pane) gets them one under the other.
QImage draw(const Numbers &n, const QList<QPair<QString, QString>> &info, int width, int height, const Look &look)
{
    width = qBound(240, width, 4000);
    height = qBound(240, height, 4000);
    g_font = look.font;
    const bool wide = width >= 760 && width > height;
    // One unit: the size of ordinary text. Everything is laid out in units.
    double u = qBound(10.0, width / (wide ? 52.0 : 34.0), 26.0);
    const int rows = int(info.size());
    auto heights = [&](double unit, double *gaugeHeight, double *infoHeight, double *netHeight) {
        const double gaugeWidth = (width - 4 * unit) / 3;
        *gaugeHeight = qMin(gaugeWidth, unit * 15) + unit * 4.4;
        *infoHeight = unit * (3.2 + 1.65 * rows);
        *netHeight = unit * 6.3;
        return wide ? unit * 3 + *gaugeHeight + qMax(*infoHeight, 2 * *netHeight + unit)
                    : unit * 5 + *gaugeHeight + *infoHeight + 2 * *netHeight;
    };
    double gaugeHeight = 0, infoHeight = 0, netHeight = 0;
    double total = heights(u, &gaugeHeight, &infoHeight, &netHeight);
    if (total > height) { // a low pane: everything a little smaller, so nothing is scaled down later
        u = qMax(8.0, u * height / total);
        total = heights(u, &gaugeHeight, &infoHeight, &netHeight);
    }

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

    const double below = top + gaugeHeight + u;
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

bool save(const QImage &image, const QString &path)
{
    // Whole or not at all: Argonaut reads it as soon as it is told.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    return image.save(&file, "PNG", 80) && file.commit(); // 80: little compression, quick
}

} // namespace

int main(int argc, char **argv)
{
    // Pictures with text need Qt's drawing, but no window is ever shown.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    Sampler sampler;
    const QList<QPair<QString, QString>> info = systemInfo();

    if (app.arguments().value(1) == QLatin1String("--png")) {
        QThread::msleep(400); // two readings, for the rates
        const QImage image = draw(sampler.sample(QDir::homePath()), info, app.arguments().value(3, QStringLiteral("900")).toInt(),
                                  app.arguments().value(4, QStringLiteral("700")).toInt(),
                                  lookFor(app.arguments().value(5) != QLatin1String("light")));
        return save(image, app.arguments().value(2)) ? 0 : 1;
    }

    const QString id = QStringLiteral("dashboard");
    QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (runtime.isEmpty() || !QFileInfo(runtime).isWritable())
        runtime = QDir::tempPath();
    const QString picture = QStringLiteral("%1/argonaut-dashboard-%2.png").arg(runtime).arg(::getpid());
    QString folder;           // whose disk is shown
    int width = 560, height = 760;
    Look look = lookFor(true);
    bool shown = false;       // Argonaut has it on screen

    auto show = [&](bool update) {
        const Numbers &numbers = sampler.sample(folder);
        if (!save(draw(numbers, info, width, height, look), picture))
            return;
        QJsonObject message{{QStringLiteral("type"), QStringLiteral("show")},
                            {QStringLiteral("id"), id},
                            {QStringLiteral("title"), QStringLiteral("System Dashboard")},
                            {QStringLiteral("kind"), QStringLiteral("image")},
                            {QStringLiteral("image"), picture},
                            {QStringLiteral("note"), QStringLiteral("Disk: %1 · every 2 seconds").arg(numbers.diskName)}};
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

    // Argonaut's messages, a line each, as they come.
    QByteArray buffer;
    QSocketNotifier input(STDIN_FILENO, QSocketNotifier::Read);
    QObject::connect(&input, &QSocketNotifier::activated, &app, [&] {
        char chunk[8192];
        const ssize_t got = ::read(STDIN_FILENO, chunk, sizeof chunk);
        if (got <= 0) { // Argonaut closed the pipe
            input.setEnabled(false);
            app.quit();
            return;
        }
        buffer.append(chunk, qsizetype(got));
        qsizetype end;
        while ((end = buffer.indexOf('\n')) >= 0) {
            const QJsonObject message = QJsonDocument::fromJson(buffer.left(end)).object();
            buffer.remove(0, end + 1);
            const QString type = message.value(QLatin1String("type")).toString();
            if (type == QLatin1String("hello")) {
                send({{QStringLiteral("type"), QStringLiteral("commands")},
                      {QStringLiteral("commands"), QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("dashboard")},
                                                                          {QStringLiteral("label"), QStringLiteral("System Dashboard")},
                                                                          {QStringLiteral("menu"), QStringLiteral("view")}}}}});
            } else if (type == QLatin1String("invoke")) {
                folder = message.value(QLatin1String("folder")).toString();
                show(false); // Argonaut answers with the room there is
            } else if (type == QLatin1String("result") && message.value(QLatin1String("id")).toString() == id) {
                shown = message.value(QLatin1String("shown")).toBool();
                if (!shown) {
                    timer.stop();
                    continue;
                }
                width = message.value(QLatin1String("width")).toInt();
                height = message.value(QLatin1String("height")).toInt();
                look = lookFor(message.value(QLatin1String("dark")).toBool(), message.value(QLatin1String("colors")).toObject(),
                               message.value(QLatin1String("font")).toString().left(100));
                show(true); // drawn for this room, in this theme
                timer.start();
            }
        }
    });

    const int code = app.exec();
    QFile::remove(picture);
    return code;
}
