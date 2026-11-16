#pragma once

// What the extensions that draw their own result share (System Dashboard,
// Space Overview): sending a message to Argonaut, the colours and font of a
// picture (Argonaut's theme, as its "result" message tells them), and the
// colour codes of the same thing drawn with characters for the terminal
// interface.

#include <QColor>
#include <QFont>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QSaveFile>
#include <QString>

#include <cstdio>

inline void send(const QJsonObject &message)
{
    const QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    std::fwrite(line.constData(), 1, size_t(line.size()), stdout);
    std::fflush(stdout);
}

// ------------------------------------------------------------ picture ---

// The colours and font of the picture: Argonaut's theme (so the result looks
// like part of the window, in every theme), or a dark or light look of its
// own until Argonaut has told them.
struct Look
{
    QColor window, card, track, text, dim, accent;
    QString font;
};

inline QColor mix(const QColor &a, const QColor &b, double share)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * share, a.greenF() + (b.greenF() - a.greenF()) * share,
                            a.blueF() + (b.blueF() - a.blueF()) * share);
}

inline Look lookFor(bool dark, const QJsonObject &colors = {}, const QString &font = {})
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
    look.track = mix(look.card, look.text, 0.10); // the empty part of a gauge or bar
    look.font = font;
    return look;
}

// A gauge or bar is in the theme's accent colour, and warns when it is nearly full.
inline QColor levelColor(const Look &look, double share)
{
    if (share >= 0.9)
        return QColor(0xe0, 0x1b, 0x24);
    if (share >= 0.75)
        return QColor(0xe6, 0x61, 0x00);
    return look.accent;
}

inline QString g_font; // the family every text is drawn in (empty: Qt's own)

inline QFont fontOf(double pixels, bool bold = false)
{
    QFont font;
    if (!g_font.isEmpty())
        font.setFamily(g_font);
    font.setPixelSize(qMax(8, int(pixels + 0.5)));
    font.setBold(bold);
    return font;
}

// A panel; `u` is the size of ordinary text, the unit everything is laid out in.
inline void card(QPainter &p, const QRectF &r, const Look &look, double u)
{
    p.setPen(Qt::NoPen);
    p.setBrush(look.card);
    p.drawRoundedRect(r, u * 0.5, u * 0.5);
}

inline bool save(const QImage &image, const QString &path)
{
    // Whole or not at all: Argonaut reads it as soon as it is told.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    return image.save(&file, "PNG", 80) && file.commit(); // 80: little compression, quick
}

// ---------------------------------------------- the same, in a terminal ---

inline QString sgr(int fg, int bg = -1)
{
    QString code = QStringLiteral("\x1b[0");
    if (fg >= 0)
        code += QStringLiteral(";38;5;%1").arg(fg);
    if (bg >= 0)
        code += QStringLiteral(";48;5;%1").arg(bg);
    return code + QLatin1Char('m');
}

inline const QString kReset = QStringLiteral("\x1b[0m");
inline const QString kBold = QStringLiteral("\x1b[0;1m");
inline const QString kDim = QStringLiteral("\x1b[0;38;5;8m");
constexpr int kTrack = 238;  // the empty part of a gauge or bar: a dark grey
constexpr int kAccent = 6;   // the terminal's own cyan, as the rest of the interface
constexpr int kNearlyFull = 208;
constexpr int kFull = 196;

inline int levelIndex(double share)
{
    return share >= 0.9 ? kFull : share >= 0.75 ? kNearlyFull : kAccent;
}

// A text in exactly `width` cells: cut with … or filled with spaces (at the left for `right`).
inline QString padded(const QString &text, int width, bool right = false)
{
    const QString cut = text.size() > width ? text.left(qMax(0, width - 1)) + QChar(0x2026) : text;
    const QString space(width - int(cut.size()), QLatin1Char(' '));
    return right ? space + cut : cut + space;
}

// A bar of `width` cells, `share` of it in `color`.
inline QString blocks(int width, double share, int color)
{
    const int filled = qBound(0, int(width * share + 0.5), width);
    return sgr(color) + QString(filled, QChar(0x2588)) + sgr(kTrack) + QString(width - filled, QChar(0x2588)) + kReset;
}

// ------------------------------------------------ a list, biggest first ---
// What Running Services and Running Processes show: a panel that says how
// many there are with one bar, then a row each with a bar of its own.

struct ListRow
{
    QString name;
    QString mark;    // a word that belongs to the name ("user"); kept when the name is cut
    QString about;   // what it is, where there is room
    QString size;    // at the right ("1.1 GB")
    double share = 0; // its bar, 0…1
    bool faint = false; // drawn lighter (not the system's, not yours)
};

struct ListHead
{
    QString title;     // "RUNNING SERVICES"
    QString big;       // the number ("81")
    QString beside;    // at the right of it ("35 system · 46 user")
    double share = 0;  // the top bar
    QString line;      // under the bar ("Together they use\n3.8 GB of 33.6 GB memory": the line break is
                       // where it is cut when it doesn't fit on one line, and a space otherwise)
    QString listTitle; // "BY MEMORY"
    QString empty;     // said when there are no rows
    QString more;      // said under the rows when some were left out
};

// A name from outside (a process's, a service's) is data: a control
// character in it (an escape sequence, a line break) becomes "?".
inline QString plainText(QString text)
{
    for (QChar &c : text) {
        if (c.unicode() < 32 || c.unicode() == 127 || (c.unicode() >= 0x80 && c.unicode() < 0xa0))
            c = QLatin1Char('?');
    }
    return text;
}

inline QList<ListRow> plainRows(QList<ListRow> rows)
{
    for (ListRow &row : rows) {
        row.name = plainText(row.name);
        row.mark = plainText(row.mark);
        row.about = plainText(row.about);
    }
    return rows;
}

// Before drawing: only the rows that have `search` in them (in the name,
// its mark or what it is; case doesn't matter), with the bars against the
// biggest of those; then at most `most` of them, the rest said in one line.
inline void narrowList(ListHead *head, QList<ListRow> *rows, const QString &search, int most)
{
    const QString wanted = search.trimmed();
    if (!wanted.isEmpty()) {
        const int all = int(rows->size());
        QList<ListRow> kept;
        double biggest = 0;
        for (const ListRow &row : std::as_const(*rows)) {
            if (row.name.contains(wanted, Qt::CaseInsensitive) || row.mark.contains(wanted, Qt::CaseInsensitive)
                || row.about.contains(wanted, Qt::CaseInsensitive)) {
                kept.append(row);
                biggest = qMax(biggest, row.share);
            }
        }
        for (ListRow &row : kept)
            row.share = biggest > 0 ? row.share / biggest : 0;
        *rows = kept;
        head->big = QString::number(kept.size());
        head->beside = QStringLiteral("of %1 · with “%2”").arg(all).arg(plainText(wanted));
        head->empty = QStringLiteral("Nothing has “%1” in it").arg(plainText(wanted));
    }
    if (rows->size() > most) {
        head->more = QStringLiteral("and %1 more, each using less").arg(rows->size() - most);
        rows->erase(rows->begin() + most, rows->end());
    }
}

// The picture, for a pane `width` pixels wide: as long as the rows make it
// (sent with "scroll": true, Argonaut scrolls it).
inline QImage drawList(const ListHead &head, const QList<ListRow> &given, int width, const Look &look)
{
    const QList<ListRow> rows = plainRows(given);
    width = qBound(240, width, 4000);
    g_font = look.font;
    const double u = qBound(10.0, width / 34.0, 20.0); // the size of ordinary text
    const double rowHeight = u * 2.7;
    const double headHeight = u * 9.6;
    const double listTop = headHeight + 2 * u;
    const int count = int(rows.size());
    const double listHeight = u * 3.2 + rowHeight * qMax(1, count) + (head.more.isEmpty() ? 0 : u * 1.9);

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

    card(p, QRectF(u, u, width - 2 * u, headHeight), look, u);
    p.setPen(look.dim);
    p.setFont(fontOf(u));
    text(u * 1.5, u * 1.5, Qt::AlignLeft, head.title);
    p.setPen(look.text);
    p.setFont(fontOf(u * 2.1, true));
    text(u * 3.1, u * 2.8, Qt::AlignLeft, head.big);
    p.setPen(look.dim);
    p.setFont(fontOf(u));
    text(u * 3.9, u * 1.8, Qt::AlignRight, head.beside);
    bar(u * 7.0, u * 0.8, head.share, levelColor(look, head.share));
    p.setPen(look.dim);
    p.setFont(fontOf(u * 0.9));
    text(u * 8.3, u * 1.6, Qt::AlignLeft, QString(head.line).replace(QLatin1Char('\n'), QLatin1Char(' ')));

    card(p, QRectF(u, listTop, width - 2 * u, listHeight), look, u);
    p.setPen(look.text);
    p.setFont(fontOf(u * 1.05));
    text(listTop + u * 0.7, u * 1.6, Qt::AlignLeft, head.listTitle);
    double y = listTop + u * 2.8;
    if (count == 0) {
        p.setPen(look.dim);
        p.setFont(fontOf(u));
        text(y, u * 1.6, Qt::AlignLeft, head.empty);
    }
    for (const ListRow &row : rows) {
        p.setFont(fontOf(u));
        const int sizeWidth = p.fontMetrics().horizontalAdvance(row.size);
        p.setPen(look.dim);
        p.drawText(QRectF(left, y, room, u * 1.6), Qt::AlignRight | Qt::AlignVCenter, row.size);
        // The name, then what it is in the room that is left.
        const double nameRoom = room - sizeWidth - u;
        const QString name = p.fontMetrics().elidedText(row.name, Qt::ElideMiddle, qMax(10, int(nameRoom)));
        p.setPen(look.text);
        p.drawText(QRectF(left, y, nameRoom, u * 1.6), Qt::AlignLeft | Qt::AlignVCenter, name);
        const double used = p.fontMetrics().horizontalAdvance(name) + u * 0.8;
        const QString about = row.mark.isEmpty() ? row.about : row.about.isEmpty() ? row.mark : row.mark + QStringLiteral(" · ") + row.about;
        if (!about.isEmpty() && nameRoom - used > u * 5) {
            p.setPen(look.dim);
            p.setFont(fontOf(u * 0.9));
            p.drawText(QRectF(left + used, y, nameRoom - used, u * 1.6), Qt::AlignLeft | Qt::AlignVCenter,
                       p.fontMetrics().elidedText(about, Qt::ElideRight, int(nameRoom - used)));
        }
        bar(y + u * 1.75, u * 0.45, row.share, row.faint ? mix(look.accent, look.card, 0.45) : look.accent);
        y += rowHeight;
    }
    if (!head.more.isEmpty()) {
        p.setPen(look.dim);
        p.setFont(fontOf(u * 0.95));
        text(y, u * 1.6, Qt::AlignLeft, head.more);
    }
    return image;
}

// The same as text with colour codes, for the terminal interface: a line a
// row (the pane scrolls), nothing wider than `columns`.
inline QString drawListAnsi(const ListHead &head, const QList<ListRow> &given, int columns)
{
    const QList<ListRow> rows = plainRows(given); // a name can't bring colour codes of its own

    columns = qBound(24, columns, 200);
    QStringList out;
    out << kBold + head.title.left(columns) + kReset;
    out << kBold + head.big + kReset + kDim + (QStringLiteral(" · ") + head.beside).left(qMax(0, columns - int(head.big.size()))) + kReset;
    out << blocks(columns, head.share, levelIndex(head.share));
    // The line under the bar, on as many lines as it needs.
    QString rest = head.line;
    if (rest.size() <= columns)
        rest.replace(QLatin1Char('\n'), QLatin1Char(' '));
    while (!rest.isEmpty()) {
        const qsizetype given = rest.indexOf(QLatin1Char('\n'));
        if (given >= 0 && given <= columns) { // cut where it was meant to be
            out << kDim + rest.left(given) + kReset;
            rest = rest.mid(given + 1);
            continue;
        }
        qsizetype cut = rest.size() <= columns ? rest.size() : rest.lastIndexOf(QLatin1Char(' '), columns);
        if (cut <= 0)
            cut = columns;
        out << kDim + rest.left(cut) + kReset;
        rest = rest.mid(cut).trimmed();
    }
    out << QString() << kBold + head.listTitle.left(columns) + kReset;
    if (rows.isEmpty())
        out << kDim + head.empty.left(columns) + kReset;
    // name, a bar (where there is room for one), the size
    const int sizeWidth = 9;
    const int barWidth = columns >= 40 ? qBound(6, columns / 5, 24) : 0;
    const int nameWidth = columns - sizeWidth - 1 - (barWidth ? barWidth + 1 : 0);
    for (const ListRow &row : rows) {
        const QString mark = row.mark.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(row.mark).left(qMax(0, nameWidth - 2));
        const QString name = padded(row.name, qMax(1, qMin(int(row.name.size()), nameWidth - int(mark.size()))));
        QString line = kReset + name + kDim + mark + kReset + QString(qMax(0, nameWidth - int(name.size()) - int(mark.size())), QLatin1Char(' ')) + QLatin1Char(' ');
        if (barWidth)
            line += blocks(barWidth, row.share, kAccent) + QLatin1Char(' ');
        out << line + padded(row.size, sizeWidth, true);
    }
    if (!head.more.isEmpty())
        out << kDim + head.more.left(columns) + kReset;
    return out.join(QLatin1Char('\n'));
}
