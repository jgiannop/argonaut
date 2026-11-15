#pragma once

class QString;
class QTextDocument;
struct Theme;

// Gives a rich-text document (Markdown or HTML shown in the preview pane) the
// look of a page: headings with their own sizes and room, code on a tinted
// background in a fixed-width font, quotes set apart, links in the accent
// colour, tables with lines and a header row, more air between lines.
//
// Qt formats Markdown into a QTextDocument but applies no style to it, and
// its style sheets only work on HTML. So the document's own formats are
// changed here, block by block; nothing is added to or removed from its text.
namespace RichTextStyle {

void apply(QTextDocument *document, const Theme &theme);

// Fills the document with a unified diff (what `git diff` prints): added
// lines on green, removed lines on red, file and position lines set apart,
// all in a fixed-width font. The text is shown as it is, never as markup.
void setDiff(QTextDocument *document, const QString &diff, const Theme &theme);

} // namespace RichTextStyle
