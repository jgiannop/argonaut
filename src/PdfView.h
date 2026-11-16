#pragma once

#include <QAbstractScrollArea>
#include <QHash>
#include <QPixmap>
#include <QSet>
#include <QSizeF>
#include <QString>

class QThreadPool;
class QTimer;

// The PDF reader of the preview pane: every page of the file, one under
// another, fitted to the pane's width; the wheel and the scrollbar go
// through them, Ctrl and the wheel make them bigger.
//
// Pages are pictures made by PdfPages (poppler's tools in the sandbox), and
// only the ones in sight (and the next) are made, so a long document opens
// as fast as a short one. A page that isn't ready is a blank sheet with its
// number. It never takes the keyboard: the file view keeps it.
class PdfView : public QAbstractScrollArea
{
    Q_OBJECT

public:
    explicit PdfView(QWidget *parent = nullptr);
    ~PdfView() override;

    // Shows `path` from its first page. failed() follows if it can't be
    // read as a PDF (the pane then shows what it shows for any file).
    void setFile(const QString &path);
    void clear();

    QString path() const { return m_path; }
    int pageCount() const { return m_pages; }
    int currentPage() const;          // the page at the top of the pane, from 1 (0: none)
    int readyPages() const { return int(m_shown.size()); } // drawn so far (tests)
    qreal zoom() const { return m_zoom; }
    void setZoom(qreal zoom);         // 1: fitted to the width
    void showPage(int number);        // scrolls to it

signals:
    void loaded(int pages);
    void failed();
    void pageChanged(int page, int pages);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    struct Shown
    {
        QPixmap picture;
        int width = 0; // device pixels it was made for
    };
    int pageWidth() const;            // of a page on screen
    int pageHeight() const;
    int pageTop(int number) const;
    void layoutPages();
    void want();                      // asks for the pages in sight that aren't there
    void pump();
    void pageMade(quint64 file, int number, int width, const QImage &image);
    void tellPage();

    QString m_path;
    quint64 m_file = 0;               // counts files: an answer for an earlier one is dropped
    int m_pages = 0;
    QSizeF m_pageSize{595, 842};
    qreal m_zoom = 1;
    QHash<int, Shown> m_shown;
    QList<int> m_wanted;              // most wanted first
    QSet<int> m_making;
    QThreadPool *m_pool;
    QTimer *m_again;                  // after a resize or a zoom: made again at the new size
    int m_told = -1;
    int m_wheel = 0;
};
