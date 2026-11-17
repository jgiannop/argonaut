// The Document Preview extension: previews for the pane Space opens.
//   Markdown            sent as it is; Argonaut formats it
//   HTML                as a page to read: its text and structure, without
//                       its scripts, styles and pictures (nothing is run,
//                       nothing is fetched)
//   CSV, TSV            the first rows as a table
//   .xlsx, .ods         the first sheet as a table
// Spreadsheets are zip files full of XML from who knows where, so they are
// not read here: this program starts itself again inside a sandbox
// (`--table FILE`, see Sandbox::run), which reads the one file and prints the
// table. Without a sandbox, spreadsheets aren't previewed at all.

#include "../shared/Conversation.h"
#include "Sandbox.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QSocketNotifier>
#include <QXmlStreamReader>

#include <functional>

#include <cstdio>

#if defined(ARGONAUT_ARCHIVES)
#include <archive.h>
#include <archive_entry.h>
#endif

namespace {

constexpr int MaxRows = 200;
constexpr int MaxColumns = 30;
constexpr int MaxCellChars = 200;
constexpr qint64 MaxTextBytes = 400 * 1000;
constexpr qint64 MaxHtmlBytes = 3 * 1000 * 1000; // a page's source is mostly what isn't shown
constexpr qsizetype MaxPageChars = 300 * 1000;   // of what is
constexpr qint64 MaxEntryBytes = 32 * 1024 * 1024; // one XML file inside a spreadsheet

using Table = QList<QStringList>;

struct Sheet
{
    Table rows;
    bool more = false;  // rows or columns were left out
    int sheets = 1;
    bool ok = false;
};

// ---------------------------------------------------------------- tables ---

QString toHtml(const Table &rows)
{
    int columns = 0;
    for (const QStringList &row : rows)
        columns = qMax(columns, int(row.size()));
    QString html = QStringLiteral("<table border=\"1\" cellspacing=\"0\" cellpadding=\"5\" "
                                  "style=\"border-collapse: collapse; border-color: #888888;\">");
    bool first = true;
    for (const QStringList &row : rows) {
        html += QStringLiteral("<tr>");
        for (int c = 0; c < columns; ++c) {
            QString cell = c < row.size() ? row.at(c) : QString();
            if (cell.size() > MaxCellChars)
                cell = cell.left(MaxCellChars) + QStringLiteral("…");
            cell = cell.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"));
            html += first ? QStringLiteral("<th align=\"left\">%1</th>").arg(cell) : QStringLiteral("<td>%1</td>").arg(cell);
        }
        html += QStringLiteral("</tr>");
        first = false;
    }
    return html + QStringLiteral("</table>");
}

// Rows and columns that are empty at the end aren't shown.
void trim(Table *rows)
{
    for (QStringList &row : *rows) {
        while (!row.isEmpty() && row.last().trimmed().isEmpty())
            row.removeLast();
    }
    while (!rows->isEmpty() && rows->last().isEmpty())
        rows->removeLast();
}

// CSV as programs write it: quoted cells may hold the separator, line breaks
// and doubled quotes. The separator is the one the first line uses most.
Sheet readCsv(const QString &text)
{
    Sheet sheet;
    const QString firstLine = text.section(QLatin1Char('\n'), 0, 0);
    QChar separator = QLatin1Char(',');
    int best = firstLine.count(separator);
    for (const QChar candidate : {QLatin1Char(';'), QLatin1Char('\t'), QLatin1Char('|')}) {
        if (firstLine.count(candidate) > best) {
            best = firstLine.count(candidate);
            separator = candidate;
        }
    }
    QStringList row;
    QString cell;
    bool quoted = false;
    auto endCell = [&] {
        if (row.size() < MaxColumns)
            row << cell;
        else
            sheet.more = true;
        cell.clear();
    };
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (quoted) {
            if (c == QLatin1Char('"') && i + 1 < text.size() && text.at(i + 1) == QLatin1Char('"')) {
                cell += c;
                ++i;
            } else if (c == QLatin1Char('"')) {
                quoted = false;
            } else {
                cell += c;
            }
        } else if (c == QLatin1Char('"') && cell.isEmpty()) {
            quoted = true;
        } else if (c == separator) {
            endCell();
        } else if (c == QLatin1Char('\n')) {
            endCell();
            sheet.rows << row;
            row.clear();
            if (sheet.rows.size() >= MaxRows) {
                sheet.more = sheet.more || i + 1 < text.size();
                break;
            }
        } else if (c != QLatin1Char('\r')) {
            cell += c;
        }
    }
    if (sheet.rows.size() < MaxRows && (!cell.isEmpty() || !row.isEmpty())) {
        endCell();
        sheet.rows << row;
    }
    trim(&sheet.rows);
    sheet.ok = !sheet.rows.isEmpty();
    return sheet;
}

// ------------------------------------------- spreadsheets (in the sandbox) ---

#if defined(ARGONAUT_ARCHIVES)
// The files inside the zip that `wanted` accepts, by name, each at most
// MaxEntryBytes; and every name it has.
QHash<QString, QByteArray> readZip(const QString &path, const std::function<bool(const QString &)> &wanted,
                                   QStringList *names)
{
    QHash<QString, QByteArray> out;
    struct archive *a = archive_read_new();
    archive_read_support_format_zip(a);
    if (archive_read_open_filename(a, QFile::encodeName(path).constData(), 64 * 1024) != ARCHIVE_OK) {
        archive_read_free(a);
        return out;
    }
    struct archive_entry *entry = nullptr;
    int seen = 0;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK && ++seen < 5000) {
        const QString name = QString::fromUtf8(archive_entry_pathname(entry));
        if (names)
            *names << name;
        if (!wanted(name))
            continue;
        QByteArray data;
        char chunk[64 * 1024];
        la_ssize_t n;
        while ((n = archive_read_data(a, chunk, sizeof chunk)) > 0 && data.size() < MaxEntryBytes)
            data.append(chunk, n);
        out.insert(name, data);
    }
    archive_read_free(a);
    return out;
}

// "BC12" -> 54 (the column, counted from 0).
int columnOf(const QString &reference)
{
    int column = 0;
    for (const QChar c : reference) {
        if (!c.isLetter())
            break;
        column = column * 26 + (c.toUpper().unicode() - 'A' + 1);
    }
    return column - 1;
}

Sheet readXlsx(const QString &path)
{
    Sheet sheet;
    QStringList names;
    const QHash<QString, QByteArray> files = readZip(path, [](const QString &name) {
        return name == QLatin1String("xl/sharedStrings.xml") || name.startsWith(QLatin1String("xl/worksheets/sheet"));
    }, &names);
    QStringList sheetNames;
    for (const QString &name : names) {
        if (name.startsWith(QLatin1String("xl/worksheets/")) && name.endsWith(QLatin1String(".xml"))
            && name.count(QLatin1Char('/')) == 2)
            sheetNames << name;
    }
    if (sheetNames.isEmpty())
        return sheet;
    sheetNames.sort();
    sheet.sheets = int(sheetNames.size());
    const QString firstSheet = sheetNames.contains(QStringLiteral("xl/worksheets/sheet1.xml"))
                                   ? QStringLiteral("xl/worksheets/sheet1.xml") : sheetNames.first();

    // Texts are kept once, in their own file; cells refer to them by number.
    QStringList shared;
    {
        QXmlStreamReader xml(files.value(QStringLiteral("xl/sharedStrings.xml")));
        QString current;
        bool inItem = false;
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement() && xml.name() == QLatin1String("si")) {
                inItem = true;
                current.clear();
            } else if (xml.isStartElement() && xml.name() == QLatin1String("rPh")) {
                xml.skipCurrentElement(); // how to pronounce it: not the text
            } else if (inItem && xml.isStartElement() && xml.name() == QLatin1String("t")) {
                current += xml.readElementText();
            } else if (xml.isEndElement() && xml.name() == QLatin1String("si")) {
                shared << current;
                inItem = false;
            }
        }
    }
    QXmlStreamReader xml(files.value(firstSheet));
    QStringList row;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == QLatin1String("row")) {
            row.clear();
        } else if (xml.isEndElement() && xml.name() == QLatin1String("row")) {
            if (sheet.rows.size() >= MaxRows) {
                sheet.more = true;
                break;
            }
            sheet.rows << row;
        } else if (xml.isStartElement() && xml.name() == QLatin1String("c")) {
            const int column = columnOf(xml.attributes().value(QLatin1String("r")).toString());
            const QString type = xml.attributes().value(QLatin1String("t")).toString();
            QString value;
            while (xml.readNextStartElement()) {
                if (xml.name() == QLatin1String("v")) {
                    value = xml.readElementText();
                } else if (xml.name() == QLatin1String("is")) { // text written in the cell itself
                    while (xml.readNextStartElement()) {
                        if (xml.name() == QLatin1String("t"))
                            value += xml.readElementText();
                        else
                            xml.skipCurrentElement();
                    }
                } else {
                    xml.skipCurrentElement();
                }
            }
            if (type == QLatin1String("s"))
                value = shared.value(value.toInt());
            else if (type == QLatin1String("b"))
                value = value == QLatin1String("1") ? QStringLiteral("TRUE") : QStringLiteral("FALSE");
            const int at = column >= 0 ? column : int(row.size());
            if (at >= MaxColumns) {
                sheet.more = true;
                continue;
            }
            while (row.size() < at)
                row << QString();
            row << value;
        }
    }
    trim(&sheet.rows);
    sheet.ok = true;
    return sheet;
}

Sheet readOds(const QString &path)
{
    Sheet sheet;
    const QHash<QString, QByteArray> files =
        readZip(path, [](const QString &name) { return name == QLatin1String("content.xml"); }, nullptr);
    if (!files.contains(QStringLiteral("content.xml")))
        return sheet;
    QXmlStreamReader xml(files.value(QStringLiteral("content.xml")));
    int tables = 0;
    bool inFirst = false;
    QStringList row;
    int rowRepeat = 1;
    while (!xml.atEnd()) {
        xml.readNext();
        const auto name = xml.name();
        if (xml.isStartElement() && name == QLatin1String("table")) {
            inFirst = ++tables == 1;
        } else if (xml.isEndElement() && name == QLatin1String("table")) {
            inFirst = false;
        } else if (!inFirst) {
            continue;
        } else if (xml.isStartElement() && name == QLatin1String("table-row")) {
            row.clear();
            rowRepeat = qMax(1, xml.attributes().value(QLatin1String("table:number-rows-repeated")).toInt());
        } else if (xml.isEndElement() && name == QLatin1String("table-row")) {
            // A run of the same row: empty ones (the sheet's unused rest) once.
            const bool empty = row.join(QString()).trimmed().isEmpty();
            for (int i = 0; i < (empty ? 1 : rowRepeat); ++i) {
                if (sheet.rows.size() >= MaxRows) {
                    sheet.more = true;
                    break;
                }
                sheet.rows << row;
            }
        } else if (xml.isStartElement() && (name == QLatin1String("table-cell") || name == QLatin1String("covered-table-cell"))) {
            const int repeat = qMax(1, xml.attributes().value(QLatin1String("table:number-columns-repeated")).toInt());
            QStringList lines;
            while (xml.readNextStartElement()) {
                if (xml.name() == QLatin1String("p"))
                    lines << xml.readElementText(QXmlStreamReader::IncludeChildElements);
                else
                    xml.skipCurrentElement();
            }
            const QString value = lines.join(QLatin1Char('\n'));
            for (int i = 0; i < repeat; ++i) {
                if (row.size() >= MaxColumns) {
                    sheet.more = sheet.more || !value.isEmpty();
                    break;
                }
                row << value;
            }
        }
    }
    sheet.sheets = qMax(1, tables);
    trim(&sheet.rows);
    sheet.ok = tables > 0;
    return sheet;
}
#endif

// `--table FILE`: what the sandboxed copy of this program does. One line of
// JSON on standard output: {"html", "more", "sheets"} or nothing.
int printTable(const QString &path)
{
    Sheet sheet;
#if defined(ARGONAUT_ARCHIVES)
    sheet = readXlsx(path);
    if (!sheet.ok)
        sheet = readOds(path);
#else
    Q_UNUSED(path);
#endif
    if (!sheet.ok)
        return 1;
    const QJsonObject out{{QStringLiteral("html"), toHtml(sheet.rows)},
                          {QStringLiteral("more"), sheet.more},
                          {QStringLiteral("sheets"), sheet.sheets},
                          {QStringLiteral("rows"), int(sheet.rows.size())}};
    const QByteArray line = QJsonDocument(out).toJson(QJsonDocument::Compact) + '\n';
    std::fwrite(line.constData(), 1, size_t(line.size()), stdout);
    return 0;
}

// -------------------------------------------------------------- protocol ---

QString tableNote(bool more, int rows, int sheets)
{
    QStringList parts;
    if (more)
        parts << QStringLiteral("Showing the first %1 rows and %2 columns at most").arg(MaxRows).arg(MaxColumns);
    else
        parts << (rows == 1 ? QStringLiteral("1 row") : QStringLiteral("%1 rows").arg(rows));
    if (sheets > 1)
        parts << QStringLiteral("first of %1 sheets").arg(sheets);
    return parts.join(QStringLiteral(" · "));
}

// An HTML file as a page to read. Argonaut shows it in a rich-text view
// that runs nothing and loads nothing, and knows only simple HTML: so what
// such a view can't use is taken out here (scripts, style sheets, pictures,
// frames, drawings), leaving the text with its headings, lists, tables and
// links. A picture leaves its description, where it has one.
QString readablePage(QString html, bool *pictures)
{
    using Re = QRegularExpression;
    const auto options = Re::CaseInsensitiveOption | Re::DotMatchesEverythingOption;
    // With what is inside them; one that is never closed goes to the end.
    for (const char *tag : {"script", "style", "svg", "iframe", "object", "embed", "template", "canvas", "video", "audio", "head"}) {
        if (qstrcmp(tag, "head") == 0) {
            // The head has nothing to show, but its title is worth keeping apart.
            html.remove(Re(QStringLiteral("<head\\b.*?</head\\s*>"), options));
            continue;
        }
        html.remove(Re(QStringLiteral("<%1\\b.*?(</%1\\s*>|$)").arg(QLatin1String(tag)), options));
    }
    html.remove(Re(QStringLiteral("<!--.*?(-->|$)"), options));
    html.remove(Re(QStringLiteral("<(link|meta|base|source|track)\\b[^>]*>"), options));
    // A picture: its description in its place.
    const Re image(QStringLiteral("<img\\b[^>]*>"), options);
    const Re alt(QStringLiteral("\\balt\\s*=\\s*(\"([^\"]*)\"|'([^']*)')"), options);
    QString out;
    qsizetype from = 0;
    auto it = image.globalMatch(html);
    while (it.hasNext()) {
        const auto match = it.next();
        *pictures = true;
        out += html.mid(from, match.capturedStart() - from);
        const auto described = alt.match(match.captured());
        const QString words = (described.captured(2) + described.captured(3)).trimmed();
        if (!words.isEmpty())
            out += QStringLiteral("<i>[%1]</i>").arg(words.toHtmlEscaped());
        from = match.capturedEnd();
    }
    out += html.mid(from);
    // Styles written on the elements themselves (colours made for the page's
    // own background) go too: inside tags only, never in what is read.
    const Re tag(QStringLiteral("<[a-z][^>]*>"), options);
    const Re styling(QStringLiteral("\\s(style|class|id|bgcolor|background|color|on[a-z]+)\\s*=\\s*(\"[^\"]*\"|'[^']*'|[^\\s>]+)"), options);
    QString plain;
    from = 0;
    auto tags = tag.globalMatch(out);
    while (tags.hasNext()) {
        const auto match = tags.next();
        plain += QStringView(out).mid(from, match.capturedStart() - from);
        plain += match.captured().remove(styling);
        from = match.capturedEnd();
    }
    plain += QStringView(out).mid(from);
    return plain;
}

QString titleOf(const QString &html)
{
    const auto match = QRegularExpression(QStringLiteral("<title\\b[^>]*>(.*?)</title\\s*>"),
                                          QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption)
                           .match(html);
    return match.captured(1).simplified().left(120);
}

QJsonObject htmlPreview(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {{QStringLiteral("kind"), QStringLiteral("none")}};
    const QByteArray bytes = file.read(MaxHtmlBytes);
    if (bytes.left(4096).contains('\0'))
        return {{QStringLiteral("kind"), QStringLiteral("none")}};
    bool cut = !file.atEnd();
    // In the page's own encoding (its <meta charset>, or a byte order mark; else UTF-8).
    QStringDecoder decoder = QStringDecoder::decoderForHtml(bytes);
    const QString source = decoder.isValid() ? QString(decoder.decode(bytes)) : QString::fromUtf8(bytes);
    bool pictures = false;
    QString page = readablePage(source, &pictures);
    if (page.size() > MaxPageChars) {
        // Not in the middle of a tag.
        const qsizetype at = page.lastIndexOf(QLatin1Char('<'), MaxPageChars);
        page.truncate(at > 0 ? at : MaxPageChars);
        cut = true;
    }
    QStringList notes;
    const QString title = titleOf(source);
    if (!title.isEmpty())
        notes << title;
    notes << (pictures ? QStringLiteral("As a page to read: without its styles, scripts and pictures")
                       : QStringLiteral("As a page to read: without its styles and scripts"));
    if (cut)
        notes << QStringLiteral("only the beginning");
    return {{QStringLiteral("kind"), QStringLiteral("html")}, {QStringLiteral("text"), page}, {QStringLiteral("note"), notes.join(QStringLiteral(" · "))}};
}

QJsonObject preview(const QString &path, const QString &mime)
{
    QJsonObject none{{QStringLiteral("kind"), QStringLiteral("none")}};
    const QString suffix = QFileInfo(path).suffix().toLower();
    const bool spreadsheet = mime.contains(QLatin1String("spreadsheet")) || suffix == QLatin1String("xlsx")
                             || suffix == QLatin1String("ods");
    if (spreadsheet) {
        bool ok = false;
        const QByteArray out = Sandbox::run(QCoreApplication::applicationFilePath(),
                                            {QStringLiteral("--table"), QStringLiteral("@FILE@")}, path, &ok, 8000,
                                            4 * 1024 * 1024);
        const QJsonObject table = QJsonDocument::fromJson(out).object();
        if (!ok || !table.contains(QLatin1String("html")))
            return none;
        return {{QStringLiteral("kind"), QStringLiteral("html")},
                {QStringLiteral("text"), table.value(QLatin1String("html")).toString()},
                {QStringLiteral("note"), tableNote(table.value(QLatin1String("more")).toBool(),
                                                   table.value(QLatin1String("rows")).toInt(),
                                                   table.value(QLatin1String("sheets")).toInt(1))}};
    }
    if (mime == QLatin1String("text/html") || mime == QLatin1String("application/xhtml+xml") || suffix == QLatin1String("html")
        || suffix == QLatin1String("htm") || suffix == QLatin1String("xhtml"))
        return htmlPreview(path);
    // Text files: read here, they are only ever shown.
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return none;
    const QByteArray bytes = file.read(MaxTextBytes);
    if (bytes.left(4096).contains('\0'))
        return none; // not text after all
    const bool cut = !file.atEnd();
    const QString text = QString::fromUtf8(bytes);
    if (mime == QLatin1String("text/markdown") || suffix == QLatin1String("md") || suffix == QLatin1String("markdown")) {
        QJsonObject reply{{QStringLiteral("kind"), QStringLiteral("markdown")}, {QStringLiteral("text"), text}};
        if (cut)
            reply.insert(QStringLiteral("note"), QStringLiteral("Showing the first 400 kB"));
        return reply;
    }
    const Sheet sheet = readCsv(text);
    if (!sheet.ok)
        return none;
    return {{QStringLiteral("kind"), QStringLiteral("html")},
            {QStringLiteral("text"), toHtml(sheet.rows)},
            {QStringLiteral("note"), tableNote(sheet.more || cut, int(sheet.rows.size()), 1)}};
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().value(1) == QLatin1String("--table"))
        return printTable(app.arguments().value(2));

    // Of several previews asked for in a row (arrow keys held down), only
    // the last one is still wanted.
    QJsonObject wanted;
    Conversation conversation([&](const QJsonObject &message) {
        const QString type = message.value(QLatin1String("type")).toString();
        if (type == QLatin1String("hello")) {
            send({{QStringLiteral("type"), QStringLiteral("previews")},
                  {QStringLiteral("types"),
                   QJsonArray{QStringLiteral("text/markdown"), QStringLiteral("text/html"), QStringLiteral("application/xhtml+xml"),
                              QStringLiteral("text/csv"),
                              QStringLiteral("text/tab-separated-values"),
                              QStringLiteral("application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"),
                              QStringLiteral("application/vnd.oasis.opendocument.spreadsheet")}}});
        } else if (type == QLatin1String("preview")) {
            wanted = message;
        }
    }, [&] {
        if (wanted.isEmpty())
            return;
        QJsonObject reply = preview(wanted.value(QLatin1String("path")).toString(),
                                    wanted.value(QLatin1String("mime")).toString());
        reply.insert(QStringLiteral("type"), QStringLiteral("preview"));
        reply.insert(QStringLiteral("id"), wanted.value(QLatin1String("id")));
        send(reply);
        wanted = QJsonObject();
    });
    return app.exec();
}
