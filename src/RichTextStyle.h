#pragma once

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

} // namespace RichTextStyle
