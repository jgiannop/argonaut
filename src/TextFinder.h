#pragma once

#include "CodeEditor.h"

#include <QFrame>
#include <QList>
#include <QPointer>

class QLabel;
class QLineEdit;
class QToolButton;

// Ctrl+F in a text the preview pane shows (a text file, Markdown, a
// report): a small bar under it. What is typed is found as it is typed,
// ignoring case; every place is marked, the current one selected and
// scrolled to. Enter / F3 the next, Shift+Enter / Shift+F3 the one before,
// Esc closes the bar and gives the keys back to the text.
//
// The text is a QPlainTextEdit or a QTextEdit (both have a document,
// a cursor and extra selections, but no common class for them), or the
// CodeEditor of a file being edited.
class TextFinder : public QFrame
{
    Q_OBJECT

public:
    explicit TextFinder(QWidget *parent);

    // Shows the bar for `view`, with the text selected there (if any) to find.
    void open(QWidget *view);
    void close();       // hides it and takes the marks away
    void next(int step); // +1: the next place, -1: the one before
    QWidget *view() const { return m_view; }
    QLineEdit *field() const { return m_field; } // (tests)
    int count() const { return int(m_found.size()); }
    int current() const { return m_current; } // 0-based; -1: none
    QString countText() const;                 // "3 of 12", "No matches"

    // The text changed under it (another file): found again in the new one.
    void textChanged();
    // The text was typed in (an editor): the places found again, the cursor left where it is.
    void textEdited();

    void applyTheme(const QColor &mark, const QColor &currentMark);

signals:
    void closed(); // the keys go back to the text

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void find(bool moveTo = true);
    void mark();
    QString selectedText() const;
    void select(const CodeEditor::Range &range);

    QPointer<QWidget> m_view;
    QLineEdit *m_field = nullptr;
    QLabel *m_count = nullptr;
    QToolButton *m_previous = nullptr;
    QToolButton *m_next = nullptr;
    QToolButton *m_close = nullptr;
    QList<CodeEditor::Range> m_found;
    int m_current = -1;
    QColor m_mark, m_currentMark;
};
