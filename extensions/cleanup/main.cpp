// System Cleanup: an extension that shows, in the preview pane, what can
// safely be removed to free disk space, and how.
//
// Each thing is measured and explained; **nothing is removed by this
// program**. The system's part (packages no longer needed, the downloaded
// package cache, old kernels, old Snap revisions, Docker's unused data, the
// journal, crash reports) needs the administrator, so it is shown as the
// commands for the user to run, under the list. The user's own caches
// (~/.cache) can go to the Trash: the button under the list proposes a
// plan, a checklist that Argonaut checks and carries out with its own file
// engine, so it can be undone. The Trash itself is emptied in Argonaut.
//
// Everything it reads, it reads with the user's own rights: `apt-get -s`
// (a simulation), `dpkg-query`, `snap list`, `docker system df`,
// `journalctl --disk-usage`, `flatpak list`, and the sizes of folders. In
// tests every program is a pretend one (ARGONAUT_*_PROGRAM) and the
// system's folders are under ARGONAUT_CLEANUP_ROOT.
//
//   argonaut-cleanup-extension            the extension (talks on stdin/stdout)
//   argonaut-cleanup-extension --text [COLUMNS]   measures once and prints it as the terminal shows it

#include "../shared/Conversation.h"
#include "../shared/ResultDrawing.h"
#include "Format.h"
#include "Tools.h"
#include "Trash.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QProcess>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QSysInfo>
#include <QThread>
#include <QTimer>
#include <QVersionNumber>

#include <algorithm>
#include <cstdio>
#include <sys/stat.h>

namespace {

void tell(const QString &text)
{
    send({{QStringLiteral("type"), QStringLiteral("message")}, {QStringLiteral("text"), text}});
}

struct Item
{
    QString name;       // "Packages no longer needed"
    bool yours = false; // the user's own (no administrator needed)
    qint64 bytes = 0;
    QString about;      // one line: what it is, where
    QString tip;        // more: what removing it means
    QString command;    // what frees it (empty: done in Argonaut)
    QString cacheName;  // a folder of ~/.cache that the plan may move to the Trash
};

struct Findings
{
    QList<Item> items; // biggest first
    qint64 total = 0;
    qint64 yours = 0;
    qint64 diskSize = 0;
};

QString root(const QString &path) // where a system folder is (tests: under ARGONAUT_CLEANUP_ROOT)
{
    return qEnvironmentVariable("ARGONAUT_CLEANUP_ROOT") + path;
}

QString outputOf(const char *variable, const QString &name, const QStringList &arguments, int limitMs = 10000)
{
    const QString program = Tools::find(variable, name);
    if (program.isEmpty())
        return {};
    QProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    process.setProcessEnvironment(environment);
    process.setStandardInputFile(QProcess::nullDevice()); // never a question answered by accident
    process.start(program, arguments, QIODevice::ReadOnly);
    if (!process.waitForStarted(2000) || !process.waitForFinished(limitMs)) {
        process.kill();
        process.waitForFinished(500);
        return {};
    }
    return QString::fromUtf8(process.readAllStandardOutput().left(4 * 1024 * 1024));
}

// What a folder takes on the disk: its files' blocks, links not followed,
// each file once, nothing outside the folder's own disk.
qint64 sizeOf(const QString &path)
{
    struct stat top {};
    if (::lstat(QFile::encodeName(path).constData(), &top) != 0)
        return 0;
    if (!S_ISDIR(top.st_mode))
        return qint64(top.st_blocks) * 512;
    qint64 bytes = qint64(top.st_blocks) * 512;
    QDirIterator it(path, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        struct stat st {};
        if (::lstat(QFile::encodeName(it.next()).constData(), &st) == 0 && st.st_dev == top.st_dev)
            bytes += qint64(st.st_blocks) * 512;
    }
    return bytes;
}

constexpr qint64 MB = 1000 * 1000;
constexpr qint64 JournalKeep = 200 * MB; // what a vacuum keeps
// A cache listed on its own (ARGONAUT_CLEANUP_SMALLEST in tests, which can't write 100 MB folders).
qint64 smallestCache()
{
    bool given = false;
    const qint64 bytes = qEnvironmentVariable("ARGONAUT_CLEANUP_SMALLEST").toLongLong(&given);
    return given ? bytes : 100 * 1000 * 1000;
}
constexpr int MostCaches = 15;

Findings gather()
{
    Findings out;
    out.diskSize = QStorageInfo(QDir::rootPath()).bytesTotal();

    // Packages apt would remove as no longer needed (old kernels among them).
    {
        static const QRegularExpression removed(QStringLiteral("^Remv (\\S+)"), QRegularExpression::MultilineOption);
        QStringList packages;
        auto found = removed.globalMatch(outputOf("ARGONAUT_APT_GET_PROGRAM", QStringLiteral("apt-get"), {QStringLiteral("-s"), QStringLiteral("autoremove")}));
        while (found.hasNext())
            packages << found.next().captured(1);
        if (!packages.isEmpty()) {
            qint64 kib = 0;
            const QStringList sizes = outputOf("ARGONAUT_DPKG_QUERY_PROGRAM", QStringLiteral("dpkg-query"), QStringList{QStringLiteral("-W"), QStringLiteral("-f=${Installed-Size}\\n")} + packages)
                                          .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            for (const QString &size : sizes)
                kib += size.trimmed().toLongLong();
            const int kernels = int(std::count_if(packages.begin(), packages.end(), [](const QString &p) { return p.startsWith(QLatin1String("linux-")); }));
            Item item;
            item.name = QStringLiteral("Packages no longer needed");
            item.bytes = kib * 1024;
            item.about = QStringLiteral("%1 packages%2: %3").arg(packages.size()).arg(kernels ? QStringLiteral(", %1 of them old kernel parts").arg(kernels) : QString(),
                                                                   packages.mid(0, 4).join(QStringLiteral(", ")) + (packages.size() > 4 ? QStringLiteral("…") : QString()));
            item.tip = QStringLiteral("Installed once as what other programs needed; nothing installed needs them now. "
                                      "apt keeps the kernel in use and the one before it.\n%1")
                           .arg(packages.mid(0, 30).join(QLatin1Char('\n')));
            item.command = QStringLiteral("sudo apt autoremove");
            out.items << item;
        }
    }
    // Kernels apt keeps because they were installed by hand: older than the one running and the newest other one.
    {
        const QString running = qEnvironmentVariable("ARGONAUT_KERNEL_RELEASE", QSysInfo::kernelVersion());
        static const QRegularExpression image(QStringLiteral("^linux-image-(\\d+\\.\\d+\\.\\d+-\\d+)-(\\S+) install ok installed$"), QRegularExpression::MultilineOption);
        const QString listed = outputOf("ARGONAUT_DPKG_QUERY_PROGRAM", QStringLiteral("dpkg-query"), {QStringLiteral("-W"), QStringLiteral("-f=${Package} ${Status}\\n"), QStringLiteral("linux-image-[0-9]*")});
        QStringList versions;
        auto found = image.globalMatch(listed);
        while (found.hasNext()) {
            const auto one = found.next();
            versions << one.captured(1) + QLatin1Char('-') + one.captured(2);
        }
        std::sort(versions.begin(), versions.end(), [](const QString &a, const QString &b) {
            return QVersionNumber::fromString(a.section(QLatin1Char('-'), 0, 1).replace(QLatin1Char('-'), QLatin1Char('.')))
                   < QVersionNumber::fromString(b.section(QLatin1Char('-'), 0, 1).replace(QLatin1Char('-'), QLatin1Char('.')));
        });
        QStringList kept{running};
        for (auto it = versions.crbegin(); it != versions.crend(); ++it) {
            if (*it != running) {
                kept << *it; // the newest other one stays, as a way back
                break;
            }
        }
        for (const QString &version : std::as_const(versions)) {
            if (kept.contains(version) || QVersionNumber::fromString(version.section(QLatin1Char('-'), 0, 0)) > QVersionNumber::fromString(running.section(QLatin1Char('-'), 0, 0)))
                continue;
            const QString number = version.section(QLatin1Char('-'), 0, 1);
            const QStringList packages = outputOf("ARGONAUT_DPKG_QUERY_PROGRAM", QStringLiteral("dpkg-query"),
                                                  {QStringLiteral("-W"), QStringLiteral("-f=${Package} ${Status}\\n"), QStringLiteral("linux-*%1*").arg(number)})
                                             .split(QLatin1Char('\n'), Qt::SkipEmptyParts)
                                             .filter(QStringLiteral(" install ok installed"));
            QStringList names;
            for (const QString &row : packages)
                names << row.section(QLatin1Char(' '), 0, 0);
            Item item;
            item.name = QStringLiteral("Old kernel %1").arg(version);
            item.bytes = sizeOf(root(QStringLiteral("/lib/modules/") + version)) + sizeOf(root(QStringLiteral("/usr/lib/modules/") + version))
                         + sizeOf(root(QStringLiteral("/boot/vmlinuz-") + version)) + sizeOf(root(QStringLiteral("/boot/initrd.img-") + version));
            item.about = QStringLiteral("Running: %1. This one is older than the one kept as a way back").arg(running);
            item.tip = QStringLiteral("Kernels apt doesn't remove by itself because they were installed by hand. The one running (%1) and the newest other one (%2) stay, "
                                      "so there is always one to start from if the newest has a problem.\n%3")
                           .arg(running, kept.value(1, QStringLiteral("none")), names.join(QLatin1Char('\n')));
            item.command = names.isEmpty() ? QString() : QStringLiteral("sudo apt purge %1").arg(names.join(QLatin1Char(' ')));
            if (!item.command.isEmpty())
                out.items << item;
        }
    }
    // The downloaded package files apt keeps.
    {
        qint64 bytes = 0;
        int count = 0;
        const QFileInfoList debs = QDir(root(QStringLiteral("/var/cache/apt/archives"))).entryInfoList({QStringLiteral("*.deb")}, QDir::Files);
        for (const QFileInfo &deb : debs) {
            bytes += sizeOf(deb.filePath());
            ++count;
        }
        if (count > 0) {
            Item item;
            item.name = QStringLiteral("Downloaded package files");
            item.bytes = bytes;
            item.about = QStringLiteral("%1 package files apt downloaded to install or update").arg(count);
            item.tip = QStringLiteral("Copies of packages already installed (in /var/cache/apt/archives). They are downloaded again if they are ever needed.");
            item.command = QStringLiteral("sudo apt clean");
            out.items << item;
        }
    }
    // Snap keeps the revision before the current one of each snap.
    {
        const QStringList rows = outputOf("ARGONAUT_SNAP_PROGRAM", QStringLiteral("snap"), {QStringLiteral("list"), QStringLiteral("--all")}).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        static const QRegularExpression safe(QStringLiteral("^[a-z0-9][a-z0-9_-]{0,63}$"));
        QStringList commands, names;
        qint64 bytes = 0;
        for (const QString &row : rows.mid(1)) {
            const QStringList fields = row.simplified().split(QLatin1Char(' '));
            if (fields.size() < 4 || !fields.last().contains(QLatin1String("disabled")) || !safe.match(fields.at(0)).hasMatch() || !fields.at(2).toInt())
                continue;
            names << QStringLiteral("%1 (revision %2)").arg(fields.at(0), fields.at(2));
            commands << QStringLiteral("sudo snap remove %1 --revision=%2").arg(fields.at(0), fields.at(2));
            bytes += sizeOf(root(QStringLiteral("/var/lib/snapd/snaps/%1_%2.snap").arg(fields.at(0), fields.at(2))));
        }
        if (!commands.isEmpty()) {
            Item item;
            item.name = QStringLiteral("Old Snap revisions");
            item.bytes = bytes;
            item.about = names.mid(0, 4).join(QStringLiteral(", ")) + (names.size() > 4 ? QStringLiteral("…") : QString());
            item.tip = QStringLiteral("Snap keeps the version before the current one of each snap, to go back to. Removing it means there is no going back "
                                      "until the next update.\n%1").arg(names.join(QLatin1Char('\n')));
            item.command = commands.join(QLatin1Char('\n'));
            out.items << item;
        }
    }
    // Docker: each kind on its own, since each has its own command and its own risk. Volumes hold data: never offered.
    {
        const QStringList rows = outputOf("ARGONAUT_DOCKER_PROGRAM", QStringLiteral("docker"), {QStringLiteral("system"), QStringLiteral("df"), QStringLiteral("--format"), QStringLiteral("{{.Type}}\t{{.Reclaimable}}")}).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        static const QRegularExpression amount(QStringLiteral("^([0-9.]+)\\s*([kKMGT]?B)"));
        for (const QString &row : rows) {
            const QString type = row.section(QLatin1Char('\t'), 0, 0);
            const auto m = amount.match(row.section(QLatin1Char('\t'), 1).trimmed());
            if (!m.hasMatch())
                continue;
            const QString unit = m.captured(2).toUpper();
            const double factor = unit == QLatin1String("KB") ? 1e3 : unit == QLatin1String("MB") ? 1e6 : unit == QLatin1String("GB") ? 1e9 : unit == QLatin1String("TB") ? 1e12 : 1;
            Item item;
            item.yours = true; // (in the docker group: no password is asked)
            item.bytes = qint64(m.captured(1).toDouble() * factor);
            if (item.bytes < 10 * MB)
                continue;
            if (type == QLatin1String("Images")) {
                item.name = QStringLiteral("Docker images no container uses");
                item.about = QStringLiteral("Downloaded or built images that no container, running or stopped, is made from");
                item.tip = QStringLiteral("They are downloaded or built again when they are needed. An image you built yourself and keep nowhere else is gone for good: "
                                          "look first with docker images.");
                item.command = QStringLiteral("docker image prune -a");
            } else if (type == QLatin1String("Containers")) {
                item.name = QStringLiteral("Stopped Docker containers");
                item.about = QStringLiteral("Containers that have stopped; look at them first with docker ps -a");
                item.tip = QStringLiteral("A stopped container keeps whatever was changed inside it. If one holds work of yours, keep it; the rest can go.");
                item.command = QStringLiteral("docker container prune");
            } else if (type == QLatin1String("Build Cache")) {
                item.name = QStringLiteral("Docker's build cache");
                item.about = QStringLiteral("What docker build keeps to build the same steps again quicker");
                item.tip = QStringLiteral("Builds are slower the first time after it is removed; nothing else is lost.");
                item.command = QStringLiteral("docker builder prune");
            } else {
                continue; // Local Volumes and anything new: they can hold data
            }
            out.items << item;
        }
    }
    // The journal beyond what a vacuum keeps.
    {
        static const QRegularExpression size(QStringLiteral("take up ([0-9.]+)([KMGT])"));
        const auto m = size.match(outputOf("ARGONAUT_JOURNALCTL_PROGRAM", QStringLiteral("journalctl"), {QStringLiteral("--disk-usage")}));
        if (m.hasMatch()) {
            const QChar unit = m.captured(2).at(0);
            const double factor = unit == QLatin1Char('K') ? 1024.0 : unit == QLatin1Char('M') ? 1024.0 * 1024 : unit == QLatin1Char('G') ? 1024.0 * 1024 * 1024 : 1024.0 * 1024 * 1024 * 1024;
            const qint64 bytes = qint64(m.captured(1).toDouble() * factor);
            if (bytes > JournalKeep + 50 * MB) {
                Item item;
                item.name = QStringLiteral("The journal's older part");
                item.bytes = bytes - JournalKeep;
                item.about = QStringLiteral("The system's log takes %1; keeping the newest %2 is plenty").arg(formatSize(bytes), formatSize(JournalKeep));
                item.tip = QStringLiteral("The journal is what System Journal reads. Only its oldest messages go; the newest 200 MB stay.");
                item.command = QStringLiteral("sudo journalctl --vacuum-size=200M");
                out.items << item;
            }
        }
    }
    // Crash reports.
    {
        qint64 bytes = 0;
        QStringList files;
        const QFileInfoList crashes = QDir(root(QStringLiteral("/var/crash"))).entryInfoList({QStringLiteral("*.crash")}, QDir::Files);
        for (const QFileInfo &crash : crashes) {
            bytes += sizeOf(crash.filePath());
            files << crash.fileName();
        }
        if (!files.isEmpty()) {
            Item item;
            item.name = QStringLiteral("Crash reports");
            item.bytes = bytes;
            item.about = QStringLiteral("%1 reports of programs that crashed").arg(files.size());
            item.tip = QStringLiteral("Written when a program crashed, to be sent to its makers. Once looked at or sent, they are only space.\n%1").arg(files.mid(0, 20).join(QLatin1Char('\n')));
            item.command = QStringLiteral("sudo rm /var/crash/*.crash");
            out.items << item;
        }
    }
    // Flatpak: runtimes nothing uses any more (flatpak lists them itself and asks before it removes them).
    if (!outputOf("ARGONAUT_FLATPAK_PROGRAM", QStringLiteral("flatpak"), {QStringLiteral("list"), QStringLiteral("--runtime"), QStringLiteral("--columns=ref")}).trimmed().isEmpty()) {
        Item item;
        item.name = QStringLiteral("Unused Flatpak runtimes");
        item.bytes = 0;
        item.about = QStringLiteral("Flatpak can't be asked their size without removing them: its command lists them and asks first");
        item.tip = QStringLiteral("Runtimes are the shared parts Flatpak programs run on; an old one stays when the program that needed it was removed or updated.");
        item.command = QStringLiteral("flatpak uninstall --unused");
        out.items << item;
    }
    // The user's own: the Trash, then the caches in ~/.cache.
    {
        qint64 trash = 0;
        int count = 0;
        const QStringList folders = Trash::filesDirs();
        for (const QString &folder : folders) {
            const QFileInfoList entries = QDir(folder).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
            for (const QFileInfo &entry : entries) {
                trash += sizeOf(entry.filePath());
                ++count;
            }
        }
        if (count > 0) {
            Item item;
            item.name = QStringLiteral("The Trash");
            item.yours = true;
            item.bytes = trash;
            item.about = QStringLiteral("%1 items you moved to the Trash").arg(count);
            item.tip = QStringLiteral("What is in the Trash still takes its space until the Trash is emptied. Empty it in Argonaut: the Trash in the sidebar → Empty Trash. That is for good.");
            out.items << item;
        }
    }
    {
        const QString cache = QDir::homePath() + QStringLiteral("/.cache");
        QList<Item> caches;
        const QFileInfoList entries = QDir(cache).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::NoSymLinks);
        for (const QFileInfo &entry : entries) {
            const qint64 bytes = sizeOf(entry.filePath());
            if (bytes < smallestCache())
                continue;
            Item item;
            item.yours = true;
            item.name = entry.fileName() == QLatin1String("thumbnails") ? QStringLiteral("Thumbnails") : QStringLiteral("Cache: %1").arg(entry.fileName());
            item.bytes = bytes;
            item.cacheName = entry.fileName();
            item.about = entry.fileName() == QLatin1String("thumbnails")
                             ? QStringLiteral("Small pictures of your files, made again when a folder is shown")
                             : QStringLiteral("~/.cache/%1: kept by a program to be quicker; it makes it again").arg(entry.fileName());
            item.tip = QStringLiteral("~/.cache is for what programs can make again, so taking a folder away is safe; the program is slower the first time, "
                                      "and what it had downloaded (models, packages) is downloaded again. Close the program first. "
                                      "The button under the list moves the caches you tick to the Trash; their space returns when the Trash is emptied.");
            caches << item;
        }
        std::sort(caches.begin(), caches.end(), [](const Item &a, const Item &b) { return a.bytes > b.bytes; });
        out.items << caches.mid(0, MostCaches);
    }
    std::stable_sort(out.items.begin(), out.items.end(), [](const Item &a, const Item &b) { return a.bytes > b.bytes; });
    for (const Item &item : std::as_const(out.items)) {
        out.total += item.bytes;
        if (item.yours)
            out.yours += item.bytes;
    }
    return out;
}

// ------------------------------------------------- what is drawn ---

ListHead headOf(const Findings &f)
{
    ListHead head;
    head.title = QStringLiteral("SYSTEM CLEANUP");
    head.big = formatSize(f.total);
    head.beside = QStringLiteral("can be freed · %1 yours · %2 the system's").arg(formatSize(f.yours), formatSize(f.total - f.yours));
    head.share = f.diskSize > 0 ? double(f.total) / double(f.diskSize) : 0;
    head.line = f.diskSize > 0 ? QStringLiteral("Of the system disk's %1.\nNothing here is removed by Argonaut on its own").arg(formatSize(f.diskSize)) : QString();
    head.listTitle = QStringLiteral("BIGGEST FIRST");
    head.empty = QStringLiteral("Nothing worth removing was found");
    return head;
}

QList<ListRow> rowsOf(const Findings &f)
{
    QList<ListRow> rows;
    const qint64 biggest = f.items.isEmpty() ? 1 : qMax<qint64>(1, f.items.first().bytes);
    for (const Item &item : f.items) {
        ListRow row;
        row.name = item.name;
        row.mark = item.yours ? QStringLiteral("yours") : QStringLiteral("needs the administrator");
        row.about = item.about;
        row.size = item.bytes > 0 ? formatSize(item.bytes) : QStringLiteral("—");
        row.share = double(item.bytes) / double(biggest);
        row.faint = !item.yours;
        row.tip = item.tip + (item.command.isEmpty() ? QString() : QStringLiteral("\n\nTo free it:\n") + item.command);
        rows << row;
    }
    return rows;
}

// The commands for the system's part, to read and copy.
QString commandsText(const Findings &f, bool markdown)
{
    QStringList blocks;
    for (const Item &item : f.items) {
        if (item.command.isEmpty())
            continue;
        const QString size = item.bytes > 0 ? QStringLiteral(" (%1)").arg(formatSize(item.bytes)) : QString();
        blocks << (markdown ? QStringLiteral("**%1**%2\n\n```\n%3\n```").arg(item.name, size, item.command)
                            : QStringLiteral("%1%2:\n  %3").arg(item.name, size, QString(item.command).replace(QLatin1Char('\n'), QStringLiteral("\n  "))));
    }
    const QString intro = QStringLiteral("Argonaut doesn't run these: they change the system or remove things for good, so read each one and run it yourself in a terminal "
                                         "(those starting with sudo ask for your password).");
    const QString yours = QStringLiteral("Your caches: the button under the list (in the terminal: Enter) shows them as a checklist; the ones you keep ticked go to the Trash "
                                         "and Ctrl+Z brings them back. Their space, and the Trash's, returns when the Trash is emptied.");
    if (markdown)
        return QStringLiteral("### Freeing the rest\n\n%1\n\n%2\n\n%3").arg(intro, blocks.join(QStringLiteral("\n\n")), yours);
    return QStringLiteral("FREEING THE REST\n%1\n\n%2\n\n%3").arg(intro, blocks.join(QStringLiteral("\n\n")), yours);
}

// A line of text cut at spaces to `columns`; a command is cut the same way
// with " \" at the end of each part, so what is copied still runs.
QStringList wrapped(const QString &line, int columns, bool command)
{
    const QString indent = command ? QStringLiteral("    ") : QStringLiteral("  ");
    QStringList out;
    QString rest = line;
    const int room = qMax(20, columns - (command ? 2 : 0));
    while (rest.size() > room) {
        qsizetype cut = rest.lastIndexOf(QLatin1Char(' '), room);
        if (cut <= indent.size())
            cut = rest.indexOf(QLatin1Char(' '), room); // a word longer than the line: kept whole
        if (cut <= 0)
            break;
        out << rest.left(cut) + (command ? QStringLiteral(" \\") : QString());
        rest = indent + rest.mid(cut + 1);
    }
    out << rest;
    return out;
}

QString drawAnsi(const Findings &f, int columns)
{
    ListHead head = headOf(f);
    QList<ListRow> rows = rowsOf(f);
    narrowList(&head, &rows, {}, int(rows.size()));
    QString text = drawListAnsi(head, rows, columns) + QStringLiteral("\n\n");
    for (const QString &raw : commandsText(f, false).split(QLatin1Char('\n'))) {
        const QString line = plainText(raw);
        const bool command = line.startsWith(QLatin1String("  "));
        for (const QString &part : wrapped(line, columns, command))
            text += (command ? part : kDim + part + kReset) + QLatin1Char('\n');
    }
    return text;
}

QJsonObject planFor(const Findings &f, const QString &id)
{
    QJsonArray moves;
    qint64 bytes = 0;
    for (const Item &item : f.items) {
        if (item.cacheName.isEmpty())
            continue;
        bytes += item.bytes;
        moves.append(QJsonObject{{QStringLiteral("action"), QStringLiteral("trash")},
                                 {QStringLiteral("item"), item.cacheName},
                                 {QStringLiteral("reason"), QStringLiteral("%1 · %2").arg(formatSize(item.bytes), item.about)}});
    }
    return {{QStringLiteral("type"), QStringLiteral("plan")},
            {QStringLiteral("id"), id},
            {QStringLiteral("title"), QStringLiteral("Your caches to the Trash")},
            {QStringLiteral("folder"), QDir::homePath() + QStringLiteral("/.cache")},
            {QStringLiteral("report"),
             QStringLiteral("## Your programs' caches\n\nThese folders of `~/.cache` together take **%1**. Programs make them again, so taking them away is safe, "
                            "but each program is slower the first time, and what it had downloaded is downloaded again. **Close the programs first.**\n\n"
                            "Untick what you want to keep. What stays ticked goes to the **Trash** (Ctrl+Z brings it back); the space returns when the Trash is emptied.")
                 .arg(formatSize(bytes))},
            {QStringLiteral("note"), QStringLiteral("Only folders of ~/.cache · nothing is deleted for good")},
            {QStringLiteral("moves"), moves}};
}

} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen"); // pictures with text need Qt's drawing, but no window is ever shown
    QGuiApplication app(argc, argv);
    if (app.arguments().value(1) == QLatin1String("--text")) {
        std::printf("%s\n", qPrintable(drawAnsi(gather(), app.arguments().value(2, QStringLiteral("70")).toInt())));
        return 0;
    }

    const QString id = QStringLiteral("cleanup");
    const QString picture = pictureFile("cleanup");
    Room room;
    bool onScreen = false, terminal = false, measuring = false, measureAgain = false;
    Findings found;
    bool have = false;
    int plans = 0;

    auto show = [&](bool update) {
        QJsonObject message{{QStringLiteral("type"), QStringLiteral("show")},
                            {QStringLiteral("id"), id},
                            {QStringLiteral("title"), QStringLiteral("System Cleanup")},
                            {QStringLiteral("note"), have ? QStringLiteral("Measured now · nothing is removed without you") : QStringLiteral("Measuring…")}};
        if (have && std::any_of(found.items.begin(), found.items.end(), [](const Item &i) { return !i.cacheName.isEmpty(); }))
            message.insert(QStringLiteral("action"), QJsonObject{{QStringLiteral("id"), QStringLiteral("caches")}, {QStringLiteral("label"), QStringLiteral("Move My Caches to the Trash…")}});
        if (!have) {
            message.insert(QStringLiteral("kind"), QStringLiteral("text"));
            message.insert(QStringLiteral("text"), QStringLiteral("Measuring what can be freed… (the caches can take a little while)"));
        } else if (terminal) {
            message.insert(QStringLiteral("kind"), QStringLiteral("ansi"));
            message.insert(QStringLiteral("text"), drawAnsi(found, room.columns));
        } else {
            ListHead head = headOf(found);
            QList<ListRow> rows = rowsOf(found);
            narrowList(&head, &rows, {}, 250);
            QList<TipArea> tips;
            if (!save(drawList(head, rows, room.width, room.look, &tips), picture))
                return;
            message.insert(QStringLiteral("kind"), QStringLiteral("image"));
            message.insert(QStringLiteral("image"), picture);
            message.insert(QStringLiteral("tips"), tipsJson(tips));
            message.insert(QStringLiteral("scroll"), true);
        }
        if (update)
            message.insert(QStringLiteral("update"), true);
        send(message);
        // The commands to copy, under the list (the terminal has them in the text).
        if (have && onScreen && !terminal)
            send({{QStringLiteral("type"), QStringLiteral("show")},
                  {QStringLiteral("id"), QStringLiteral("cleanup-commands")},
                  {QStringLiteral("under"), id},
                  {QStringLiteral("title"), QStringLiteral("Freeing the rest")},
                  {QStringLiteral("kind"), QStringLiteral("markdown")},
                  {QStringLiteral("text"), commandsText(found, true)}});
    };

    std::function<void()> measure = [&] {
        if (measuring) {
            measureAgain = true;
            return;
        }
        measuring = true;
        QThread *worker = QThread::create([&found, &have, &measuring, &measureAgain, &show, &measure] {
            Findings result = gather();
            QMetaObject::invokeMethod(qApp, [result, &found, &have, &measuring, &measureAgain, &show, &measure] {
                found = result;
                have = true;
                measuring = false;
                show(true);
                if (measureAgain) {
                    measureAgain = false;
                    measure();
                }
            });
        });
        QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);
        worker->start();
    };

    Conversation conversation([&](const QJsonObject &message) {
        const QString type = message.value(QLatin1String("type")).toString();
        if (type == QLatin1String("hello")) {
            terminal = message.value(QLatin1String("interface")).toString() == QLatin1String("terminal");
            offerViewCommand(QStringLiteral("cleanup"), QStringLiteral("System Cleanup"));
        } else if (type == QLatin1String("invoke")) {
            have = false;
            show(false);
            measure();
        } else if (type == QLatin1String("result") && message.value(QLatin1String("id")).toString() == id) {
            onScreen = message.value(QLatin1String("shown")).toBool();
            if (onScreen) {
                room.read(message);
                show(true);
            }
        } else if (type == QLatin1String("action") && message.value(QLatin1String("id")).toString() == id
                   && message.value(QLatin1String("action")).toString() == QLatin1String("caches")) {
            if (!have)
                return;
            send(planFor(found, QStringLiteral("caches-%1").arg(++plans))); // Argonaut checks it, shows it as a checklist and does what stays ticked
        } else if (type == QLatin1String("plan")) {
            if (message.value(QLatin1String("applied")).toInt() > 0) {
                tell(QStringLiteral("System Cleanup: the caches are in the Trash; their space returns when the Trash is emptied"));
                QTimer::singleShot(1500, qApp, [&] { measure(); }); // after Argonaut's job has moved them
            }
        }
    });

    const int code = app.exec();
    QFile::remove(picture);
    return code;
}
