#pragma once

#include <QColor>
#include <QList>
#include <QWidget>

class QAbstractScrollArea;

// The editor of "Edit in Preview": a text file being changed in the
// preview pane. Built with QScintilla (ARGONAUT_QSCINTILLA) it is a code
// editor: line numbers, the language's colours chosen by the file's name,
// folding, matching brackets, the current line marked. Without it, a plain
// QPlainTextEdit with the same functions, so nothing else has to know.
//
// Positions (`find`, `select`, `setMarks`) are offsets into text(), in
// QString units, whichever editor it is.
class CodeEditor : public QWidget
{
    Q_OBJECT

public:
    explicit CodeEditor(QWidget *parent = nullptr);

    static bool isCodeEditor(); // QScintilla: colours, line numbers, folding

    // The text of a file named `fileName` (its ending picks the language).
    void setText(const QString &text, const QString &fileName);
    QString text() const;
    // The language the colours are for ("Python"); empty: plain text.
    QString language() const { return m_language; }
    bool isModified() const;
    void setModified(bool modified);
    void moveToStart();
    // The widget that has the keys and the focus.
    QAbstractScrollArea *editorWidget() const;
    // On white paper, like a notepad, whatever the theme; `accent` for the
    // selection and the frame.
    void setPaper(const QColor &accent);

    // For the find bar.
    struct Range {
        int start = 0;
        int length = 0;
    };
    QString selectedText() const;
    int firstVisibleOffset() const; // where the text in sight begins
    void select(const Range &range); // and scrolls to it
    void setMarks(const QList<Range> &ranges, int current, const QColor &mark, const QColor &currentMark);

signals:
    void modificationChanged(bool modified);
    void textChanged();

private:
    void fitLineNumbers();

    QString m_language;
    QAbstractScrollArea *m_view = nullptr; // QsciScintilla, or QPlainTextEdit without it
    int m_digits = 0;                       // the line-number margin is this wide
};
