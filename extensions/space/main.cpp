// Space Overview: an extension that shows, in the preview pane, what takes
// the room in the open folder: how big it is, how full its disk is, and the
// biggest folders and files directly inside it, each with a bar.
//
// It only shows. Working in the tree (opening folders, deleting, sizes that
// follow what you do) is Argonaut's own Analyze view; this is the quick
// look, and the one the terminal interface has.
//
// It offers one command, "Space Overview", in the View menu (and so in the
// command palette). The folder is measured with the same code as Analyze
// (DiskUsageModel: space used on disk, as `du` counts it), in the
// background; the result is a named result ("show" with an "id") that fills
// in while the measuring goes on. It is a picture drawn in the theme's
// colours, or text with colour codes in the terminal interface.
//
//   argonaut-space-extension                 the extension (talks on stdin/stdout)
//   argonaut-space-extension --png FILE FOLDER [WIDTH HEIGHT [dark|light]]
//                                            measures FOLDER and draws once into FILE
//   argonaut-space-extension --text FOLDER [COLUMNS]
//                                            prints it once as the terminal interface shows it

#include "../shared/ResultDrawing.h"
#include "DiskUsageModel.h"
#include "Format.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QSocketNotifier>
#include <QStorageInfo>
#include <QTime>

#include <algorithm>
#include <unistd.h>

namespace {

struct Row
{
    QString name;
    qint64 bytes = 0;
    bool smaller = false; // the "N smaller files" row
};

struct Overview
{
    QString folder;
    bool measuring = false;
    bool incomplete = false; // some folders inside couldn't be read
    qint64 bytes = 0, items = 0;
    QList<Row> rows; // biggest first
    qint64 diskUsed = 0, diskTotal = 0;
    QString diskName;
};

// What the model knows now: complete, or as far as the measuring has got.
Overview read(const DiskUsageModel &model)
{
    Overview o;
    o.folder = model.rootPath();
    o.measuring = model.isScanning();
    o.incomplete = !o.measuring && model.rootIncomplete();
    const QModelIndex root = model.index(0, 0);
    qint64 sum = 0;
    if (root.isValid()) {
        const int count = model.rowCount(root);
        o.rows.reserve(count);
        for (int i = 0; i < count; ++i) {
            const QModelIndex index = model.index(i, 0, root);
            Row row;
            const int kind = index.data(DiskUsageModel::KindRole).toInt();
            row.smaller = kind == DiskUsageModel::Node::SmallerFiles;
            row.name = index.data(Qt::DisplayRole).toString() + (kind == DiskUsageModel::Node::Folder ? QStringLiteral("/") : QString());
            row.bytes = index.data(DiskUsageModel::BytesRole).toLongLong();
            sum += row.bytes;
            o.rows.append(row);
        }
    }
    std::stable_sort(o.rows.begin(), o.rows.end(), [](const Row &a, const Row &b) {
        return a.smaller != b.smaller ? b.smaller : a.bytes > b.bytes;
    });
    o.bytes = o.measuring ? qMax(sum, model.scannedBytes()) : model.rootBytes();
    o.items = o.measuring ? model.scannedItems() : model.rootItems();
    const QStorageInfo disk(o.folder);
    if (disk.isValid() && disk.bytesTotal() > 0) {
        o.diskTotal = disk.bytesTotal();
        o.diskUsed = disk.bytesTotal() - disk.bytesFree();
        o.diskName = disk.rootPath();
    }
    return o;
}

QString itemsText(const Overview &o)
{
    return o.items == 1 ? QStringLiteral("1 item") : QStringLiteral("%1 items").arg(QLocale().toString(o.items));
}

QString diskText(const Overview &o)
{
    return QStringLiteral("Disk %1: %2 of %3 used · %4 free")
        .arg(o.diskName, formatSize(o.diskUsed), formatSize(o.diskTotal), formatSize(o.diskTotal - o.diskUsed));
}

// The rows after the first `shown`: how many, and how big together.
QString restText(const Overview &o, int shown)
{
    qint64 bytes = 0;
    for (int i = shown; i < o.rows.size(); ++i)
        bytes += o.rows.at(i).bytes;
    return QStringLiteral("and %1 more · %2").arg(QLocale().toString(o.rows.size() - shown), formatSize(bytes));
}

QString percent(qint64 part, qint64 whole)
{
    if (whole <= 0)
        return QString();
    const double share = 100.0 * double(part) / double(whole);
    return share > 0 && share < 1 ? QStringLiteral("<1%") : QStringLiteral("%1%").arg(qRound(share));
}

// ------------------------------------------------------------ picture ---

// For a pane `width` pixels wide and at most `height` tall: as many rows as
// fit, and one line for the rest.
QImage draw(const Overview &o, int width, int height, const Look &look)
{
    width = qBound(240, width, 4000);
    height = qBound(240, height, 4000);
    g_font = look.font;
    const double u = qBound(10.0, width / 34.0, 20.0); // the size of ordinary text
    const double rowHeight = u * 2.7;
    const double headHeight = u * (o.diskTotal > 0 ? 9.6 : 5.6);
    const double listTop = headHeight + 2 * u;
    const int fit = qMax(3, int((height - listTop - u * 4.2) / rowHeight));
    const bool more = o.rows.size() > fit;
    const int shown = more ? fit - 1 : int(o.rows.size());
    const double listHeight = u * 3.2 + rowHeight * qMax(1, shown) + (more ? u * 1.9 : 0);

    QImage image(width, int(listTop + listHeight + u + 0.5), QImage::Format_RGB32);
    image.fill(look.window);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    const double left = 2 * u, room = width - 4 * u;
    auto text = [&](double y, double lineHeight, int flags, const QString &string, Qt::TextElideMode elide = Qt::ElideRight) {
        p.drawText(QRectF(left, y, room, lineHeight), flags | Qt::AlignVCenter, p.fontMetrics().elidedText(string, elide, int(room)));
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

    // The folder: its name, its size, and how full its disk is.
    card(p, QRectF(u, u, width - 2 * u, headHeight), look, u);
    p.setPen(look.dim);
    p.setFont(fontOf(u));
    text(u * 1.5, u * 1.5, Qt::AlignLeft, tildePath(o.folder), Qt::ElideMiddle);
    p.setPen(look.text);
    p.setFont(fontOf(u * 2.1, true));
    text(u * 3.1, u * 2.8, Qt::AlignLeft, formatSize(o.bytes));
    p.setPen(look.dim);
    p.setFont(fontOf(u));
    text(u * 3.9, u * 1.8, Qt::AlignRight, o.measuring ? QStringLiteral("Measuring…") : itemsText(o));
    if (o.diskTotal > 0) {
        const double share = double(o.diskUsed) / double(o.diskTotal);
        bar(u * 7.0, u * 0.8, share, levelColor(look, share));
        p.setPen(look.dim);
        p.setFont(fontOf(u * 0.9));
        text(u * 8.3, u * 1.6, Qt::AlignLeft, diskText(o));
    }

    // What is inside, biggest first. A bar is the item's share of the folder.
    card(p, QRectF(u, listTop, width - 2 * u, listHeight), look, u);
    p.setPen(look.text);
    p.setFont(fontOf(u * 1.05));
    text(listTop + u * 0.7, u * 1.6, Qt::AlignLeft, QStringLiteral("BIGGEST INSIDE"));
    double y = listTop + u * 2.8;
    if (o.rows.isEmpty()) {
        p.setPen(look.dim);
        p.setFont(fontOf(u));
        text(y, u * 1.6, Qt::AlignLeft, o.measuring ? QStringLiteral("Nothing measured yet") : QStringLiteral("The folder is empty"));
    }
    for (int i = 0; i < shown; ++i, y += rowHeight) {
        const Row &row = o.rows.at(i);
        p.setFont(fontOf(u));
        const QString size = QStringLiteral("%1   %2").arg(formatSize(row.bytes), percent(row.bytes, o.bytes));
        const int sizeWidth = p.fontMetrics().horizontalAdvance(size);
        p.setPen(look.dim);
        p.drawText(QRectF(left, y, room, u * 1.6), Qt::AlignRight | Qt::AlignVCenter, size);
        p.setPen(row.smaller ? look.dim : look.text);
        p.drawText(QRectF(left, y, room - sizeWidth - u, u * 1.6), Qt::AlignLeft | Qt::AlignVCenter,
                   p.fontMetrics().elidedText(row.name, Qt::ElideMiddle, qMax(10, int(room - sizeWidth - u))));
        bar(y + u * 1.75, u * 0.45, o.bytes > 0 ? double(row.bytes) / double(o.bytes) : 0,
            row.smaller ? mix(look.accent, look.card, 0.45) : look.accent);
    }
    if (more) {
        p.setPen(look.dim);
        p.setFont(fontOf(u * 0.95));
        text(y, u * 1.6, Qt::AlignLeft, restText(o, shown));
    }
    return image;
}

// ---------------------------------------------- the same, in a terminal ---

QString padded(const QString &text, int width, bool right = false)
{
    const QString cut = text.size() > width ? text.left(qMax(0, width - 1)) + QChar(0x2026) : text;
    const QString space(width - int(cut.size()), QLatin1Char(' '));
    return right ? space + cut : cut + space;
}

QString blocks(int width, double share, int color)
{
    const int filled = qBound(0, int(width * share + 0.5), width);
    return sgr(color) + QString(filled, QChar(0x2588)) + sgr(kTrack) + QString(width - filled, QChar(0x2588)) + kReset;
}

QString drawAnsi(const Overview &o, int columns)
{
    columns = qBound(24, columns, 200);
    QStringList out;
    const QString folder = tildePath(o.folder);
    out << kDim + (folder.size() > columns ? QChar(0x2026) + folder.right(columns - 1) : folder) + kReset;
    out << kBold + formatSize(o.bytes) + kReset + kDim + QStringLiteral(" · ") + (o.measuring ? QStringLiteral("measuring…") : itemsText(o)) + kReset;
    if (o.diskTotal > 0) {
        const double share = double(o.diskUsed) / double(o.diskTotal);
        out << blocks(columns, share, levelIndex(share));
        if (diskText(o).size() <= columns) {
            out << kDim + diskText(o) + kReset;
        } else { // a narrow pane: on two lines
            out << kDim + padded(QStringLiteral("Disk %1: %2 of %3").arg(o.diskName, formatSize(o.diskUsed), formatSize(o.diskTotal)), columns).trimmed() + kReset;
            out << kDim + QStringLiteral("%1 free").arg(formatSize(o.diskTotal - o.diskUsed)) + kReset;
        }
    }
    out << QString() << kBold + QStringLiteral("BIGGEST INSIDE") + kReset;
    if (o.rows.isEmpty())
        out << kDim + (o.measuring ? QStringLiteral("Nothing measured yet") : QStringLiteral("The folder is empty")) + kReset;
    // name, a bar (where there is room for one), the size, its share
    const int sizeWidth = 9, shareWidth = 4;
    const int barWidth = columns >= 40 ? qBound(6, columns / 5, 24) : 0;
    const int nameWidth = columns - sizeWidth - shareWidth - 2 - (barWidth ? barWidth + 1 : 0);
    const int shown = qMin<int>(100, o.rows.size()); // the pane scrolls
    for (int i = 0; i < shown; ++i) {
        const Row &row = o.rows.at(i);
        QString line = (row.smaller ? kDim : kReset) + padded(row.name, nameWidth) + kReset + QLatin1Char(' ');
        if (barWidth)
            line += blocks(barWidth, o.bytes > 0 ? double(row.bytes) / double(o.bytes) : 0, kAccent) + QLatin1Char(' ');
        line += padded(formatSize(row.bytes), sizeWidth, true) + QLatin1Char(' ') + kDim + padded(percent(row.bytes, o.bytes), shareWidth, true) + kReset;
        out << line;
    }
    if (o.rows.size() > shown)
        out << kDim + restText(o, shown) + kReset;
    return out.join(QLatin1Char('\n'));
}

// Measures `folder` and waits for it: for --png and --text.
Overview measured(const QString &folder)
{
    DiskUsageModel model;
    model.setShowFiles(true);
    QEventLoop loop;
    QObject::connect(&model, &DiskUsageModel::finished, &loop, &QEventLoop::quit);
    model.setRoot(folder);
    if (model.isScanning())
        loop.exec();
    return read(model);
}

} // namespace

int main(int argc, char **argv)
{
    // Pictures with text need Qt's drawing, but no window is ever shown.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    if (app.arguments().value(1) == QLatin1String("--text")) { // as the terminal interface shows it
        std::printf("%s\n", qPrintable(drawAnsi(measured(app.arguments().value(2, QDir::homePath())),
                                                app.arguments().value(3, QStringLiteral("44")).toInt())));
        return 0;
    }
    if (app.arguments().value(1) == QLatin1String("--png")) {
        const QImage image = draw(measured(app.arguments().value(3, QDir::homePath())), app.arguments().value(4, QStringLiteral("560")).toInt(),
                                  app.arguments().value(5, QStringLiteral("800")).toInt(),
                                  lookFor(app.arguments().value(6) != QLatin1String("light")));
        return save(image, app.arguments().value(2)) ? 0 : 1;
    }

    const QString id = QStringLiteral("space");
    QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (runtime.isEmpty() || !QFileInfo(runtime).isWritable())
        runtime = QDir::tempPath();
    const QString picture = QStringLiteral("%1/argonaut-space-%2.png").arg(runtime).arg(::getpid());
    int width = 560, height = 760;
    Look look = lookFor(true);
    bool shown = false;    // Argonaut has it on screen
    bool terminal = false; // it is Argonaut's terminal interface: text, not a picture
    int columns = 44;      // …with room for this many characters
    bool starting = false; // between a command and its measuring having begun
    Overview overview;     // what was measured, once it is complete
    QString note;
    QElapsedTimer sinceShown;

    DiskUsageModel model;
    model.setShowFiles(true);

    auto show = [&](bool update) {
        if (model.rootPath().isEmpty())
            return; // nothing asked for yet
        const Overview current = model.isScanning() ? read(model) : overview;
        QJsonObject message{{QStringLiteral("type"), QStringLiteral("show")},
                            {QStringLiteral("id"), id},
                            {QStringLiteral("title"), QStringLiteral("Space Overview")},
                            {QStringLiteral("note"), model.isScanning() ? QStringLiteral("Measuring…") : note}};
        if (terminal) {
            message.insert(QStringLiteral("kind"), QStringLiteral("ansi"));
            message.insert(QStringLiteral("text"), drawAnsi(current, columns));
        } else {
            if (!save(draw(current, width, height, look), picture))
                return;
            message.insert(QStringLiteral("kind"), QStringLiteral("image"));
            message.insert(QStringLiteral("image"), picture);
        }
        if (update)
            message.insert(QStringLiteral("update"), true);
        send(message);
        sinceShown.start();
    };

    QObject::connect(&model, &DiskUsageModel::finished, &app, [&] {
        if (starting || model.isScanning())
            return;
        overview = read(model);
        note = QStringLiteral("Measured at %1 in %2 s%3 · choose Space Overview again to measure again")
                   .arg(QTime::currentTime().toString(QStringLiteral("HH:mm")), QString::number(model.scanMs() / 1000.0, 'f', 1),
                        overview.incomplete ? QStringLiteral(" · some folders could not be read") : QString());
        show(true);
    });
    // While measuring, what there is so far, now and then.
    QObject::connect(&model, &DiskUsageModel::progress, &app, [&] {
        if (shown && model.isScanning() && sinceShown.elapsed() >= 700)
            show(true);
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
                      {QStringLiteral("commands"), QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("space")},
                                                                          {QStringLiteral("label"), QStringLiteral("Space Overview")},
                                                                          {QStringLiteral("menu"), QStringLiteral("view")}}}}});
            } else if (type == QLatin1String("invoke")) {
                const QString folder = message.value(QLatin1String("folder")).toString();
                if (!QDir::isAbsolutePath(folder) || !QFileInfo(folder).isDir()) { // Recent, Starred, a list of files
                    send({{QStringLiteral("type"), QStringLiteral("message")},
                          {QStringLiteral("text"), QStringLiteral("Space Overview needs a folder: open one first")}});
                    continue;
                }
                // Always measured anew: nothing is watched, so what an earlier
                // command measured may no longer be true.
                starting = true;
                const bool same = QDir::cleanPath(folder) == model.rootPath();
                model.setRoot(folder);
                if (same || !model.isScanning())
                    model.rescan();
                starting = false;
                show(false); // Argonaut answers with the room there is
            } else if (type == QLatin1String("result") && message.value(QLatin1String("id")).toString() == id) {
                shown = message.value(QLatin1String("shown")).toBool();
                if (!shown)
                    continue;
                width = message.value(QLatin1String("width")).toInt();
                height = message.value(QLatin1String("height")).toInt();
                if (message.value(QLatin1String("columns")).toInt() > 0)
                    columns = message.value(QLatin1String("columns")).toInt();
                look = lookFor(message.value(QLatin1String("dark")).toBool(), message.value(QLatin1String("colors")).toObject(),
                               message.value(QLatin1String("font")).toString().left(100));
                show(true); // drawn for this room, in this theme
            }
        }
    });

    const int code = app.exec();
    QFile::remove(picture);
    return code;
}
