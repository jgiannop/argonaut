#pragma once

#include "Extensions.h"
#include "Preview.h"

#include <QPixmap>
#include <QWidget>

class QLabel;
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
    QString resultId() const { return m_resultId; } // the named result shown, if any (tests)
    QString path() const { return m_item.path; }
    Preview::Kind kind() const { return m_kind; } // what's shown (tests)
    bool fromExtension() const { return m_fromExtension; } // an extension made what's shown
    QString shownText() const;                           // the text or rich text shown (tests)
    // A picture that scrolls is shown (an extension's, with "scroll"): how far it can be
    // scrolled and where it is, in pixels; -1: none is shown (tests).
    int pictureScrollRange() const;
    int pictureScrollValue() const;
    void scrollPictureTo(int value);

signals:
    void openRequested(const QString &path);
    void closeRequested();

protected:
    void hideEvent(QHideEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void apply(quint64 generation, const Preview::Result &result);
    void loadBuiltIn(quint64 generation);
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
    class QScrollArea *m_scroll = nullptr; // an extension's picture that is taller than the pane ("scroll")
    QLabel *m_scrollPicture = nullptr;
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
    QTreeWidget *m_archive = nullptr;
    QWidget *m_infoPage = nullptr;
    QLabel *m_infoIcon = nullptr;
    QLabel *m_infoText = nullptr;
    QLabel *m_note = nullptr;
    QPushButton *m_open = nullptr;
};
