#pragma once

#include <memory>

#include "TextFile.h"

#include "Extensions.h"
#include "Preview.h"

#include <QElapsedTimer>
#include <QPixmap>
#include <QWidget>

class PdfView;
class QLabel;
class QLineEdit;
class QMovie;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QTextBrowser;
class QTimer;
class QTreeWidget;
class Thumbnailer;

// Quick Look (Space): a preview of the current file, as a pane at the right
// of the window. The content is loaded on a worker (Preview::load), so a
// slow file never blocks; a newer request replaces an older one. It never
// takes the keyboard: the file view keeps it, and the window sends the pane
// each newly selected file.
class TipPicture;
class QuickLook : public QWidget
{
    Q_OBJECT

public:
    struct Item
    {
        QString path;
        QString name;
        QString mimeType;              // the effective (real) type
        ContentType::Verdict verdict = ContentType::Verdict::NotChecked;
        QString details;               // "PDF document · 1.2 MB · 13/11/2026"
        QIcon icon;
        QString listing;               // the extension's list it is shown in, if any (its address)
    };

    QuickLook(Thumbnailer *thumbnailer, QWidget *parent);

    void showItem(const Item &item);
    void clear(); // nothing is selected
    // Something that isn't a file's preview: a result an extension wants shown
    // (a diff). It stays until a file is selected or the pane closes.
    // A result with a name (`content.resultId`) can be replaced while it is
    // shown, and its extension is told when it is shown (and how much room
    // there is) and when it no longer is. An `update` for a result that
    // isn't the one shown does nothing. True: it is shown now.
    bool showResult(const QString &title, const QString &details, const Extensions::PreviewReply &content);
    // Changes an extension proposes: shown until they are applied or
    // dismissed (planApplied / planDismissed); selecting a file meanwhile
    // doesn't replace them. False: another plan has the pane.
    bool showPlan(const Extensions::Plan &plan);
    bool hasPlan() const;
    // What an extension shows (a dashboard, a list of services, a diff) has
    // the pane: it stays while files are selected and folders change, until
    // the user asks for a file's preview (Space) or closes the pane.
    bool hasResult() const { return m_showsResult; }
    QString detailsText() const; // the line under the title (tests)
    QString noteText() const;    // the line at the bottom (tests)
    // An extension is waiting for something that takes a while (Claude's
    // answer): the pane says so, with how long it has been, until
    // endWorking (or a plan, or the pane closing). Selecting a file
    // meanwhile doesn't take it away; "Stop Waiting" asks to (workingStopRequested).
    void showWorking(const QString &extension, const QString &title, const QString &text);
    void endWorking(const QString &extension);
    bool isWorking() const { return !m_workingFor.isEmpty(); }
    // A plan or "working" has the pane: files and other results don't take it.
    bool isHeld() const { return hasPlan() || isWorking() || m_editing; }
    // Editing the text file shown (the Edit box, or Ctrl+Space from the
    // files): it holds the pane until the box is unticked or the pane closes.
    void editCurrent();
    bool isEditing() const { return m_editing; }
    bool saveEditing();
    // Before the pane closes (or the window): unsaved changes are asked
    // about. False: the user went on editing, so it mustn't close.
    bool mayClose();
    class QCheckBox *editBox() const { return m_editBox; } // (tests)
    QString workingText() const; // what it says (tests)
    QPushButton *stopWaitingButton() const { return m_stopWaiting; } // (tests)
    class PlanView *planView() const { return m_plan; } // (tests)
    QString resultId() const { return m_resultId; } // the named result shown, if any (tests)
    QString path() const { return m_item.path; }
    Preview::Kind kind() const { return m_kind; } // what's shown (tests)
    bool fromExtension() const { return m_fromExtension; } // an extension made what's shown
    QString shownText() const;                           // the text or rich text shown (tests)
    // A picture that scrolls is shown (an extension's, with "scroll"): how far it can be
    // scrolled and where it is, in pixels; -1: none is shown (tests).
    int pictureScrollRange() const;
    // The part under a result that stays on screen (tests): whether it is
    // there, what it says, and its waiting line and Stop Waiting button.
    bool isUnderShown() const;
    QString underTitle() const;
    QString underText() const;
    QString underNote() const;
    QString underWaitingText() const;
    QPushButton *underStopButton() const { return m_underStop; }
    void closeUnder();
    // The button under a result and the text beside it (tests).
    QPushButton *actionButton() const { return m_actionButton; }
    class QLineEdit *actionInput() const { return m_actionInput; }
    bool hasAction() const;
    TipPicture *tipPicture() const { return m_scrollPicture; } // the long picture and its tooltips (tests)
    int pictureScrollValue() const;
    void scrollPictureTo(int value);
    class TextFinder *finder() const { return m_finder; } // Ctrl+F in a text (tests)
    QPlainTextEdit *textView() const { return m_text; }   // (tests)
    QLineEdit *searchField() const { return m_search; } // shown over a result that can be searched (tests)
    PdfView *pdfReader() const { return m_pdf; }         // the reader, shown for a PDF where it can be (tests)
    bool isReadingPdf() const;
    bool hasOpenButton() const;                          // "Open" is there: not for an extension's result, which is no file (tests)

signals:
    void planApplied(const Extensions::Plan &plan, const QList<Extensions::PlanMove> &kept);
    void planDismissed(const Extensions::Plan &plan);
    void workingStopRequested(const QString &extension);
    void planItemActivated(const QString &path); // a move was clicked: show its file in the folder
    void keysBackRequested(); // Esc or Enter in the search field: the keys are the files' again
    void openRequested(const QString &path);
    void closeRequested();
    void fileSaved(const QString &path);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void apply(quint64 generation, const Preview::Result &result);
    void loadBuiltIn(quint64 generation);
    void loadPreview(quint64 generation); // Preview::load: what every kind of file gets
    void extensionAnswered(int request, const Extensions::PreviewReply &reply);
    bool applyExtension(const Extensions::PreviewReply &reply);
    void showInfo(const QString &message);
    QSize pictureBox() const;

    Thumbnailer *m_thumbnailer;
    Item m_item;
    quint64 m_generation = 0;
    Preview::Kind m_kind = Preview::Kind::Generic;
    QLabel *m_title = nullptr;
    QLabel *m_details = nullptr;
    QStackedWidget *m_pages = nullptr;
    class PictureView *m_picture = nullptr;
    class PlanView *m_plan = nullptr;      // changes an extension proposes, to tick and apply
    void closePlan(bool dismissed);
    QString m_workingFor;                  // the extension the pane says "working" for
    QString m_workingText;
    QElapsedTimer m_workingSince;
    QTimer *m_workingTimer = nullptr;      // moves the dots and counts the seconds
    QPushButton *m_stopWaiting = nullptr;
    bool m_showsResult = false;            // an extension's result is what is shown
    // The lower part of the pane, under a named result that stays on
    // screen: what its extension has to say about it (Claude's report on
    // the list above), or that Claude is being asked. A divider between the
    // two can be dragged.
    class QSplitter *m_split = nullptr;
    QWidget *m_under = nullptr;
    QLabel *m_underTitle = nullptr;
    class RichView *m_underText = nullptr;
    QWidget *m_underWait = nullptr;
    QLabel *m_underWaitText = nullptr;
    QPushButton *m_underStop = nullptr;
    QLabel *m_underNote = nullptr;
    QString m_underWaitFor;                // the extension Claude is being asked for, said down there
    bool m_underHasText = false;
    bool showUnder(const QString &title, const Extensions::PreviewReply &content);
    void openUnder();
    void updateWorking();
    void forgetWorking(); // the pane no longer says "working" (the waiting itself may go on)
    PdfView *m_pdf = nullptr;        // the PDF reader: every page, in the sandbox (PdfView)
    class QScrollArea *m_scroll = nullptr; // an extension's picture that is taller than the pane ("scroll")
    TipPicture *m_scrollPicture = nullptr; // a long picture at its own size; parts of it may have tooltips
    QLabel *m_movieLabel = nullptr;
    QMovie *m_movie = nullptr;
    QPlainTextEdit *m_text = nullptr;
    class TextFinder *m_finder = nullptr; // Ctrl+F in a text
    QTimer *m_refind = nullptr;           // the text changed: find again
    bool textKey(QWidget *view, QEvent *event);
    void themeFinder();
    QTextBrowser *m_rich = nullptr;   // Markdown and HTML from extensions
    int m_extensionRequest = 0;       // waiting for this answer (0: not waiting)
    QTimer *m_extensionTimer = nullptr; // not for long: then the built-in preview
    bool m_fromExtension = false;
    void closeResult();              // the named result is no longer shown: its extension is told
    void reportResult();             // it is shown, with this much room
    QString m_resultExtension;       // the named result on screen (an extension's id and its result's)
    QString m_resultId;
    QTimer *m_resultTimer = nullptr; // a resize is told once it has settled
    class QLineEdit *m_search = nullptr; // over a result that can be searched; what is typed goes to its extension
    QTimer *m_searchTimer = nullptr;     // …once the typing pauses
    // Under a result that offers something to do with it ("Analyze with
    // Claude"): a button, and a line of text to go with it when it wants one.
    QWidget *m_actionRow = nullptr;
    class QLineEdit *m_actionInput = nullptr;
    QPushButton *m_actionButton = nullptr;
    QString m_actionId;
    void setActionShown(const Extensions::PreviewReply &content);
    void runAction();
    void setSearchShown(bool on, const QString &hint = QString());
    QTreeWidget *m_archive = nullptr;
    QWidget *m_infoPage = nullptr;
    QLabel *m_infoIcon = nullptr;
    QLabel *m_infoText = nullptr;
    QLabel *m_note = nullptr;
    QPushButton *m_open = nullptr;
    // Editing (the Edit box).
    class QCheckBox *m_editBox = nullptr;
    QPushButton *m_save = nullptr;
    bool m_editing = false;
    bool m_editKept = false; // the version from before is in the Trash already (this editing)
    std::unique_ptr<TextFile::Loaded> m_edit;
    QMetaObject::Connection m_editTitle;
    static bool isEditableKind(const Item &item);
    void applyEditLook();
    bool startEditing();
    bool stopEditing();
    void endEditing(); // no questions: the editing is over
};
