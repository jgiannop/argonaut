// The preview pane of the terminal interface (Tui::App): what is in the
// item under the cursor (F3), and what an extension shows there (a
// dashboard, a list). A part of App in a file of its own.

#include "App.h"

#include "AppInternal.h"

#include "ContentType.h"
#include "DirectoryModel.h"
#include "Extensions.h"
#include "Format.h"
#include "Preview.h"

#include <QFileInfo>
#include <QImageReader>
#include <QMimeDatabase>
#include <QPointer>
#include <QSettings>
#include <QTextDocument>
#include <QThreadPool>

namespace Tui {

// ------------------------------------------------------------- preview ---

namespace {

// A line of text as the lines it takes in `width` cells (cut where it
// must be; a tab is four spaces).
void wrapInto(QStringList *out, QString line, int width)
{
    line.replace(QLatin1Char('\t'), QStringLiteral("    "));
    if (line.isEmpty()) {
        out->append(QString());
        return;
    }
    while (!line.isEmpty()) {
        int used = 0;
        qsizetype at = 0;
        while (at < line.size()) {
            const qsizetype step = line.at(at).isHighSurrogate() && at + 1 < line.size() ? 2 : 1;
            const int cells = Screen::widthOf(line.mid(at, step));
            if (used + cells > width)
                break;
            used += cells;
            at += step;
        }
        if (at == 0)
            at = 1; // a character wider than the pane: still moves on
        out->append(line.left(at));
        line = line.mid(at);
    }
}

} // namespace

void App::setPreviewShown(bool on)
{
    m_preview = on;
    m_inPreview = false;
    if (!on) {
        closeResult();
        ++m_previewRequest; // an answer still on its way is dropped
        m_previewPath.clear();
        m_previewLoading = false;
        m_previewLines.clear();
        m_previewImage = QImage();
    }
    emit changed();
}

void App::scrollPreview(int top)
{
    m_previewTop = qBound(0, top, qMax(0, m_previewTotal - m_previewShown));
    emit changed();
}

// The preview is of the item under the cursor of the pane you are in: when
// that is another item, it is loaded (Preview::load: the same rules as the
// window's preview; a program in disguise is never opened) on another
// thread. Nothing is run on the file; without a thumbnailer only what Qt
// reads itself becomes a picture.
void App::syncPreview()
{
    const Pane &p = pane();
    const QString path = cursorPath();
    if (m_resultShown)
        return; // an extension's result has the pane while the cursor moves: F3 closes it (and again shows files)
    if (path == m_previewPath)
        return;
    m_previewCells.clear();
    m_previewPath = path;
    const quint64 request = ++m_previewRequest;
    m_previewTop = 0;
    m_previewLines.clear();
    m_previewWrapWidth = -1;
    m_previewImage = QImage();
    m_previewWarning = false;
    m_previewInfo.clear();
    m_previewLoading = false;
    if (path.isEmpty()) {
        m_previewTitle = QStringLiteral("Preview");
        m_previewLines = {upRows(p) == 1 && p.cursor == 0 ? QStringLiteral("Up to the folder this one is in.") : QStringLiteral("Nothing to preview.")};
        return;
    }
    const QModelIndex index = p.model->indexForPath(path);
    m_previewTitle = QFileInfo(path).fileName();
    const bool dir = index.isValid() && p.model->isDir(index);
    m_previewLoading = true;
    // A picture has two "pixels" to the cell; asked for a few times that,
    // so it can be made smaller smoothly.
    const QSize box(qMax(40, m_previewWidth) * 4, qMax(20, m_lastHeight) * 8);
    // What it really is, looked up here: the cache of types is this thread's.
    QString mime = QStringLiteral("inode/directory");
    ContentType::Verdict verdict = ContentType::Verdict::NotChecked;
    if (!dir) {
        const ContentType::Result type = typeOf(path);
        verdict = type.verdict;
        mime = ContentType::effectiveMime(type);
    }
    QPointer<App> self(this);
    QThreadPool::globalInstance()->start([self, request, path, dir, box, mime, verdict] {
        const QFileInfo info(path);
        const Preview::Result result = Preview::load(path, mime, verdict, box, nullptr);
        QStringList details;
        details << (dir ? QStringLiteral("Folder") : QMimeDatabase().mimeTypeForName(mime).comment());
        if (!dir)
            details << formatSize(info.size());
        if (result.kind == Preview::Kind::Image) {
            const QSize size = QImageReader(path).size();
            if (size.isValid())
                details << QStringLiteral("%1 × %2").arg(size.width()).arg(size.height());
        }
        details.removeAll(QString());
        QStringList lines;
        bool warning = false;
        switch (result.kind) {
        case Preview::Kind::Text:
            lines = result.text.split(QLatin1Char('\n'));
            if (result.truncated)
                lines << QString() << QStringLiteral("… (the start of the file; Enter opens all of it)");
            break;
        case Preview::Kind::Archive:
            for (const FileJob::ArchiveEntry &entry : result.entries)
                lines << (entry.isDir ? entry.path + QLatin1Char('/') : QStringLiteral("%1   %2").arg(entry.path, formatSize(entry.size)));
            if (result.truncated)
                lines << QString() << QStringLiteral("… and %1 more").arg(result.total - int(result.entries.size()));
            details << (result.total == 1 ? QStringLiteral("1 item inside") : QStringLiteral("%1 items inside").arg(result.total));
            break;
        case Preview::Kind::Folder:
            lines << (result.items < 0 ? QStringLiteral("A folder.")
                                       : result.items == 1 ? QStringLiteral("1 item inside.") : QStringLiteral("%1 items inside.").arg(result.items));
            break;
        case Preview::Kind::Blocked:
            warning = true;
            lines << QStringLiteral("This is a program disguised as a document.") << QString()
                  << QStringLiteral("Argonaut doesn't preview it and won't open it.");
            break;
        case Preview::Kind::Image:
            break;
        case Preview::Kind::Generic:
            lines << QStringLiteral("No preview for this kind of file.");
            break;
        }
        const QImage image = result.kind == Preview::Kind::Image ? result.image.convertToFormat(QImage::Format_RGB32) : QImage();
        QMetaObject::invokeMethod(qApp, [self, request, details, lines, warning, image] {
            if (!self || request != self->m_previewRequest)
                return; // the cursor has moved on
            self->m_previewLoading = false;
            self->m_previewInfo = details.join(QStringLiteral(" · "));
            self->m_previewLines = lines;
            self->m_previewWarning = warning;
            self->m_previewImage = image;
            self->m_previewWrapWidth = -1;
            emit self->changed();
        }, Qt::QueuedConnection);
    });
}

namespace {

// Text with colour codes (ESC [ … m, the usual ones: bold, underline,
// reverse, the 16 and the 256 colours, and full colour as the nearest of
// 256) as cells. Every other escape sequence is dropped: an extension
// colours its text, it doesn't drive the terminal.
QList<QList<Cell>> cellsOf(const QString &text)
{
    QList<QList<Cell>> lines;
    QList<Cell> line;
    Style style;
    style.raw = true;
    const QList<uint> points = text.toUcs4();
    for (qsizetype i = 0; i < points.size(); ++i) {
        const uint c = points.at(i);
        if (c == '\n') {
            lines.append(line);
            line.clear();
            continue;
        }
        if (c == 0x1b) {
            if (i + 1 >= points.size())
                continue;
            if (points.at(i + 1) == ']' || points.at(i + 1) == 'P' || points.at(i + 1) == '_' || points.at(i + 1) == '^') {
                // A string for the terminal (a window title…): all of it
                // goes, up to its end (BEL, or ESC and a backslash).
                qsizetype end = i + 2;
                while (end < points.size() && points.at(end) != 0x07 && !(points.at(end) == 0x1b && end + 1 < points.size() && points.at(end + 1) == '\\'))
                    ++end;
                i = end < points.size() && points.at(end) == 0x1b ? end + 1 : end;
                continue;
            }
            if (points.at(i + 1) != '[') {
                ++i; // a two-character sequence: both go
                continue;
            }
            qsizetype end = i + 2;
            while (end < points.size() && !(points.at(end) >= 0x40 && points.at(end) <= 0x7e))
                ++end;
            if (end < points.size() && points.at(end) == 'm') {
                QList<int> codes;
                int value = 0;
                for (qsizetype k = i + 2; k <= end; ++k) {
                    const uint d = points.at(k);
                    if (d >= '0' && d <= '9') {
                        value = qMin(value * 10 + int(d - '0'), 9999);
                    } else {
                        codes.append(value);
                        value = 0;
                    }
                }
                for (qsizetype k = 0; k < codes.size(); ++k) {
                    const int code = codes.at(k);
                    const bool fore = code == 38;
                    if (code == 0) {
                        style = Style();
                        style.raw = true;
                    } else if (code == 1) {
                        style.bold = true;
                    } else if (code == 22) {
                        style.bold = false;
                    } else if (code == 4) {
                        style.underline = true;
                    } else if (code == 24) {
                        style.underline = false;
                    } else if (code == 7) {
                        style.reverse = true;
                    } else if (code == 27) {
                        style.reverse = false;
                    } else if (code >= 30 && code <= 37) {
                        style.fg = qint16(code - 30);
                    } else if (code >= 90 && code <= 97) {
                        style.fg = qint16(code - 90 + 8);
                    } else if (code == 39) {
                        style.fg = -1;
                    } else if (code >= 40 && code <= 47) {
                        style.bg = qint16(code - 40);
                    } else if (code >= 100 && code <= 107) {
                        style.bg = qint16(code - 100 + 8);
                    } else if (code == 49) {
                        style.bg = -1;
                    } else if ((code == 38 || code == 48) && k + 2 < codes.size() && codes.at(k + 1) == 5) {
                        (fore ? style.fg : style.bg) = qint16(qBound(0, codes.at(k + 2), 255));
                        k += 2;
                    } else if ((code == 38 || code == 48) && k + 4 < codes.size() && codes.at(k + 1) == 2) {
                        (fore ? style.fg : style.bg) = nearest256(qRgb(qBound(0, codes.at(k + 2), 255), qBound(0, codes.at(k + 3), 255),
                                                                         qBound(0, codes.at(k + 4), 255)));
                        k += 4;
                    }
                }
            }
            i = end;
            continue;
        }
        if (c < 0x20 || c == 0x7f)
            continue; // no other control characters
        Cell cell;
        cell.ch = char32_t(c);
        cell.style = style;
        line.append(cell);
        if (line.size() >= 400)
            continue;
    }
    lines.append(line);
    return lines;
}

} // namespace

// What an extension wants shown: in the preview, which opens for it. A
// result with a name replaces itself while it is shown (an "update" for
// anything else does nothing), and its extension hears how it fares.
void App::showResult(const QString &title, const Extensions::PreviewReply &content)
{
    using Extensions::PreviewReply;
    const bool same = m_resultShown && !content.resultId.isEmpty() && content.resultId == m_resultId
                      && content.extension == m_resultExtension && m_preview;
    if (content.update && !same)
        return; // for a result that isn't shown (any more)
    if (!same) {
        closeResult();
        ++m_previewRequest; // a file's preview still on its way is dropped
        m_preview = true;
        m_previewTop = 0;
        m_resultShown = true;
        m_previewPath = QStringLiteral("\x01result"); // not a file's: the next file loads afresh
    }
    m_previewLoading = false;
    m_previewTitle = title;
    m_previewInfo = content.note.isEmpty() ? QStringLiteral("F3 closes it") : content.note + QStringLiteral(" · F3 closes it");
    m_previewWarning = false;
    m_previewLines.clear();
    m_previewCells.clear();
    m_previewImage = QImage();
    m_previewWrapWidth = -1;
    switch (content.kind) {
    case PreviewReply::Ansi:
        m_previewCells = cellsOf(content.text.left(400000));
        break;
    case PreviewReply::Image: {
        QImageReader reader(content.image);
        const QSize size = reader.size();
        if (size.isValid() && size.width() <= 16384 && size.height() <= 16384)
            m_previewImage = reader.read().convertToFormat(QImage::Format_RGB32);
        if (m_previewImage.isNull())
            m_previewLines = {QStringLiteral("Nothing to show.")};
        break;
    }
    case PreviewReply::Html: {
        QTextDocument document;
        document.setHtml(content.text);
        m_previewLines = document.toPlainText().split(QLatin1Char('\n'));
        break;
    }
    case PreviewReply::Diff: {
        // The added lines green, the removed ones red, as text with codes.
        QString coloured;
        const QStringList lines = content.text.split(QLatin1Char('\n'));
        for (const QString &line : lines) {
            const int colour = line.startsWith(QLatin1String("+++")) || line.startsWith(QLatin1String("---")) ? -1
                               : line.startsWith(QLatin1Char('+')) ? 2 : line.startsWith(QLatin1Char('-')) ? 1
                               : line.startsWith(QLatin1String("@@")) ? 6 : -1;
            QString plain = line;
            plain.remove(QChar(0x1b)); // a diff is text: no codes of its own
            coloured += (colour < 0 ? QStringLiteral("\x1b[0m") : QStringLiteral("\x1b[0;3%1m").arg(colour)) + plain + QLatin1Char('\n');
        }
        m_previewCells = cellsOf(coloured);
        break;
    }
    case PreviewReply::Text:
    case PreviewReply::Markdown:
        m_previewLines = content.text.split(QLatin1Char('\n'));
        break;
    case PreviewReply::None:
        m_previewLines = {QStringLiteral("Nothing to show.")};
        break;
    }
    if (!same && !content.resultId.isEmpty()) {
        m_resultExtension = content.extension;
        m_resultId = content.resultId;
        m_resultRoom = QSize(); // told once the pane has been laid out
        m_resultSearch.clear();
    }
    if (!same) { // said when the result comes; making it again doesn't take the search away
        m_resultSearchable = content.searchable && !content.resultId.isEmpty();
        m_resultSearchHint = content.searchHint;
        // What can be done with it (the window's button under the result): Enter here.
        m_resultActionId = content.resultId.isEmpty() ? QString() : content.actionId;
        m_resultActionLabel = content.actionLabel;
        m_resultActionHint = content.actionHint;
        m_resultActionInput = content.actionInput;
    }
    emit changed();
}

void App::closeResult()
{
    if (!m_resultShown)
        return;
    m_resultShown = false;
    m_previewCells.clear();
    m_previewPath.clear();
    if (!m_resultId.isEmpty())
        Extensions::Host::instance()->resultShown(m_resultExtension, m_resultId, false);
    m_resultExtension.clear();
    m_resultId.clear();
    m_resultRoom = QSize();
    m_resultSearchable = false;
    m_resultSearch.clear();
    m_resultActionId.clear();
}

// Enter in the preview of a result that offers something to do with it: a
// line may be typed to go with it (nothing typed is an answer too), then its
// extension is told, as the window's button does.
void App::runResultAction()
{
    if (!m_resultShown || m_resultActionId.isEmpty())
        return;
    const QString extension = m_resultExtension, id = m_resultId, action = m_resultActionId;
    auto run = [this, extension, id, action](const QString &text) {
        if (!m_resultShown || m_resultExtension != extension || m_resultId != id)
            return; // it went while the text was typed
        Extensions::Host::instance()->resultAction(extension, id, action, text.trimmed());
    };
    if (!m_resultActionInput) {
        run(QString());
        return;
    }
    prompt(QStringLiteral("%1 — %2").arg(m_resultActionLabel, m_resultActionHint.isEmpty() ? QStringLiteral("anything to add?") : m_resultActionHint) + QLatin1Char(':'),
           QString(), -1, run, true);
}

// The named result's extension is told the room there is, in characters
// (again when that changes): it draws for it.
void App::reportResult()
{
    if (m_resultId.isEmpty() || m_previewWidth <= 0)
        return;
    const QSize room(qMax(1, m_previewWidth - 3), qMax(1, m_previewShown));
    if (room == m_resultRoom)
        return;
    m_resultRoom = room;
    Extensions::ResultLook look;
    look.dark = true; // unknown: most terminals are, and text keeps the terminal's own colours anyway
    look.columns = room.width();
    look.rows = room.height();
    Extensions::Host::instance()->resultShown(m_resultExtension, m_resultId, true, QSize(), look);
}

// A command an extension put in the menu bar of the window: here it is in
// the command palette. What it wants asked first is asked here too.
void App::runExtensionCommand(const Extensions::Command &command, const QStringList &selection)
{
    const Pane &p = pane();
    const QString folder = p.model->extensionsFolder();
    auto invoke = [this, command, folder, selection](const QString &input) {
        Extensions::Host::instance()->invoke(command, folder, selection, input);
    };
    auto typed = [this, command, invoke] {
        if (!command.asks) {
            invoke(QString());
            return;
        }
        prompt(command.askLabel.isEmpty() ? command.label : command.askLabel, command.askText, int(command.askText.size()),
               [invoke](const QString &text) {
                   if (!text.trimmed().isEmpty())
                       invoke(text);
               });
    };
    if (!command.confirm.isEmpty())
        ask({command.label, command.confirm, QString(), command.label}, typed);
    else
        typed();
}

// The preview pane: the item's name, what it is, then its content with a
// scrollbar: text as lines (long ones go on to the next), an archive's
// entries, or a picture made of half blocks, two "pixels" to the cell.
void App::drawPreview(Screen &screen, int x, int width, int top, int height)
{
    m_previewX = x;
    for (int y = top; y < top + height; ++y)
        screen.put(x, y, QStringLiteral("│"), kDim);
    const int inner = qMax(1, width - 3); // the line at the left, a margin, the scrollbar
    const Style heading{m_inPreview ? Ink::Accent : Ink::Normal, Paper::Normal, true};
    screen.put(x + 2, top, Screen::elided(m_previewTitle, inner), heading);
    screen.put(x + 2, top + 1, Screen::elided(m_previewLoading ? QStringLiteral("Loading…") : m_previewInfo, inner, false), kDim);
    // A result that offers something to do with it (Analyze with Claude):
    // a line of its own says so, above the last line, where it can't be
    // missed or cut off. With the keys here it is lit and says "Enter";
    // from the files it says how to get here. A click on it does it.
    const bool offer = m_resultShown && !m_resultActionId.isEmpty() && height >= 9;
    m_previewActionY = -1;
    if (offer) {
        m_previewActionY = top + height - 2;
        const Style chip{Ink::Normal, m_inPreview ? Paper::Cursor : Paper::Bar, true};
        const QString key = m_inPreview ? QStringLiteral(" Enter ") : QStringLiteral(" Tab, then Enter ");
        int at = x + 2;
        at += screen.put(at, m_previewActionY, key, chip, inner);
        screen.put(at + 1, m_previewActionY, m_resultActionLabel, {m_inPreview ? Ink::Accent : Ink::Dim, Paper::Normal, m_inPreview},
                   qMax(0, inner - (at + 1 - (x + 2))));
    }
    const int rows = qMax(1, height - 4 - (offer ? 2 : 0)); // name, details, a gap; a line at the bottom (and the offer's, with a gap)
    const int contentY = top + 3;
    m_previewY = contentY;
    m_previewShown = rows;
    m_previewTotal = 0;

    if (!m_previewImage.isNull()) {
        // Every cell is "▀": the upper half in the text colour, the lower
        // in the background's. Coarse, but it is the picture.
        const QImage small = m_previewImage.scaled(inner, rows * 2, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        const int offsetX = (inner - small.width()) / 2;
        for (int cy = 0; cy * 2 < small.height() && cy < rows; ++cy) {
            for (int cx = 0; cx < small.width(); ++cx) {
                Cell cell;
                cell.ch = 0x2580;
                cell.style.raw = true;
                cell.style.fg = nearest256(small.pixel(cx, cy * 2));
                cell.style.bg = cy * 2 + 1 < small.height() ? nearest256(small.pixel(cx, cy * 2 + 1)) : -1;
                screen.set(x + 2 + offsetX + cx, contentY + cy, cell);
            }
        }
        screen.put(x + 2, top + height - 1, QStringLiteral("a rough picture: Enter opens the real one"), kDim, inner);
        return;
    }
    if (!m_previewCells.isEmpty()) {
        // Text with its own colours: a line a line, cut at the pane's edge.
        const int count = int(m_previewCells.size());
        m_previewTotal = count;
        m_previewTop = qBound(0, m_previewTop, qMax(0, count - rows));
        for (int i = 0; i < rows && m_previewTop + i < count; ++i) {
            const QList<Cell> &line = m_previewCells.at(m_previewTop + i);
            int cx = 0;
            for (const Cell &cell : line) {
                const int cells = Screen::widthOf(QString::fromUcs4(&cell.ch, 1));
                if (cx + cells > inner)
                    break;
                if (cells > 0)
                    screen.set(x + 2 + cx, contentY + i, cell);
                if (cells == 2) {
                    Cell tail = cell;
                    tail.ch = U' ';
                    tail.tail = true;
                    screen.set(x + 2 + cx + 1, contentY + i, tail);
                }
                cx += cells;
            }
        }
        if (count > rows) {
            const int thumb = qBound(1, rows * rows / count, rows - 1);
            const int at = (rows - thumb) * m_previewTop / (count - rows);
            for (int i = 0; i < rows; ++i) {
                const bool on = i >= at && i < at + thumb;
                screen.put(x + width - 1, contentY + i, on ? QStringLiteral("█") : QStringLiteral("│"),
                           {on ? (m_inPreview ? Ink::Accent : Ink::Normal) : Ink::Dim, Paper::Normal, false});
            }
        }
        reportResult();
        return;
    }
    if (m_resultShown)
        reportResult();
    if (m_previewWrapWidth != inner) {
        m_previewWrapped.clear();
        for (const QString &line : std::as_const(m_previewLines))
            wrapInto(&m_previewWrapped, line, inner);
        m_previewWrapWidth = inner;
    }
    const int count = int(m_previewWrapped.size());
    m_previewTotal = count;
    m_previewTop = qBound(0, m_previewTop, qMax(0, count - rows));
    const Style text{m_previewWarning ? Ink::Bad : Ink::Normal, Paper::Normal, m_previewWarning};
    for (int i = 0; i < rows && m_previewTop + i < count; ++i)
        screen.put(x + 2, contentY + i, m_previewWrapped.at(m_previewTop + i), text, inner);
    if (count > rows) {
        // The scrollbar, and where we are.
        const int thumb = qBound(1, rows * rows / count, rows - 1);
        const int at = (rows - thumb) * m_previewTop / (count - rows);
        for (int i = 0; i < rows; ++i) {
            const bool on = i >= at && i < at + thumb;
            screen.put(x + width - 1, contentY + i, on ? QStringLiteral("█") : QStringLiteral("│"),
                       {on ? (m_inPreview ? Ink::Accent : Ink::Normal) : Ink::Dim, Paper::Normal, false});
        }
        screen.put(x + 2, top + height - 1, QStringLiteral("lines %1–%2 of %3").arg(m_previewTop + 1).arg(qMin(count, m_previewTop + rows)).arg(count)
                                                + (m_resultShown && m_resultSearchable ? QStringLiteral(" · / search") : QString())
                                                + (m_resultShown && !m_resultActionId.isEmpty() && !offer ? QStringLiteral(" · Enter: ") + m_resultActionLabel : QString()),
                   kDim, inner);
    } else if (m_resultShown && (m_resultSearchable || (!m_resultActionId.isEmpty() && !offer))) {
        QStringList hints;
        if (m_resultSearchable)
            hints << QStringLiteral("/ search");
        if (!m_resultActionId.isEmpty() && !offer) // (a pane too low for the line of its own)
            hints << QStringLiteral("Enter: ") + m_resultActionLabel;
        screen.put(x + 2, top + height - 1, hints.join(QStringLiteral(" · ")), kDim, inner);
    }
}

bool App::previewKey(const Key &key)
{
    const int page = qMax(1, m_previewShown - 1);
    int top = m_previewTop;
    if (scrollKey(key, page, m_previewTotal, &top)) {
        scrollPreview(top); // it knows how far there is to go
        return true;
    }
    switch (key.code) {
    case Key::Enter:
        if (m_resultShown && !m_resultActionId.isEmpty()) {
            runResultAction();
            return true;
        }
        break;
    case Key::Left:
    case Key::Escape:
    case Key::Backspace:
        m_inPreview = false; // back to the files
        emit changed();
        return true;
    case Key::Tab:
    case Key::F1:
    case Key::F3:
    case Key::F4:
    case Key::F10:
        return false; // these mean the same everywhere
    case Key::Char:
        if (key.isChar(' ')) { // a page on, as in a pager
            scrollPreview(m_previewTop + page);
            return true;
        }
        if (key.isChar('/') && m_resultShown && m_resultSearchable) {
            // A result that can be searched (a list of processes): what is
            // typed goes to its extension, which makes the list again.
            // Nothing typed: everything again.
            const QString extension = m_resultExtension, id = m_resultId;
            prompt(m_resultSearchHint.isEmpty() ? QStringLiteral("Search") : m_resultSearchHint, m_resultSearch, int(m_resultSearch.size()),
                   [this, extension, id](const QString &text) {
                       if (!m_resultShown || m_resultExtension != extension || m_resultId != id)
                           return; // it went while the text was typed
                       m_resultSearch = text.trimmed();
                       m_previewTop = 0;
                       Extensions::Host::instance()->searchResult(extension, id, m_resultSearch);
                   },
                   true);
            return true;
        }
        return !(key.isControl('q') || key.isControl('b'));
    default:
        return true;
    }
    return true; // (Enter with nothing to do)
}

} // namespace Tui
