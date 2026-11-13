#pragma once

#include "Preview.h"

#include <QPixmap>
#include <QWidget>

class QLabel;
class QMovie;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QTreeWidget;
class Thumbnailer;

// Quick Look (Space): a preview of the current file in a window over the
// main one. The content is loaded on a worker (Preview::load), so a slow
// file never blocks; a newer request replaces an older one. Keys: Space and
// Esc close, Enter opens, arrows (and Home/End/Page keys) go to the next
// file in the view (navigate()).
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
    };

    QuickLook(Thumbnailer *thumbnailer, QWidget *parent);

    void showItem(const Item &item);
    QString path() const { return m_item.path; }
    Preview::Kind kind() const { return m_kind; } // what's shown (tests)

signals:
    void navigate(QKeyEvent *event); // a key for the file view
    void openRequested(const QString &path);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void apply(quint64 generation, const Preview::Result &result);
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
    QLabel *m_movieLabel = nullptr;
    QMovie *m_movie = nullptr;
    QPlainTextEdit *m_text = nullptr;
    QTreeWidget *m_archive = nullptr;
    QWidget *m_infoPage = nullptr;
    QLabel *m_infoIcon = nullptr;
    QLabel *m_infoText = nullptr;
    QLabel *m_note = nullptr;
    QPushButton *m_open = nullptr;
};
