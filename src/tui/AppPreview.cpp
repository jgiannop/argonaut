// The preview pane of the terminal interface (Tui::App): what is in the
// item under the cursor (F3), and what an extension shows there (a
// dashboard, a list). A part of App in a file of its own.

#include "App.h"

#include "AppInternal.h"

#include "ContentType.h"
#include "DirectoryModel.h"
#include "FileClipboard.h"
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
    forgetPreviewSelection();
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
        forgetPreviewSelection();
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
        findInPreview(false);
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

// ------------------------------------------- finding and copying text ---

// The preview shows rows of text (a file, a result as text or with
// colours): those can be searched and copied. A picture can't.
bool App::previewIsText() const
{
    return m_preview && m_previewImage.isNull() && !m_previewLoading && !m_previewWarning;
}

int App::previewRowCount() const
{
    return int(m_previewCells.isEmpty() ? m_previewWrapped.size() : m_previewCells.size());
}

QString App::previewRow(int row) const
{
    if (m_previewCells.isEmpty())
        return m_previewWrapped.value(row);
    QString text;
    if (row >= 0 && row < m_previewCells.size()) {
        for (const Cell &cell : m_previewCells.at(row)) {
            if (!cell.tail)
                text += QString::fromUcs4(&cell.ch, 1);
        }
    }
    return text;
}

// Every place the text is in the rows, ignoring case. A word cut where a
// long line goes on in the next row isn't found (the rows are what is shown).
void App::findInPreview(bool goToFirst)
{
    m_previewFound.clear();
    if (m_previewFind.isEmpty() || !previewIsText()) {
        m_previewMatch = -1;
        return;
    }
    const int count = previewRowCount();
    for (int row = 0; row < count && m_previewFound.size() < 100000; ++row) {
        const QString text = previewRow(row);
        for (qsizetype at = text.indexOf(m_previewFind, 0, Qt::CaseInsensitive); at >= 0;
             at = text.indexOf(m_previewFind, at + m_previewFind.size(), Qt::CaseInsensitive))
            m_previewFound.append({row, int(at), int(m_previewFind.size())});
    }
    m_previewMatch = qMin(m_previewMatch, int(m_previewFound.size()) - 1);
    if (goToFirst && !m_previewFound.isEmpty()) {
        int first = 0; // the first from the top of what is shown, so the text doesn't jump back to its start
        while (first < m_previewFound.size() && m_previewFound.at(first).row < m_previewTop)
            ++first;
        showFound(first < m_previewFound.size() ? first : 0);
    }
}

void App::showFound(int index)
{
    m_previewMatch = index;
    const int row = m_previewFound.at(index).row;
    if (row < m_previewTop || row >= m_previewTop + m_previewShown)
        scrollPreview(row - m_previewShown / 3); // with a little of what comes before
    emit changed();
}

// Ctrl+F or "/": what to find, typed at the bottom (what was found last is there to change).
void App::findPrompt()
{
    prompt(QStringLiteral("Find in the text"), m_previewFind, int(m_previewFind.size()),
           [this](const QString &text) {
               m_previewFind = text;
               m_previewMatch = -1;
               findInPreview(true);
               if (!m_previewFind.isEmpty() && m_previewFound.isEmpty())
                   say(QStringLiteral("“%1” isn't in the text").arg(m_previewFind));
               emit changed();
           },
           true);
}

QString App::previewFindText() const
{
    if (m_previewFind.isEmpty())
        return {};
    if (m_previewFound.isEmpty())
        return QStringLiteral("no matches");
    return QStringLiteral("%1 of %2").arg(m_previewMatch + 1).arg(m_previewFound.size());
}

void App::forgetPreviewSelection()
{
    m_previewSelAnchor = m_previewSelEnd = -1;
    m_previewDragFrom = -1;
    m_previewDragging = false;
}

// The selected rows as the text they are: the rows of one line of a file
// are joined again, and a line that is selected whole is the file's own
// (tabs and all).
QString App::previewSelectedText() const
{
    if (m_previewSelAnchor < 0 || !previewIsText())
        return {};
    const int count = previewRowCount();
    const int from = qBound(0, qMin(m_previewSelAnchor, m_previewSelEnd), count - 1);
    const int to = qBound(0, qMax(m_previewSelAnchor, m_previewSelEnd), count - 1);
    QStringList lines;
    if (!m_previewCells.isEmpty()) {
        for (int row = from; row <= to; ++row)
            lines << previewRow(row);
        return lines.join(QLatin1Char('\n'));
    }
    int last = -1;
    for (int row = from; row <= to; ++row) {
        const int source = m_previewSource.value(row, -1);
        if (source == last) {
            continue; // a whole line was taken at its first row; a part is added below
        }
        int first = row, end = row;
        while (first > 0 && m_previewSource.value(first - 1) == source)
            --first;
        while (end + 1 < m_previewSource.size() && m_previewSource.value(end + 1) == source)
            ++end;
        if (first >= from && end <= to) {
            lines << m_previewLines.value(source);
        } else {
            QString part;
            for (int r = qMax(first, from); r <= qMin(end, to); ++r)
                part += m_previewWrapped.value(r);
            lines << part;
        }
        last = source;
    }
    return lines.join(QLatin1Char('\n'));
}

// Ctrl+C: the selected lines to the desktop's clipboard, or, where there is
// none (over SSH), to the terminal's.
void App::copyPreviewSelection()
{
    const QString text = previewSelectedText();
    if (m_previewSelAnchor < 0) {
        say(QStringLiteral("Select lines first: drag over them, Shift+↑/↓, or Ctrl+A for all"));
        return;
    }
    const int lines = qAbs(m_previewSelEnd - m_previewSelAnchor) + 1;
    const QString what = lines == 1 ? QStringLiteral("1 line") : QStringLiteral("%1 lines").arg(lines);
    if (FileClipboard::copyTextToDesktop(text)) {
        say(QStringLiteral("Copied %1").arg(what));
    } else {
        emit terminalClipboardRequested(text);
        say(QStringLiteral("Copied %1 through the terminal (where it allows it)").arg(what));
    }
}

// A drag over the rows selects whole lines; past the top or bottom it scrolls.
void App::previewDragTo(int y)
{
    const int count = previewRowCount();
    if (m_previewDragFrom < 0 || count == 0)
        return;
    int row;
    if (y < m_previewY) {
        scrollPreview(m_previewTop - 1);
        row = m_previewTop;
    } else if (y >= m_previewY + m_previewShown) {
        scrollPreview(m_previewTop + 1);
        row = m_previewTop + m_previewShown - 1;
    } else {
        row = m_previewTop + (y - m_previewY);
    }
    row = qBound(0, row, count - 1);
    if (m_previewSelAnchor < 0 && row == m_previewDragFrom)
        return; // not moved off the line yet
    m_previewSelAnchor = m_previewDragFrom;
    m_previewSelEnd = row;
    emit changed();
}

void App::markPreview(Screen &screen, int x, int width, int rows)
{
    if (m_previewSelAnchor >= 0) {
        const int from = qMin(m_previewSelAnchor, m_previewSelEnd), to = qMax(m_previewSelAnchor, m_previewSelEnd);
        for (int i = 0; i < rows; ++i) {
            const int row = m_previewTop + i;
            if (row >= from && row <= to)
                screen.tint(x, m_previewY + i, width, 1, Paper::Normal, Paper::Cursor);
        }
    }
    for (int i = 0; i < m_previewFound.size(); ++i) {
        const Found &found = m_previewFound.at(i);
        if (found.row < m_previewTop)
            continue;
        if (found.row >= m_previewTop + rows)
            break;
        const QString row = previewRow(found.row);
        const int column = Screen::widthOf(row.left(found.at));
        if (column >= width)
            continue;
        Style style{Ink::Normal, Paper::Field, false};
        if (i == m_previewMatch) {
            style.bold = true;
            style.reverse = true;
        }
        screen.put(x + column, m_previewY + found.row - m_previewTop, row.mid(found.at, found.length), style, width - column);
    }
}

QString App::previewMarksText() const
{
    QStringList parts;
    if (!m_previewFind.isEmpty())
        parts << (m_previewFound.isEmpty() ? QStringLiteral("no “%1”").arg(m_previewFind)
                                           : QStringLiteral("“%1” %2 · n, N").arg(m_previewFind, previewFindText()));
    if (m_previewSelAnchor >= 0) {
        const int lines = qAbs(m_previewSelEnd - m_previewSelAnchor) + 1;
        parts << (lines == 1 ? QStringLiteral("1 line") : QStringLiteral("%1 lines").arg(lines)) + QStringLiteral(" selected · Ctrl+C copies");
    }
    return parts.join(QStringLiteral(" · "));
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
        markPreview(screen, x + 2, inner, rows);
        if (const QString marks = previewMarksText(); !marks.isEmpty() && !offer)
            screen.put(x + 2, top + height - 1, marks, kDim, inner);
        reportResult();
        return;
    }
    if (m_resultShown)
        reportResult();
    if (m_previewWrapWidth != inner) {
        m_previewWrapped.clear();
        m_previewSource.clear();
        for (int i = 0; i < m_previewLines.size(); ++i) {
            wrapInto(&m_previewWrapped, m_previewLines.at(i), inner);
            while (m_previewSource.size() < m_previewWrapped.size())
                m_previewSource.append(i);
        }
        m_previewWrapWidth = inner;
        findInPreview(false); // the rows are others now
    }
    const int count = int(m_previewWrapped.size());
    m_previewTotal = count;
    m_previewTop = qBound(0, m_previewTop, qMax(0, count - rows));
    const Style text{m_previewWarning ? Ink::Bad : Ink::Normal, Paper::Normal, m_previewWarning};
    for (int i = 0; i < rows && m_previewTop + i < count; ++i)
        screen.put(x + 2, contentY + i, m_previewWrapped.at(m_previewTop + i), text, inner);
    markPreview(screen, x + 2, inner, rows);
    const QString marks = previewMarksText();
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
                                                + (m_resultShown && !m_resultActionId.isEmpty() && !offer ? QStringLiteral(" · Enter: ") + m_resultActionLabel : QString())
                                                + (marks.isEmpty() ? QString() : QStringLiteral(" · ") + marks),
                   kDim, inner);
    } else if (!marks.isEmpty() || (m_resultShown && (m_resultSearchable || (!m_resultActionId.isEmpty() && !offer)))) {
        QStringList hints;
        if (!marks.isEmpty())
            hints << marks;
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
    // Lines to copy: Shift+arrows from the first line in sight (or the
    // selection's end), Ctrl+A all of them, Ctrl+C copies.
    if (previewIsText() && key.shift && (key.code == Key::Up || key.code == Key::Down)) {
        const int count = previewRowCount();
        if (count > 0) {
            if (m_previewSelAnchor < 0) {
                m_previewSelAnchor = m_previewSelEnd = qBound(0, m_previewTop, count - 1);
            } else {
                m_previewSelEnd = qBound(0, m_previewSelEnd + (key.code == Key::Down ? 1 : -1), count - 1);
            }
            if (m_previewSelEnd < m_previewTop)
                scrollPreview(m_previewSelEnd);
            else if (m_previewSelEnd >= m_previewTop + m_previewShown)
                scrollPreview(m_previewSelEnd - m_previewShown + 1);
            emit changed();
        }
        return true;
    }
    if (key.isControl('a') && previewIsText() && previewRowCount() > 0) {
        m_previewSelAnchor = 0;
        m_previewSelEnd = previewRowCount() - 1;
        emit changed();
        return true;
    }
    if (key.isControl('c') && previewIsText()) {
        copyPreviewSelection();
        return true;
    }
    if (key.isControl('f') && previewIsText()) {
        findPrompt();
        return true;
    }
    if (!m_previewFind.isEmpty() && (key.isChar('n') || key.isChar('N'))) {
        if (m_previewFound.isEmpty())
            say(QStringLiteral("“%1” isn't in the text").arg(m_previewFind));
        else
            showFound(int((m_previewMatch + (key.isChar('n') ? 1 : -1) + m_previewFound.size()) % m_previewFound.size()));
        return true;
    }
    if (key.code == Key::Escape && (m_previewSelAnchor >= 0 || !m_previewFind.isEmpty())) {
        // The first Esc forgets the selection, then what was found; then the keys go back to the files.
        if (m_previewSelAnchor >= 0) {
            forgetPreviewSelection();
        } else {
            m_previewFind.clear();
            m_previewFound.clear();
            m_previewMatch = -1;
        }
        emit changed();
        return true;
    }
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
        if (key.isChar('/') && previewIsText()) {
            findPrompt();
            return true;
        }
        return !(key.isControl('q') || key.isControl('b'));
    default:
        return true;
    }
    return true; // (Enter with nothing to do)
}

} // namespace Tui
