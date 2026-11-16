// Running Processes: an extension that shows, in the preview pane, every
// process that is running now, the one using the most memory first.
//
// The processes are read from /proc (ARGONAUT_PROC_ROOT in tests): each
// one's name, whose it is, its number and its memory. Memory is what the
// process has in RAM now (VmRSS); pages several processes share are counted
// for each of them, so the rows don't add up to the memory in use. The
// kernel's own threads have no memory of their own and aren't listed.
// Nothing is stopped or changed: it only shows.
//
// It offers one command, "Running Processes", in the View menu (and so in
// the command palette). The answer is a named result ("show" with an "id")
// that is made again every three seconds while it is on screen: a picture
// in the theme's colours, as long as the list is (Argonaut scrolls it), or,
// in the terminal interface, text with colour codes.
//
//   argonaut-processes-extension             the extension (talks on stdin/stdout)
//   argonaut-processes-extension --png FILE [WIDTH [dark|light]]
//                                            draws once into FILE (to look at it)
//   argonaut-processes-extension --text [COLUMNS]
//                                            prints it once as the terminal interface shows it

#include "../shared/ResultDrawing.h"
#include "Format.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QJsonArray>
#include <QSocketNotifier>
#include <QTimer>

#include <algorithm>
#include <pwd.h>
#include <unistd.h>

namespace {

struct Process
{
    QString name;
    QString command; // how it was started, where that says more than the name
    qint64 pid = 0;
    qint64 bytes = 0;
    uint uid = 0;
    bool mine = false;
};

struct Processes
{
    QList<Process> list; // most memory first
    int mine = 0, others = 0;
    qint64 memoryUsed = 0, memoryTotal = 0;
    QString problem;     // why there is no list
};

QString procRoot()
{
    const QString other = qEnvironmentVariable("ARGONAUT_PROC_ROOT");
    return other.isEmpty() ? QStringLiteral("/proc") : other;
}

QByteArray contentsOf(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.read(64 * 1024) : QByteArray();
}

// "Key:   value" in a /proc file.
QByteArray field(const QByteArray &text, const char *key)
{
    qsizetype at = text.startsWith(key) ? 0 : text.indexOf(QByteArray("\n") + key);
    if (at < 0)
        return {};
    at += (at == 0 ? 0 : 1) + qsizetype(qstrlen(key));
    qsizetype end = text.indexOf('\n', at);
    return text.mid(at, end < 0 ? -1 : end - at).simplified();
}

QString userName(uint uid)
{
    static QHash<uint, QString> names;
    auto found = names.constFind(uid);
    if (found != names.constEnd())
        return *found;
    const struct passwd *entry = ::getpwuid(uid);
    const QString name = entry ? QString::fromLocal8Bit(entry->pw_name) : QString::number(uid);
    names.insert(uid, name);
    return name;
}

Processes readProcesses()
{
    Processes all;
    const QString root = procRoot();
    const uint me = ::getuid();
    const QStringList entries = QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &entry : entries) {
        bool number = false;
        const qint64 pid = entry.toLongLong(&number);
        if (!number || pid <= 0)
            continue;
        const QByteArray status = contentsOf(root + QLatin1Char('/') + entry + QStringLiteral("/status"));
        const QByteArray memory = field(status, "VmRSS:"); // "1234 kB"; the kernel's threads have none
        if (status.isEmpty() || memory.isEmpty())
            continue;
        Process process;
        process.pid = pid;
        process.bytes = memory.split(' ').value(0).toLongLong() * 1024;
        process.uid = field(status, "Uid:").split(' ').value(0).toUInt();
        process.mine = process.uid == me;
        process.name = QString::fromUtf8(field(status, "Name:")).left(200);
        // The name is cut at 15 characters by the kernel: the program's own
        // name, from how it was started, where that is the longer one.
        const QList<QByteArray> started = contentsOf(root + QLatin1Char('/') + entry + QStringLiteral("/cmdline")).split('\0');
        const QString program = QString::fromUtf8(started.value(0));
        const QString base = program.section(QLatin1Char('/'), -1).left(200);
        if (process.name.size() >= 15 && base.startsWith(process.name))
            process.name = base;
        QStringList words;
        for (const QByteArray &word : started) {
            if (!word.isEmpty())
                words << QString::fromUtf8(word);
        }
        process.command = words.join(QLatin1Char(' ')).simplified().left(300);
        if (process.name.isEmpty())
            process.name = base.isEmpty() ? entry : base;
        (process.mine ? all.mine : all.others) += 1;
        all.list.append(process);
    }
    std::stable_sort(all.list.begin(), all.list.end(), [](const Process &a, const Process &b) {
        return a.bytes != b.bytes ? a.bytes > b.bytes : a.pid < b.pid;
    });
    const QByteArray meminfo = contentsOf(root + QStringLiteral("/meminfo"));
    all.memoryTotal = field(meminfo, "MemTotal:").split(' ').value(0).toLongLong() * 1024;
    all.memoryUsed = qMax<qint64>(0, all.memoryTotal - field(meminfo, "MemAvailable:").split(' ').value(0).toLongLong() * 1024);
    if (all.list.isEmpty())
        all.problem = QStringLiteral("No processes could be read from %1").arg(root);
    return all;
}

// ------------------------------------------------- what is drawn ---
// The drawing itself is shared (ResultDrawing.h: drawList, drawListAnsi).

ListHead headOf(const Processes &p)
{
    ListHead head;
    head.title = QStringLiteral("RUNNING PROCESSES");
    head.big = QString::number(p.list.size());
    head.beside = QStringLiteral("%1 yours · %2 others").arg(p.mine).arg(p.others);
    head.share = p.memoryTotal > 0 ? double(p.memoryUsed) / double(p.memoryTotal) : 0;
    head.line = p.memoryTotal > 0 ? QStringLiteral("Memory in use:\n%1 of %2").arg(formatSize(p.memoryUsed), formatSize(p.memoryTotal)) : QString();
    head.listTitle = QStringLiteral("BY MEMORY · NAME, PROCESS ID, MEMORY");
    head.empty = p.problem;
    return head;
}

// The processes as rows; a bar is against the biggest. As
// text a row is one line with little room: the number is with the name
// there, so that five "chrome" can be told apart.
QList<ListRow> rowsOf(const Processes &p, bool asText)
{
    QList<ListRow> rows;
    const qint64 biggest = p.list.isEmpty() ? 1 : qMax<qint64>(1, p.list.first().bytes);
    for (const Process &process : p.list) {
        ListRow row;
        row.name = process.name;
        row.mark = process.mine ? QString() : userName(process.uid); // whose it is, when it isn't yours
        row.about = process.command;
        // The process's number: always there (a column of its own in the
        // picture, with the name in a line of text) and searched like the name.
        if (asText)
            row.mark = row.mark.isEmpty() ? QString::number(process.pid) : row.mark + QLatin1Char(' ') + QString::number(process.pid);
        else
            row.tag = QString::number(process.pid);
        row.size = formatSize(process.bytes);
        row.share = double(process.bytes) / double(biggest);
        row.faint = !process.mine;
        rows.append(row);
    }
    return rows;
}

// A picture has a limit to its height; text has none.
QImage draw(const Processes &p, int width, const Look &look, const QString &search = {})
{
    ListHead head = headOf(p);
    QList<ListRow> rows = rowsOf(p, false);
    narrowList(&head, &rows, search, 250);
    return drawList(head, rows, width, look);
}

QString drawAnsi(const Processes &p, int columns, const QString &search = {})
{
    ListHead head = headOf(p);
    QList<ListRow> rows = rowsOf(p, true);
    narrowList(&head, &rows, search, int(rows.size()));
    return drawListAnsi(head, rows, columns);
}

} // namespace

int main(int argc, char **argv)
{
    // Pictures with text need Qt's drawing, but no window is ever shown.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    if (app.arguments().value(1) == QLatin1String("--text")) { // as the terminal interface shows it
        std::printf("%s\n", qPrintable(drawAnsi(readProcesses(), app.arguments().value(2, QStringLiteral("44")).toInt())));
        return 0;
    }
    if (app.arguments().value(1) == QLatin1String("--png")) {
        const QImage image = draw(readProcesses(), app.arguments().value(3, QStringLiteral("560")).toInt(),
                                  lookFor(app.arguments().value(4) != QLatin1String("light")));
        return save(image, app.arguments().value(2)) ? 0 : 1;
    }

    const QString id = QStringLiteral("processes");
    const QString picture = pictureFile("processes");
    Room room;             // what Argonaut says there is room for, and how the window looks
    bool shown = false;    // Argonaut has it on screen
    bool terminal = false; // it is Argonaut's terminal interface: text, not a picture
    QString search;        // only the processes with this in them

    auto show = [&](bool update) {
        const Processes processes = readProcesses();
        QJsonObject message{{QStringLiteral("type"), QStringLiteral("show")},
                            {QStringLiteral("id"), id},
                            {QStringLiteral("title"), QStringLiteral("Running Processes")},
                            {QStringLiteral("note"), QStringLiteral("Most memory first · every 3 seconds")},
                            {QStringLiteral("search"), QStringLiteral("Search processes")}}; // Argonaut shows a field; what is typed comes as "search"
        if (terminal) {
            message.insert(QStringLiteral("kind"), QStringLiteral("ansi"));
            message.insert(QStringLiteral("text"), drawAnsi(processes, room.columns, search));
        } else {
            if (!save(draw(processes, room.width, room.look, search), picture))
                return;
            message.insert(QStringLiteral("kind"), QStringLiteral("image"));
            message.insert(QStringLiteral("image"), picture);
            message.insert(QStringLiteral("scroll"), true); // longer than the pane: it scrolls
        }
        if (update)
            message.insert(QStringLiteral("update"), true);
        send(message);
    };

    QTimer timer;
    timer.setInterval(3000);
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
            offerViewCommand(QStringLiteral("processes"), QStringLiteral("Running Processes"));
        } else if (type == QLatin1String("invoke")) {
            show(false); // Argonaut answers with the room there is
        } else if (type == QLatin1String("search") && message.value(QLatin1String("id")).toString() == id) {
            search = message.value(QLatin1String("text")).toString().left(200);
            show(true);
        } else if (type == QLatin1String("result") && message.value(QLatin1String("id")).toString() == id) {
            shown = message.value(QLatin1String("shown")).toBool();
            if (!shown) {
                timer.stop();
                search.clear(); // the field went with the result
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
