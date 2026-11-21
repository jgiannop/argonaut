#include "TextFinder.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTextDocument>
#include <QTextEdit>
#include <QToolButton>

namespace {

constexpr int MaxMarks = 10000; // a find of "e" in a big file marks this many, and counts on

QTextDocument *documentOf(QWidget *view)
{
    if (auto *plain = qobject_cast<QPlainTextEdit *>(view))
        return plain->document();
    if (auto *rich = qobject_cast<QTextEdit *>(view))
        return rich->document();
    return nullptr;
}

bool findable(QWidget *view)
{
    return documentOf(view) || qobject_cast<CodeEditor *>(view);
}

} // namespace

TextFinder::TextFinder(QWidget *parent)
    : QFrame(parent)
{
    setObjectName("textFinder");
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(4);
    m_field = new QLineEdit(this);
    m_field->setObjectName("searchEdit");
    m_field->setPlaceholderText(tr("Find in the text"));
    m_field->setClearButtonEnabled(true);
    m_field->setFixedHeight(30);
    m_field->setMaxLength(500);
    m_field->installEventFilter(this);
    row->addWidget(m_field, 1);
    m_count = new QLabel(this);
    m_count->setObjectName("qlDetails");
    m_count->setMinimumWidth(m_count->fontMetrics().horizontalAdvance(QStringLiteral("10000 of 10000+")));
    m_count->setAlignment(Qt::AlignCenter);
    row->addWidget(m_count);
    auto button = [this, row](const QString &text, const QString &tip) {
        auto *b = new QToolButton(this);
        b->setText(text);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);
        row->addWidget(b);
        return b;
    };
    m_previous = button(QStringLiteral("↑"), tr("Previous (Shift+Enter)"));
    m_next = button(QStringLiteral("↓"), tr("Next (Enter)"));
    m_close = button(QStringLiteral("✕"), tr("Close (Esc)"));
    connect(m_previous, &QToolButton::clicked, this, [this] { next(-1); });
    connect(m_next, &QToolButton::clicked, this, [this] { next(+1); });
    connect(m_close, &QToolButton::clicked, this, [this] {
        close();
        emit closed();
    });
    connect(m_field, &QLineEdit::textChanged, this, [this] { find(); });
    hide();
}

void TextFinder::applyTheme(const QColor &markColor, const QColor &currentColor)
{
    m_mark = markColor;
    m_currentMark = currentColor;
    if (isVisible())
        mark();
}

void TextFinder::open(QWidget *view)
{
    if (!findable(view))
        return;
    if (m_view && m_view != view)
        close();
    m_view = view;
    const QString selected = selectedText();
    show();
    // What is selected (one line of it) is what to find, as in an editor.
    if (!selected.isEmpty() && !selected.contains(QChar::ParagraphSeparator) && !selected.contains(QLatin1Char('\n'))
        && selected.size() <= 200)
        m_field->setText(selected);
    else
        find(); // what was typed the last time, in this text
    m_field->setFocus(Qt::ShortcutFocusReason);
    m_field->selectAll();
}

void TextFinder::close()
{
    m_found.clear();
    m_current = -1;
    mark();
    hide();
}

void TextFinder::textChanged()
{
    if (isVisible())
        find();
}

void TextFinder::textEdited()
{
    if (isVisible())
        find(false);
}

QString TextFinder::selectedText() const
{
    if (auto *plain = qobject_cast<QPlainTextEdit *>(m_view))
        return plain->textCursor().selectedText();
    if (auto *rich = qobject_cast<QTextEdit *>(m_view))
        return rich->textCursor().selectedText();
    if (auto *code = qobject_cast<CodeEditor *>(m_view))
        return code->selectedText();
    return {};
}

void TextFinder::select(const CodeEditor::Range &range)
{
    if (auto *code = qobject_cast<CodeEditor *>(m_view)) {
        code->select(range);
        return;
    }
    QTextDocument *document = documentOf(m_view);
    if (!document)
        return;
    QTextCursor cursor(document);
    cursor.setPosition(range.start);
    cursor.setPosition(range.start + range.length, QTextCursor::KeepAnchor);
    if (auto *plain = qobject_cast<QPlainTextEdit *>(m_view)) {
        plain->setTextCursor(cursor);
        plain->ensureCursorVisible();
    } else if (auto *rich = qobject_cast<QTextEdit *>(m_view)) {
        rich->setTextCursor(cursor);
        rich->ensureCursorVisible();
    }
}

// Every place the text is, from the start; the current one is the first at
// or after where the reading is (the top of what is in sight).
void TextFinder::find(bool moveTo)
{
    m_found.clear();
    m_current = -1;
    const QString text = m_field->text();
    if (!text.isEmpty()) {
        if (auto *code = qobject_cast<CodeEditor *>(m_view)) {
            const QString all = code->text();
            for (qsizetype at = all.indexOf(text, 0, Qt::CaseInsensitive); at >= 0 && m_found.size() < MaxMarks;
                 at = all.indexOf(text, at + text.size(), Qt::CaseInsensitive))
                m_found.append({int(at), int(text.size())});
        } else if (QTextDocument *document = documentOf(m_view)) {
            QTextCursor at(document);
            while (m_found.size() < MaxMarks) {
                at = document->find(text, at); // no flags: any case
                if (at.isNull())
                    break;
                m_found.append({at.selectionStart(), at.selectionEnd() - at.selectionStart()});
            }
        }
        if (!m_found.isEmpty()) {
            // The first one from the top of what is shown, so the text doesn't jump back to its start.
            int from = 0;
            if (auto *plain = qobject_cast<QPlainTextEdit *>(m_view))
                from = plain->cursorForPosition(QPoint(0, 0)).position();
            else if (auto *rich = qobject_cast<QTextEdit *>(m_view))
                from = rich->cursorForPosition(QPoint(0, 0)).position();
            else if (auto *code = qobject_cast<CodeEditor *>(m_view))
                from = code->firstVisibleOffset();
            m_current = 0;
            for (int i = 0; i < m_found.size(); ++i) {
                if (m_found.at(i).start >= from) {
                    m_current = i;
                    break;
                }
            }
            if (moveTo)
                select(m_found.at(m_current));
        }
    }
    mark();
}

void TextFinder::next(int step)
{
    if (m_found.isEmpty())
        return;
    m_current = int((m_current + step + m_found.size()) % m_found.size());
    select(m_found.at(m_current));
    mark();
}

QString TextFinder::countText() const
{
    if (m_field->text().isEmpty())
        return {};
    if (m_found.isEmpty())
        return tr("No matches");
    return tr("%1 of %2%3").arg(m_current + 1).arg(m_found.size()).arg(m_found.size() >= MaxMarks ? QStringLiteral("+") : QString());
}

void TextFinder::mark()
{
    m_count->setText(countText());
    if (auto *code = qobject_cast<CodeEditor *>(m_view)) {
        code->setMarks(m_found, m_current, m_mark, m_currentMark);
        return;
    }
    QTextDocument *document = documentOf(m_view);
    QList<QTextEdit::ExtraSelection> marks;
    for (int i = 0; document && i < m_found.size(); ++i) {
        QTextEdit::ExtraSelection one;
        one.cursor = QTextCursor(document);
        one.cursor.setPosition(m_found.at(i).start);
        one.cursor.setPosition(m_found.at(i).start + m_found.at(i).length, QTextCursor::KeepAnchor);
        one.format.setBackground(i == m_current ? m_currentMark : m_mark);
        marks.append(one);
    }
    if (auto *plain = qobject_cast<QPlainTextEdit *>(m_view))
        plain->setExtraSelections(marks);
    else if (auto *rich = qobject_cast<QTextEdit *>(m_view))
        rich->setExtraSelections(marks);
}

bool TextFinder::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_field && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        switch (key->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_F3:
            next(key->modifiers() & Qt::ShiftModifier ? -1 : +1);
            return true;
        case Qt::Key_Escape:
            close();
            emit closed();
            return true;
        default:
            break;
        }
    }
    // The window's shortcuts (Ctrl+F is its search, Enter opens files…)
    // don't take these from the field.
    if (watched == m_field && event->type() == QEvent::ShortcutOverride) {
        const int key = static_cast<QKeyEvent *>(event)->key();
        if (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Escape || key == Qt::Key_F3 || key == Qt::Key_F)
            event->accept();
    }
    return QFrame::eventFilter(watched, event);
}
