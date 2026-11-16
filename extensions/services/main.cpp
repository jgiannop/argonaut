// Running Services: an extension that shows, in the preview pane, the
// services that are running now, the one using the most memory first.
//
// The services are systemd's: the system's, and the ones of the user's own
// session (marked "user"; "user@1000", which is that whole session and not
// a service of its own, is left out). They are asked for with `systemctl show`
// (ARGONAUT_SYSTEMCTL_PROGRAM in tests), which says each one's memory
// (its whole control group: every process of the service). Nothing is
// started, stopped or changed: it only shows.
//
// It offers one command, "Running Services", in the View menu (and so in
// the command palette). The answer is a named result ("show" with an "id")
// that is made again every three seconds while it is on screen: a picture
// in the theme's colours, as long as the list is (Argonaut scrolls it), or,
// in the terminal interface, text with colour codes.
//
//   argonaut-services-extension              the extension (talks on stdin/stdout)
//   argonaut-services-extension --png FILE [WIDTH [dark|light]]
//                                            draws once into FILE (to look at it)
//   argonaut-services-extension --text [COLUMNS]
//                                            prints it once as the terminal interface shows it

#include "../shared/ResultDrawing.h"
#include "Format.h"
#include "Tools.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QProcess>
#include <QSocketNotifier>
#include <QTimer>

#include <algorithm>
#include <functional>
#include <unistd.h>

namespace {

struct Service
{
    QString name;        // without ".service"
    QString description;
    qint64 bytes = -1;   // -1: not known
    bool user = false;   // of the user's session, not the system's
};

struct Services
{
    QList<Service> list;  // most memory first
    int system = 0, user = 0;
    qint64 used = 0;      // their memory together
    qint64 memoryTotal = 0;
    QString problem;      // why there is no list
};

qint64 numberIn(const QString &path, const char *prefix = nullptr)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return -1;
    const QList<QByteArray> lines = file.readAll().split('\n');
    for (const QByteArray &line : lines) {
        if (!prefix)
            return line.trimmed().isEmpty() ? -1 : line.trimmed().toLongLong();
        if (line.startsWith(prefix)) // "VmRSS:   1234 kB"
            return line.mid(qsizetype(qstrlen(prefix))).simplified().split(' ').value(0).toLongLong() * 1024;
    }
    return -1;
}

// What `systemctl show` printed: a block of Key=Value lines a service,
// an empty line between them.
QList<Service> parse(const QByteArray &output, bool user)
{
    QList<Service> list;
    const QList<QByteArray> blocks = QByteArray(output).replace("\r", "").split('\n');
    Service service;
    QString group;
    qint64 pid = 0;
    bool any = false;
    auto finish = [&] {
        if (any && !service.name.isEmpty()) {
            // Where systemd doesn't count a service's memory: its control
            // group's own count, else its main process.
            if (service.bytes < 0 && group.startsWith(QLatin1Char('/')) && !group.contains(QLatin1String("..")))
                service.bytes = numberIn(QStringLiteral("/sys/fs/cgroup") + group + QStringLiteral("/memory.current"));
            if (service.bytes < 0 && pid > 0)
                service.bytes = numberIn(QStringLiteral("/proc/%1/status").arg(pid), "VmRSS:");
            service.user = user;
            list.append(service);
        }
        service = Service();
        group.clear();
        pid = 0;
        any = false;
    };
    for (const QByteArray &line : blocks) {
        if (line.trimmed().isEmpty()) {
            finish();
            continue;
        }
        const qsizetype equals = line.indexOf('=');
        if (equals <= 0)
            continue;
        const QByteArray key = line.left(equals);
        const QString value = QString::fromUtf8(line.mid(equals + 1)).simplified().left(200);
        any = true;
        if (key == "Id") {
            service.name = value.endsWith(QLatin1String(".service")) ? value.chopped(8) : value;
        } else if (key == "Description") {
            service.description = value;
        } else if (key == "MemoryCurrent") {
            bool ok = false;
            const qint64 bytes = value.toLongLong(&ok); // "[not set]" where it isn't counted
            if (ok && bytes >= 0)
                service.bytes = bytes;
        } else if (key == "ControlGroup") {
            group = value;
        } else if (key == "MainPID") {
            pid = value.toLongLong();
        }
    }
    finish();
    return list;
}

QStringList arguments(bool user)
{
    QStringList list;
    if (user)
        list << QStringLiteral("--user");
    list << QStringLiteral("show") << QStringLiteral("--type=service") << QStringLiteral("--state=running");
    for (const char *property : {"Id", "Description", "MemoryCurrent", "ControlGroup", "MainPID"})
        list << QStringLiteral("-p") << QString::fromLatin1(property);
    list << QStringLiteral("*");
    return list;
}

Services together(QList<Service> system, const QList<Service> &user, const QString &problem)
{
    Services all;
    // "user@1000" is no service of its own: it is a user's whole session,
    // every program they run and the user services below included. Left in,
    // it would be counted twice and dwarf everything else.
    system.erase(std::remove_if(system.begin(), system.end(), [](const Service &service) { return service.name.startsWith(QLatin1String("user@")); }),
                 system.end());
    all.system = int(system.size());
    all.user = int(user.size());
    all.list = system + user;
    for (const Service &service : std::as_const(all.list))
        all.used += qMax<qint64>(0, service.bytes);
    std::stable_sort(all.list.begin(), all.list.end(), [](const Service &a, const Service &b) {
        return a.bytes != b.bytes ? a.bytes > b.bytes : a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    all.memoryTotal = numberIn(QStringLiteral("/proc/meminfo"), "MemTotal:");
    if (all.list.isEmpty())
        all.problem = problem.isEmpty() ? QStringLiteral("No running services were found") : problem;
    return all;
}

// Asks systemd, without waiting: `done` gets the answer. One question at a
// time; a systemctl that doesn't answer in five seconds is stopped.
class Asker : public QObject
{
public:
    explicit Asker(std::function<void(const Services &)> done)
        : m_done(std::move(done))
    {
        for (int i = 0; i < 2; ++i) {
            m_process[i].setProgram(Tools::program("ARGONAUT_SYSTEMCTL_PROGRAM", QStringLiteral("systemctl")));
            m_process[i].setArguments(arguments(i == 1));
            QObject::connect(&m_process[i], &QProcess::finished, this, [this] { oneDone(); });
            QObject::connect(&m_process[i], &QProcess::errorOccurred, this, [this, i](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart) {
                    m_problem = QStringLiteral("systemctl isn't there: this needs systemd");
                    oneDone();
                }
                Q_UNUSED(i);
            });
        }
        m_timeout.setSingleShot(true);
        m_timeout.setInterval(5000);
        QObject::connect(&m_timeout, &QTimer::timeout, this, [this] {
            for (QProcess &process : m_process)
                process.kill();
        });
    }

    bool isAsking() const { return m_waiting > 0; }

    void ask()
    {
        if (m_waiting > 0)
            return;
        m_problem.clear();
        m_waiting = 2;
        for (QProcess &process : m_process)
            process.start(QIODevice::ReadOnly);
        m_timeout.start();
    }

    // The same, waited for: for --png and --text.
    Services askNow()
    {
        Services result;
        QList<Service> lists[2];
        QString problem;
        for (int i = 0; i < 2; ++i) {
            m_process[i].blockSignals(true);
            m_process[i].start(QIODevice::ReadOnly);
            if (!m_process[i].waitForFinished(5000)) {
                m_process[i].kill();
                m_process[i].waitForFinished(1000);
                problem = QStringLiteral("systemctl didn't answer: this needs systemd");
            }
            lists[i] = parse(m_process[i].readAllStandardOutput(), i == 1);
        }
        return together(lists[0], lists[1], problem);
    }

private:
    void oneDone()
    {
        if (m_waiting <= 0 || --m_waiting > 0)
            return;
        m_timeout.stop();
        m_done(together(parse(m_process[0].readAllStandardOutput(), false), parse(m_process[1].readAllStandardOutput(), true), m_problem));
    }

    std::function<void(const Services &)> m_done;
    QProcess m_process[2]; // the system's, the user's
    QTimer m_timeout;
    int m_waiting = 0;
    QString m_problem;
};

QString sizeOf(const Service &service)
{
    return service.bytes < 0 ? QStringLiteral("—") : formatSize(service.bytes);
}

QString countsText(const Services &s)
{
    return QStringLiteral("%1 system · %2 user").arg(s.system).arg(s.user);
}

QString usedText(const Services &s)
{
    return s.memoryTotal > 0 ? QStringLiteral("Together they use %1 of %2 memory").arg(formatSize(s.used), formatSize(s.memoryTotal))
                             : QStringLiteral("Together they use %1").arg(formatSize(s.used));
}

// ------------------------------------------------------------ picture ---

// For a pane `width` pixels wide: every service, however tall that makes
// the picture (Argonaut scrolls it: "scroll"), up to a few hundred.
QImage draw(const Services &s, int width, const Look &look)
{
    width = qBound(240, width, 4000);
    g_font = look.font;
    const double u = qBound(10.0, width / 34.0, 20.0); // the size of ordinary text
    const double rowHeight = u * 2.7;
    const double headHeight = u * 9.6;
    const double listTop = headHeight + 2 * u;
    const int count = int(s.list.size());
    const int shown = qMin(count, 250); // a picture has a limit to its height
    const bool more = count > shown;
    const double listHeight = u * 3.2 + rowHeight * qMax(1, shown) + (more ? u * 1.9 : 0);

    QImage image(width, int(listTop + listHeight + u + 0.5), QImage::Format_RGB32);
    image.fill(look.window);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    const double left = 2 * u, room = width - 4 * u;
    auto text = [&](double y, double lineHeight, int flags, const QString &string) {
        p.drawText(QRectF(left, y, room, lineHeight), flags | Qt::AlignVCenter, p.fontMetrics().elidedText(string, Qt::ElideRight, int(room)));
    };
    auto bar = [&](double y, double barHeight, double share, const QColor &color) {
        p.setPen(Qt::NoPen);
        p.setBrush(look.track);
        p.drawRoundedRect(QRectF(left, y, room, barHeight), barHeight / 2, barHeight / 2);
        const double filled = room * qBound(0.0, share, 1.0);
        if (filled >= 1) {
            p.setBrush(color);
            p.drawRoundedRect(QRectF(left, y, qMax(filled, barHeight), barHeight), barHeight / 2, barHeight / 2);
        }
    };

    // How many, and how much of the memory they have together.
    card(p, QRectF(u, u, width - 2 * u, headHeight), look, u);
    p.setPen(look.dim);
    p.setFont(fontOf(u));
    text(u * 1.5, u * 1.5, Qt::AlignLeft, QStringLiteral("RUNNING SERVICES"));
    p.setPen(look.text);
    p.setFont(fontOf(u * 2.1, true));
    text(u * 3.1, u * 2.8, Qt::AlignLeft, QString::number(count));
    p.setPen(look.dim);
    p.setFont(fontOf(u));
    text(u * 3.9, u * 1.8, Qt::AlignRight, countsText(s));
    const double share = s.memoryTotal > 0 ? double(s.used) / double(s.memoryTotal) : 0;
    bar(u * 7.0, u * 0.8, share, levelColor(look, share));
    p.setPen(look.dim);
    p.setFont(fontOf(u * 0.9));
    text(u * 8.3, u * 1.6, Qt::AlignLeft, usedText(s));

    // The services, most memory first. A bar is against the biggest.
    card(p, QRectF(u, listTop, width - 2 * u, listHeight), look, u);
    p.setPen(look.text);
    p.setFont(fontOf(u * 1.05));
    text(listTop + u * 0.7, u * 1.6, Qt::AlignLeft, QStringLiteral("BY MEMORY"));
    double y = listTop + u * 2.8;
    if (count == 0) {
        p.setPen(look.dim);
        p.setFont(fontOf(u));
        text(y, u * 1.6, Qt::AlignLeft, s.problem);
    }
    const qint64 biggest = count > 0 ? qMax<qint64>(1, s.list.first().bytes) : 1;
    for (int i = 0; i < shown; ++i, y += rowHeight) {
        const Service &service = s.list.at(i);
        p.setFont(fontOf(u));
        const QString size = sizeOf(service);
        const int sizeWidth = p.fontMetrics().horizontalAdvance(size);
        p.setPen(look.dim);
        p.drawText(QRectF(left, y, room, u * 1.6), Qt::AlignRight | Qt::AlignVCenter, size);
        // The name, then what it is (or that it is the user's) in the room that is left.
        const double nameRoom = room - sizeWidth - u;
        const QString name = p.fontMetrics().elidedText(service.name, Qt::ElideMiddle, qMax(10, int(nameRoom)));
        p.setPen(look.text);
        p.drawText(QRectF(left, y, nameRoom, u * 1.6), Qt::AlignLeft | Qt::AlignVCenter, name);
        const double used = p.fontMetrics().horizontalAdvance(name) + u * 0.8;
        const QString about = service.user ? (service.description.isEmpty() ? QStringLiteral("user") : QStringLiteral("user · ") + service.description)
                                           : service.description;
        if (!about.isEmpty() && nameRoom - used > u * 5) {
            p.setPen(look.dim);
            p.setFont(fontOf(u * 0.9));
            p.drawText(QRectF(left + used, y, nameRoom - used, u * 1.6), Qt::AlignLeft | Qt::AlignVCenter,
                       p.fontMetrics().elidedText(about, Qt::ElideRight, int(nameRoom - used)));
        }
        bar(y + u * 1.75, u * 0.45, service.bytes > 0 ? double(service.bytes) / double(biggest) : 0,
            service.user ? mix(look.accent, look.card, 0.45) : look.accent);
    }
    if (more) {
        p.setPen(look.dim);
        p.setFont(fontOf(u * 0.95));
        text(y, u * 1.6, Qt::AlignLeft, QStringLiteral("and %1 more, each using less").arg(count - shown));
    }
    return image;
}

// ---------------------------------------------- the same, in a terminal ---
// Every service, a line each: the pane scrolls.

QString drawAnsi(const Services &s, int columns)
{
    columns = qBound(24, columns, 200);
    QStringList out;
    out << kBold + QStringLiteral("RUNNING SERVICES") + kReset;
    out << kBold + QString::number(s.list.size()) + kReset + kDim + QStringLiteral(" · ") + countsText(s) + kReset;
    const double share = s.memoryTotal > 0 ? double(s.used) / double(s.memoryTotal) : 0;
    out << blocks(columns, share, levelIndex(share));
    if (usedText(s).size() <= columns) {
        out << kDim + usedText(s) + kReset;
    } else { // a narrow pane: on two lines
        out << kDim + QStringLiteral("Together they use") + kReset;
        out << kDim + (s.memoryTotal > 0 ? QStringLiteral("%1 of %2").arg(formatSize(s.used), formatSize(s.memoryTotal)) : formatSize(s.used)) + kReset;
    }
    out << QString() << kBold + QStringLiteral("BY MEMORY") + kReset;
    if (s.list.isEmpty())
        out << kDim + s.problem.left(columns) + kReset;
    // name, a bar (where there is room for one), the memory
    const int sizeWidth = 9;
    const int barWidth = columns >= 40 ? qBound(6, columns / 5, 24) : 0;
    const int nameWidth = columns - sizeWidth - 1 - (barWidth ? barWidth + 1 : 0);
    const qint64 biggest = s.list.isEmpty() ? 1 : qMax<qint64>(1, s.list.first().bytes);
    for (const Service &service : s.list) {
        // "name (user)": the mark stays when the name is cut.
        const QString mark = service.user ? QStringLiteral(" (user)") : QString();
        const QString name = padded(service.name, qMax(1, qMin(int(service.name.size()), nameWidth - int(mark.size()))));
        QString line = kReset + name + kDim + mark + kReset + QString(qMax(0, nameWidth - int(name.size()) - int(mark.size())), QLatin1Char(' ')) + QLatin1Char(' ');
        if (barWidth)
            line += blocks(barWidth, service.bytes > 0 ? double(service.bytes) / double(biggest) : 0, kAccent) + QLatin1Char(' ');
        out << line + padded(sizeOf(service), sizeWidth, true);
    }
    return out.join(QLatin1Char('\n'));
}

} // namespace

int main(int argc, char **argv)
{
    // Pictures with text need Qt's drawing, but no window is ever shown.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    if (app.arguments().value(1) == QLatin1String("--text")) { // as the terminal interface shows it
        Asker asker([](const Services &) {});
        std::printf("%s\n", qPrintable(drawAnsi(asker.askNow(), app.arguments().value(2, QStringLiteral("44")).toInt())));
        return 0;
    }
    if (app.arguments().value(1) == QLatin1String("--png")) {
        Asker asker([](const Services &) {});
        const QImage image = draw(asker.askNow(), app.arguments().value(3, QStringLiteral("560")).toInt(),
                                  lookFor(app.arguments().value(4) != QLatin1String("light")));
        return save(image, app.arguments().value(2)) ? 0 : 1;
    }

    const QString id = QStringLiteral("services");
    QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (runtime.isEmpty() || !QFileInfo(runtime).isWritable())
        runtime = QDir::tempPath();
    const QString picture = QStringLiteral("%1/argonaut-services-%2.png").arg(runtime).arg(::getpid());
    int width = 560;
    Look look = lookFor(true);
    bool shown = false;      // Argonaut has it on screen
    bool terminal = false;   // it is Argonaut's terminal interface: text, not a picture
    int columns = 44;        // …with room for this many characters
    bool known = false;      // systemd has answered once
    bool asked = false;      // the command was chosen, and its first answer is still to be shown
    Services services;

    auto show = [&](bool update) {
        QJsonObject message{{QStringLiteral("type"), QStringLiteral("show")},
                            {QStringLiteral("id"), id},
                            {QStringLiteral("title"), QStringLiteral("Running Services")},
                            {QStringLiteral("note"), QStringLiteral("Most memory first · every 3 seconds")}};
        if (terminal) {
            message.insert(QStringLiteral("kind"), QStringLiteral("ansi"));
            message.insert(QStringLiteral("text"), drawAnsi(services, columns));
        } else {
            if (!save(draw(services, width, look), picture))
                return;
            message.insert(QStringLiteral("kind"), QStringLiteral("image"));
            message.insert(QStringLiteral("image"), picture);
            message.insert(QStringLiteral("scroll"), true); // longer than the pane: it scrolls
        }
        if (update)
            message.insert(QStringLiteral("update"), true);
        send(message);
    };

    Asker asker([&](const Services &answer) {
        services = answer;
        known = true;
        if (asked) { // the command's answer: this opens the pane
            asked = false;
            show(false);
        } else if (shown) {
            show(true);
        }
    });

    QTimer timer;
    timer.setInterval(3000);
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        if (shown)
            asker.ask();
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
                terminal = message.value(QLatin1String("interface")).toString() == QLatin1String("terminal");
                send({{QStringLiteral("type"), QStringLiteral("commands")},
                      {QStringLiteral("commands"), QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("services")},
                                                                          {QStringLiteral("label"), QStringLiteral("Running Services")},
                                                                          {QStringLiteral("menu"), QStringLiteral("view")}}}}});
            } else if (type == QLatin1String("invoke")) {
                asked = true; // shown when systemd has answered
                asker.ask();
            } else if (type == QLatin1String("result") && message.value(QLatin1String("id")).toString() == id) {
                shown = message.value(QLatin1String("shown")).toBool();
                if (!shown) {
                    timer.stop();
                    continue;
                }
                width = message.value(QLatin1String("width")).toInt();
                if (message.value(QLatin1String("columns")).toInt() > 0)
                    columns = message.value(QLatin1String("columns")).toInt();
                look = lookFor(message.value(QLatin1String("dark")).toBool(), message.value(QLatin1String("colors")).toObject(),
                               message.value(QLatin1String("font")).toString().left(100));
                if (known)
                    show(true); // drawn for this room, in this theme
                timer.start();
            }
        }
    });

    const int code = app.exec();
    QFile::remove(picture);
    return code;
}
