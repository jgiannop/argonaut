#pragma once

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
    // An extension is waiting for something that takes a while (Claude's
    // answer): the pane says so, with how long it has been, until
    // endWorking (or a plan, or the pane closing). Selecting a file
    // meanwhile doesn't take it away; "Stop Waiting" asks to (workingStopRequested).
    void showWorking(const QString &extension, const QString &title, const QString &text);
    void endWorking(const QString &extension);
    bool isWorking() const { return !m_workingFor.isEmpty(); }
    // A plan or "working" has the pane: files and other results don't take it.
    bool isHeld() const { return hasPlan() || isWorking(); }
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
    TipPicture *tipPicture() const { return m_scrollPicture; } // the long picture and its tooltips (tests)
    int pictureScrollValue() const;
    void scrollPictureTo(int value);
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
    void updateWorking();
    void forgetWorking(); // the pane no longer says "working" (the waiting itself may go on)
    PdfView *m_pdf = nullptr;        // the PDF reader: every page, in the sandbox (PdfView)
    class QScrollArea *m_scroll = nullptr; // an extension's picture that is taller than the pane ("scroll")
    TipPicture *m_scrollPicture = nullptr; // a long picture at its own size; parts of it may have tooltips
    QLabel *m_movieLabel = nullptr;
    QMovie *m_movie = nullptr;
    QPlainTextEdit *m_text = nullptr;
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
    void setSearchShown(bool on, const QString &hint = QString());
    QTreeWidget *m_archive = nullptr;
    QWidget *m_infoPage = nullptr;
    QLabel *m_infoIcon = nullptr;
    QLabel *m_infoText = nullptr;
    QLabel *m_note = nullptr;
    QPushButton *m_open = nullptr;
};
