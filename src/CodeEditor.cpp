#include "CodeEditor.h"

#include <QFileInfo>
#include <QFontDatabase>
#include <QHash>
#include <QVBoxLayout>

#if defined(ARGONAUT_QSCINTILLA)
#include <Qsci/qscilexer.h>
#include <Qsci/qscilexerbash.h>
#include <Qsci/qscilexerbatch.h>
#include <Qsci/qscilexercmake.h>
#include <Qsci/qscilexercpp.h>
#include <Qsci/qscilexercsharp.h>
#include <Qsci/qscilexercss.h>
#include <Qsci/qscilexerdiff.h>
#include <Qsci/qscilexerhtml.h>
#include <Qsci/qscilexerjava.h>
#include <Qsci/qscilexerjavascript.h>
#include <Qsci/qscilexerjson.h>
#include <Qsci/qscilexerlua.h>
#include <Qsci/qscilexermakefile.h>
#include <Qsci/qscilexermarkdown.h>
#include <Qsci/qscilexerperl.h>
#include <Qsci/qscilexerproperties.h>
#include <Qsci/qscilexerpython.h>
#include <Qsci/qscilexerruby.h>
#include <Qsci/qscilexersql.h>
#include <Qsci/qscilexertex.h>
#include <Qsci/qscilexerxml.h>
#include <Qsci/qscilexeryaml.h>
#include <Qsci/qsciscintilla.h>
#else
#include <QPlainTextEdit>
#include <QTextBlock>
#endif

namespace {

const QColor Paper(0xff, 0xff, 0xff);
const QColor Ink(0x1f, 0x1f, 0x1f);
const QColor MarginPaper(0xf6, 0xf5, 0xf4); // libadwaita's light window colour
const QColor MarginInk(0x9a, 0x99, 0x96);
constexpr int MarkIndicator = 8;    // INDIC_CONTAINER: the first that is ours
constexpr int CurrentIndicator = 9;

#if defined(ARGONAUT_QSCINTILLA)
// UTF-8 bytes (Scintilla's positions) of QString offsets, and back. Walks
// the text once for a sorted list of offsets.
int utf8Length(QChar c)
{
    const ushort u = c.unicode();
    if (u < 0x80)
        return 1;
    if (u < 0x800)
        return 2;
    if (c.isHighSurrogate())
        return 4; // the pair: the low half adds nothing
    if (c.isLowSurrogate())
        return 0;
    return 3;
}

QList<int> bytesAt(const QString &text, const QList<int> &offsets)
{
    QList<int> bytes;
    bytes.reserve(offsets.size());
    int at = 0, byte = 0;
    for (int offset : offsets) {
        if (offset < at) { // (not sorted: start again)
            at = 0;
            byte = 0;
        }
        for (; at < offset && at < text.size(); ++at)
            byte += utf8Length(text.at(at));
        bytes.append(byte);
    }
    return bytes;
}

int offsetAt(const QString &text, int bytePosition)
{
    int at = 0, byte = 0;
    while (at < text.size() && byte < bytePosition)
        byte += utf8Length(text.at(at++));
    return at;
}

QsciLexer *lexerFor(const QString &fileName, QObject *parent, QString *language)
{
    const QFileInfo info(fileName);
    const QString name = info.fileName().toLower();
    const QString suffix = info.suffix().toLower();
    auto is = [&](std::initializer_list<const char *> suffixes) {
        for (const char *one : suffixes) {
            if (suffix == QLatin1String(one))
                return true;
        }
        return false;
    };
    QsciLexer *lexer = nullptr;
    if (name == "cmakelists.txt" || is({"cmake"}))
        lexer = new QsciLexerCMake(parent);
    else if (name == "makefile" || name == "gnumakefile" || is({"mk", "mak"}))
        lexer = new QsciLexerMakefile(parent);
    else if (is({"c", "h", "cc", "cpp", "cxx", "hh", "hpp", "hxx", "ino", "m", "mm"}))
        lexer = new QsciLexerCPP(parent);
    else if (is({"cs"}))
        lexer = new QsciLexerCSharp(parent);
    else if (is({"java", "kt", "kts", "gradle", "scala"}))
        lexer = new QsciLexerJava(parent);
    else if (is({"js", "mjs", "cjs", "jsx", "ts", "tsx", "qml"}))
        lexer = new QsciLexerJavaScript(parent);
    else if (is({"json", "jsonc", "geojson"}))
        lexer = new QsciLexerJSON(parent);
    else if (is({"py", "pyw", "pyi"}))
        lexer = new QsciLexerPython(parent);
    else if (is({"sh", "bash", "zsh", "ksh"}) || name == ".bashrc" || name == ".profile" || name == ".bash_profile"
             || name == ".zshrc" || name == ".bash_aliases")
        lexer = new QsciLexerBash(parent);
    else if (is({"bat", "cmd"}))
        lexer = new QsciLexerBatch(parent);
    else if (is({"html", "htm", "xhtml", "php", "vue", "svelte"}))
        lexer = new QsciLexerHTML(parent);
    else if (is({"xml", "svg", "ui", "qrc", "xsd", "xsl", "plist", "csproj", "xaml", "kml", "gpx"}))
        lexer = new QsciLexerXML(parent);
    else if (is({"css", "scss", "less", "qss"}))
        lexer = new QsciLexerCSS(parent);
    else if (is({"md", "markdown", "mdown"}))
        lexer = new QsciLexerMarkdown(parent);
    else if (is({"yaml", "yml"}))
        lexer = new QsciLexerYAML(parent);
    else if (is({"sql"}))
        lexer = new QsciLexerSQL(parent);
    else if (is({"lua"}))
        lexer = new QsciLexerLua(parent);
    else if (is({"pl", "pm"}))
        lexer = new QsciLexerPerl(parent);
    else if (is({"rb", "rake", "gemspec"}) || name == "gemfile" || name == "rakefile")
        lexer = new QsciLexerRuby(parent);
    else if (is({"tex", "sty", "cls", "bib"}))
        lexer = new QsciLexerTeX(parent);
    else if (is({"diff", "patch"}))
        lexer = new QsciLexerDiff(parent);
    else if (is({"ini", "conf", "cfg", "desktop", "service", "properties", "toml", "env", "editorconfig", "gitconfig"})
             || name == ".gitconfig" || name == ".editorconfig")
        lexer = new QsciLexerProperties(parent);
    if (lexer)
        *language = QString::fromLatin1(lexer->language());
    else
        language->clear();
    return lexer;
}

// The lexer's own colours are made for white paper; its fonts are made
// the editor's fixed one, keeping what is bold or italic.
void dressLexer(QsciLexer *lexer, const QFont &fixed)
{
    lexer->setDefaultFont(fixed);
    lexer->setDefaultPaper(Paper);
    lexer->setDefaultColor(Ink);
    for (int style = 0; style < 128; ++style) {
        if (lexer->description(style).isEmpty())
            continue;
        QFont font = fixed;
        const QFont own = lexer->font(style);
        font.setBold(own.bold());
        font.setItalic(own.italic());
        lexer->setFont(font, style);
        lexer->setPaper(Paper, style);
    }
}
#endif

} // namespace

CodeEditor::CodeEditor(QWidget *parent)
    : QWidget(parent)
{
    setObjectName("codeEditor");
    setAttribute(Qt::WA_StyledBackground);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 2, 2);
    const QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);
#if defined(ARGONAUT_QSCINTILLA)
    auto *sci = new QsciScintilla(this);
    m_view = sci;
    sci->setUtf8(true);
    sci->setEolMode(QsciScintilla::EolUnix); // TextFile gives "\n" and puts CRLF back itself
    sci->setFont(fixed);
    sci->setPaper(Paper);
    sci->setColor(Ink);
    sci->setWrapMode(QsciScintilla::WrapNone);
    sci->setScrollWidth(1);
    sci->setScrollWidthTracking(true);
    sci->setTabWidth(4);
    sci->setAutoIndent(true);
    sci->setIndentationGuides(true);
    sci->setBackspaceUnindents(true);
    sci->setBraceMatching(QsciScintilla::SloppyBraceMatch);
    sci->setMatchedBraceBackgroundColor(QColor(0xdc, 0xe8, 0xfa));
    sci->setMatchedBraceForegroundColor(Ink);
    sci->setUnmatchedBraceForegroundColor(QColor(0xe0, 0x1b, 0x24));
    sci->setCaretForegroundColor(Ink);
    sci->setCaretLineVisible(true);
    sci->setCaretLineBackgroundColor(QColor(0xf4, 0xf4, 0xf2));
    sci->setMarginType(0, QsciScintilla::NumberMargin);
    sci->setMarginsFont(fixed);
    sci->setMarginsBackgroundColor(MarginPaper);
    sci->setMarginsForegroundColor(MarginInk);
    sci->setMarginWidth(1, 4); // a little room between the numbers and the text
    sci->setFolding(QsciScintilla::BoxedTreeFoldStyle, 2);
    sci->setFoldMarginColors(MarginPaper, MarginPaper);
    sci->setFrameShape(QFrame::NoFrame);
    // The find bar's marks: boxes under the text.
    for (int indicator : {MarkIndicator, CurrentIndicator}) {
        sci->SendScintilla(QsciScintillaBase::SCI_INDICSETSTYLE, indicator, QsciScintillaBase::INDIC_STRAIGHTBOX);
        sci->SendScintilla(QsciScintillaBase::SCI_INDICSETUNDER, indicator, long(true));
    }
    connect(sci, &QsciScintilla::modificationChanged, this, &CodeEditor::modificationChanged);
    connect(sci, &QsciScintilla::textChanged, this, &CodeEditor::textChanged);
    connect(sci, &QsciScintilla::linesChanged, this, &CodeEditor::fitLineNumbers);
#else
    auto *plain = new QPlainTextEdit(this);
    m_view = plain;
    plain->setFont(fixed);
    plain->setLineWrapMode(QPlainTextEdit::NoWrap);
    plain->setFrameShape(QFrame::NoFrame);
    QPalette colours = plain->palette();
    colours.setColor(QPalette::Base, Paper);
    colours.setColor(QPalette::Text, Ink);
    plain->setPalette(colours);
    connect(plain->document(), &QTextDocument::modificationChanged, this, &CodeEditor::modificationChanged);
    connect(plain, &QPlainTextEdit::textChanged, this, &CodeEditor::textChanged);
#endif
    m_view->setFocusPolicy(Qt::ClickFocus);
    setFocusProxy(m_view);
    layout->addWidget(m_view);
}

bool CodeEditor::isCodeEditor()
{
#if defined(ARGONAUT_QSCINTILLA)
    return true;
#else
    return false;
#endif
}

QAbstractScrollArea *CodeEditor::editorWidget() const
{
    return m_view;
}

void CodeEditor::setText(const QString &text, const QString &fileName)
{
#if defined(ARGONAUT_QSCINTILLA)
    auto *sci = static_cast<QsciScintilla *>(m_view);
    QsciLexer *old = sci->lexer();
    QsciLexer *lexer = lexerFor(fileName, sci, &m_language);
    if (lexer)
        dressLexer(lexer, QFontDatabase::systemFont(QFontDatabase::FixedFont));
    sci->setLexer(lexer);
    delete old;
    if (!lexer) { // setLexer(nullptr) leaves Scintilla's own defaults
        sci->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        sci->setPaper(Paper);
        sci->setColor(Ink);
    }
    // Indented with tabs: the Tab key goes on doing so; else spaces.
    bool tabs = false;
    int spaces = 0;
    for (QStringView line : QStringView(text).split(u'\n')) {
        if (line.startsWith(u'\t')) {
            tabs = true;
            break;
        }
        if (line.startsWith(QLatin1String("  ")) && ++spaces > 2)
            break;
    }
    sci->setIndentationsUseTabs(tabs);
    sci->setText(text);
    sci->setModified(false);
    sci->SendScintilla(QsciScintillaBase::SCI_EMPTYUNDOBUFFER); // the loaded text can't be undone away
    fitLineNumbers();
#else
    Q_UNUSED(fileName);
    m_language.clear();
    static_cast<QPlainTextEdit *>(m_view)->setPlainText(text);
#endif
}

QString CodeEditor::text() const
{
#if defined(ARGONAUT_QSCINTILLA)
    return static_cast<QsciScintilla *>(m_view)->text();
#else
    return static_cast<QPlainTextEdit *>(m_view)->toPlainText();
#endif
}

bool CodeEditor::isModified() const
{
#if defined(ARGONAUT_QSCINTILLA)
    return static_cast<QsciScintilla *>(m_view)->isModified();
#else
    return static_cast<QPlainTextEdit *>(m_view)->document()->isModified();
#endif
}

void CodeEditor::setModified(bool modified)
{
#if defined(ARGONAUT_QSCINTILLA)
    static_cast<QsciScintilla *>(m_view)->setModified(modified);
#else
    static_cast<QPlainTextEdit *>(m_view)->document()->setModified(modified);
#endif
}

void CodeEditor::moveToStart()
{
#if defined(ARGONAUT_QSCINTILLA)
    static_cast<QsciScintilla *>(m_view)->setCursorPosition(0, 0);
    static_cast<QsciScintilla *>(m_view)->ensureLineVisible(0);
#else
    static_cast<QPlainTextEdit *>(m_view)->moveCursor(QTextCursor::Start);
#endif
}

// As wide as the last line's number, and a digit more, so it doesn't change at every tenth line.
void CodeEditor::fitLineNumbers()
{
#if defined(ARGONAUT_QSCINTILLA)
    auto *sci = static_cast<QsciScintilla *>(m_view);
    const int digits = qMax(3, int(QString::number(sci->lines()).size()) + 1);
    if (digits == m_digits)
        return;
    m_digits = digits;
    sci->setMarginWidth(0, QString(digits, QLatin1Char('9')));
#endif
}

void CodeEditor::setPaper(const QColor &accent)
{
    setStyleSheet(QStringLiteral("CodeEditor#codeEditor { background: #ffffff; border: 1px solid %1; border-radius: 8px; }")
                      .arg(accent.name()));
#if defined(ARGONAUT_QSCINTILLA)
    auto *sci = static_cast<QsciScintilla *>(m_view);
    sci->setSelectionBackgroundColor(accent);
    sci->setSelectionForegroundColor(Paper);
#else
    QPalette colours = m_view->palette();
    colours.setColor(QPalette::Highlight, accent);
    colours.setColor(QPalette::HighlightedText, Paper);
    m_view->setPalette(colours);
#endif
}

QString CodeEditor::selectedText() const
{
#if defined(ARGONAUT_QSCINTILLA)
    return static_cast<QsciScintilla *>(m_view)->selectedText();
#else
    return static_cast<QPlainTextEdit *>(m_view)->textCursor().selectedText();
#endif
}

int CodeEditor::firstVisibleOffset() const
{
#if defined(ARGONAUT_QSCINTILLA)
    auto *sci = static_cast<QsciScintilla *>(m_view);
    const long visible = sci->SendScintilla(QsciScintillaBase::SCI_GETFIRSTVISIBLELINE);
    const long line = sci->SendScintilla(QsciScintillaBase::SCI_DOCLINEFROMVISIBLE, visible);
    const long byte = sci->SendScintilla(QsciScintillaBase::SCI_POSITIONFROMLINE, line);
    return offsetAt(sci->text(), int(byte));
#else
    return static_cast<QPlainTextEdit *>(m_view)->cursorForPosition(QPoint(0, 0)).position();
#endif
}

void CodeEditor::select(const Range &range)
{
#if defined(ARGONAUT_QSCINTILLA)
    auto *sci = static_cast<QsciScintilla *>(m_view);
    const QList<int> bytes = bytesAt(sci->text(), {range.start, range.start + range.length});
    sci->SendScintilla(QsciScintillaBase::SCI_SETSEL, bytes.at(0), long(bytes.at(1)));
#else
    auto *plain = static_cast<QPlainTextEdit *>(m_view);
    QTextCursor cursor(plain->document());
    cursor.setPosition(range.start);
    cursor.setPosition(range.start + range.length, QTextCursor::KeepAnchor);
    plain->setTextCursor(cursor);
    plain->ensureCursorVisible();
#endif
}

void CodeEditor::setMarks(const QList<Range> &ranges, int current, const QColor &mark, const QColor &currentMark)
{
#if defined(ARGONAUT_QSCINTILLA)
    auto *sci = static_cast<QsciScintilla *>(m_view);
    const long length = sci->SendScintilla(QsciScintillaBase::SCI_GETLENGTH);
    for (int indicator : {MarkIndicator, CurrentIndicator}) {
        sci->SendScintilla(QsciScintillaBase::SCI_SETINDICATORCURRENT, indicator);
        sci->SendScintilla(QsciScintillaBase::SCI_INDICATORCLEARRANGE, 0, length);
    }
    if (ranges.isEmpty())
        return;
    sci->SendScintilla(QsciScintillaBase::SCI_INDICSETFORE, MarkIndicator, mark);
    sci->SendScintilla(QsciScintillaBase::SCI_INDICSETALPHA, MarkIndicator, long(qMax(mark.alpha(), 60)));
    sci->SendScintilla(QsciScintillaBase::SCI_INDICSETFORE, CurrentIndicator, currentMark);
    sci->SendScintilla(QsciScintillaBase::SCI_INDICSETALPHA, CurrentIndicator, long(qMax(currentMark.alpha(), 120)));
    QList<int> offsets;
    offsets.reserve(ranges.size() * 2);
    for (const Range &range : ranges)
        offsets << range.start << range.start + range.length;
    const QList<int> bytes = bytesAt(sci->text(), offsets);
    for (int i = 0; i < ranges.size(); ++i) {
        sci->SendScintilla(QsciScintillaBase::SCI_SETINDICATORCURRENT, i == current ? CurrentIndicator : MarkIndicator);
        sci->SendScintilla(QsciScintillaBase::SCI_INDICATORFILLRANGE, bytes.at(2 * i), long(bytes.at(2 * i + 1) - bytes.at(2 * i)));
    }
#else
    auto *plain = static_cast<QPlainTextEdit *>(m_view);
    QList<QTextEdit::ExtraSelection> marks;
    for (int i = 0; i < ranges.size(); ++i) {
        QTextEdit::ExtraSelection one;
        one.cursor = QTextCursor(plain->document());
        one.cursor.setPosition(ranges.at(i).start);
        one.cursor.setPosition(ranges.at(i).start + ranges.at(i).length, QTextCursor::KeepAnchor);
        one.format.setBackground(i == current ? currentMark : mark);
        marks.append(one);
    }
    plain->setExtraSelections(marks);
#endif
}
