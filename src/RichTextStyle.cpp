#include "RichTextStyle.h"

#include "Theme.h"

#include <QFontDatabase>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextList>
#include <QTextTable>

namespace {

QColor mix(const QColor &a, const QColor &b, qreal share)
{
    return QColor::fromRgbF(a.redF() * (1 - share) + b.redF() * share, a.greenF() * (1 - share) + b.greenF() * share,
                            a.blueF() * (1 - share) + b.blueF() * share);
}

// A code block: Qt marks fenced ones; indented ones are blocks whose whole
// text is in a fixed-width font.
bool isCodeBlock(const QTextBlock &block)
{
    const QTextBlockFormat format = block.blockFormat();
    if (format.hasProperty(QTextFormat::BlockCodeFence) || format.hasProperty(QTextFormat::BlockCodeLanguage))
        return true;
    bool any = false;
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        if (!it.fragment().charFormat().fontFixedPitch())
            return false;
        any = true;
    }
    return any;
}

void styleTables(QTextFrame *frame, const QColor &line, const QColor &header)
{
    for (auto it = frame->begin(); !it.atEnd(); ++it) {
        QTextFrame *child = it.currentFrame();
        if (!child)
            continue;
        if (auto *table = qobject_cast<QTextTable *>(child)) {
            QTextTableFormat format = table->format();
            format.setBorder(1);
            format.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
            format.setBorderBrush(line);
            format.setBorderCollapse(true);
            format.setCellSpacing(0);
            format.setCellPadding(6);
            format.setTopMargin(6);
            format.setBottomMargin(12);
            table->setFormat(format);
            for (int column = 0; column < table->columns(); ++column) {
                QTextTableCell cell = table->cellAt(0, column);
                QTextCharFormat cellFormat = cell.format();
                cellFormat.setBackground(header);
                cell.setFormat(cellFormat);
                QTextCursor text = cell.firstCursorPosition();
                text.setPosition(cell.lastCursorPosition().position(), QTextCursor::KeepAnchor);
                QTextCharFormat bold;
                bold.setFontWeight(QFont::Bold);
                text.mergeCharFormat(bold);
            }
        }
        styleTables(child, line, header);
    }
}

} // namespace

namespace RichTextStyle {

void apply(QTextDocument *document, const Theme &theme)
{
    // Colours without transparency: rich text doesn't blend.
    const QColor page = theme.dark ? mix(theme.window, Qt::white, 0.06) : mix(theme.window, Qt::black, 0.05);
    const QColor codeBackground = mix(page, theme.fg, 0.07);
    const QColor line = mix(page, theme.fg, 0.22);
    const QColor quoteText = mix(theme.fg, page, 0.35);
    const QFont base = document->defaultFont();
    const qreal basePoints = base.pointSizeF() > 0 ? base.pointSizeF() : 10;
    const QStringList fixedFamilies = QFontDatabase::systemFont(QFontDatabase::FixedFont).families();

    document->setDocumentMargin(16);
    QTextCursor edit(document);
    edit.beginEditBlock();

    // What to change is collected first: changing formats while walking the
    // fragments would move them under our feet.
    struct CharChange
    {
        int from, to;
        QTextCharFormat format;
        bool replace = false; // the whole format, not only what it sets
    };
    QList<CharChange> charChanges;
    QList<QPair<int, QTextBlockFormat>> blockChanges;

    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        QTextBlockFormat format = block.blockFormat();
        const int from = block.position();
        const int to = from + block.length() - 1;
        const int heading = format.headingLevel();
        const bool code = heading == 0 && isCodeBlock(block);
        const bool quote = format.intProperty(QTextFormat::BlockQuoteLevel) > 0;

        if (heading > 0) {
            static const qreal scale[] = {1.75, 1.42, 1.2, 1.08, 1.0, 1.0};
            // Qt sizes headings with a relative "adjustment" that wins over a
            // point size, so each piece gets a whole new format without it.
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                const QTextFragment fragment = it.fragment();
                QTextCharFormat text = fragment.charFormat();
                text.clearProperty(QTextFormat::FontSizeAdjustment);
                text.setFontPointSize(basePoints * scale[qBound(1, heading, 6) - 1]);
                text.setFontWeight(QFont::Bold);
                charChanges.append({fragment.position(), fragment.position() + fragment.length(), text, true});
            }
            format.setTopMargin(heading <= 2 ? 20 : 14);
            format.setBottomMargin(heading <= 2 ? 10 : 6);
            format.setLineHeight(115, QTextBlockFormat::ProportionalHeight);
        } else if (code) {
            QTextCharFormat text;
            text.setFontFamilies(fixedFamilies);
            text.setFontFixedPitch(true);
            text.setFontPointSize(basePoints * 0.93);
            charChanges.append({from, to, text});
            format.setBackground(codeBackground);
            format.setNonBreakableLines(false); // wrap: no sideways scrolling in a narrow pane
            format.setLineHeight(0, QTextBlockFormat::SingleHeight); // lines touch: one box, no stripes
            // One tinted box for the whole run of code lines.
            const bool first = !block.previous().isValid() || !isCodeBlock(block.previous());
            const bool last = !block.next().isValid() || !isCodeBlock(block.next());
            format.setTopMargin(first ? 6 : 0);
            format.setBottomMargin(last ? 12 : 0);
            format.setLeftMargin(qMax<qreal>(format.leftMargin(), 0));
        } else {
            format.setLineHeight(130, QTextBlockFormat::ProportionalHeight);
            if (block.textList()) {
                format.setTopMargin(2);
                format.setBottomMargin(4);
            } else {
                format.setTopMargin(0);
                format.setBottomMargin(10);
            }
            if (quote) {
                QTextCharFormat text;
                text.setForeground(quoteText);
                charChanges.append({from, to, text});
                format.setLeftMargin(18 * format.intProperty(QTextFormat::BlockQuoteLevel));
                format.setRightMargin(12);
            }
        }
        blockChanges.append({from, format});

        // Inside the block: links, and code written in the middle of a line.
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            const QTextCharFormat has = fragment.charFormat();
            QTextCharFormat text;
            if (has.isAnchor()) {
                text.setForeground(theme.accent);
                text.setFontUnderline(true);
            } else if (!code && has.fontFixedPitch()) {
                text.setFontFamilies(fixedFamilies);
                text.setFontPointSize(basePoints * 0.93);
                text.setBackground(codeBackground);
            } else {
                continue;
            }
            charChanges.append({fragment.position(), fragment.position() + fragment.length(), text});
        }
    }

    for (const auto &[position, format] : std::as_const(blockChanges)) {
        QTextCursor cursor(document);
        cursor.setPosition(position);
        cursor.setBlockFormat(format);
    }
    for (const CharChange &change : std::as_const(charChanges)) {
        QTextCursor cursor(document);
        cursor.setPosition(change.from);
        cursor.setPosition(change.to, QTextCursor::KeepAnchor);
        if (change.replace)
            cursor.setCharFormat(change.format);
        else
            cursor.mergeCharFormat(change.format);
    }
    styleTables(document->rootFrame(), line, mix(page, theme.fg, 0.06));
    edit.endEditBlock();
}

void setDiff(QTextDocument *document, const QString &diff, const Theme &theme)
{
    const QColor page = theme.dark ? mix(theme.window, Qt::white, 0.06) : mix(theme.window, Qt::black, 0.05);
    const QColor added = mix(page, QColor(theme.dark ? "#57e389" : "#26a269"), theme.dark ? 0.22 : 0.20);
    const QColor removed = mix(page, QColor(theme.dark ? "#f66151" : "#c01c28"), theme.dark ? 0.24 : 0.16);
    const QColor position = mix(page, theme.accent, 0.16);
    const QColor dim = mix(theme.fg, page, 0.4);
    QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    const qreal basePoints = document->defaultFont().pointSizeF() > 0 ? document->defaultFont().pointSizeF() : 10;
    fixed.setPointSizeF(basePoints * 0.93);

    document->clear();
    document->setDocumentMargin(12);
    QTextCursor cursor(document);
    cursor.beginEditBlock();
    const QList<QStringView> lines = QStringView(diff).split(QLatin1Char('\n'));
    bool first = true;
    bool inHunk = false;
    for (QStringView line : lines) {
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        QTextBlockFormat block;
        QTextCharFormat text;
        text.setFont(fixed);
        text.setForeground(theme.fg);
        if (line.startsWith(QLatin1String("diff ")) || (!inHunk && line.startsWith(QLatin1String("# ")))) {
            // A new file: its name, with room above.
            inHunk = false;
            text.setFontWeight(QFont::Bold);
            block.setTopMargin(first ? 0 : 14);
        } else if (line.startsWith(QLatin1String("@@"))) {
            inHunk = true;
            block.setBackground(position);
            block.setTopMargin(6);
            text.setForeground(dim);
        } else if (!inHunk) {
            text.setForeground(dim); // index, ---, +++, mode lines
        } else if (line.startsWith(QLatin1Char('+'))) {
            block.setBackground(added);
        } else if (line.startsWith(QLatin1Char('-'))) {
            block.setBackground(removed);
        } else if (line.startsWith(QLatin1Char('\\'))) {
            text.setForeground(dim); // "\ No newline at end of file"
        }
        if (first) {
            cursor.setBlockFormat(block);
            cursor.setBlockCharFormat(text);
        } else {
            cursor.insertBlock(block, text);
        }
        cursor.insertText(line.toString(), text);
        first = false;
    }
    cursor.endEditBlock();
}

} // namespace RichTextStyle
