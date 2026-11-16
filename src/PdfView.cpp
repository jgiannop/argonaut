#include "PdfView.h"

#include "PdfPages.h"
#include "Theme.h"

#include <QGuiApplication>
#include <QPainter>
#include <QPointer>
#include <QScrollBar>
#include <QThreadPool>
#include <QTimer>
#include <QWheelEvent>

namespace {
// Where pages are drawn: two at a time, for all the readers there are (a
// preview, not the whole machine). It is not a reader's own: a reader that
// goes away while a slow page is being drawn doesn't wait for it.
constexpr int kAtOnce = 2;
QThreadPool *pagesPool()
{
    static QThreadPool pool;
    static const bool set = (pool.setMaxThreadCount(kAtOnce), true);
    Q_UNUSED(set);
    return &pool;
}

constexpr int kMargin = 8;    // around the pages
constexpr int kGap = 10;      // between two
constexpr int kKept = 14;     // pages kept as pictures: the ones in sight and those just left
constexpr qreal kMaxZoom = 4;
}

PdfView::PdfView(QWidget *parent)
    : QAbstractScrollArea(parent)
    , m_fileNow(std::make_shared<std::atomic<quint64>>(0))
    , m_again(new QTimer(this))
{
    setFocusPolicy(Qt::NoFocus);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    viewport()->setAutoFillBackground(false);
    m_again->setSingleShot(true);
    m_again->setInterval(160);
    connect(m_again, &QTimer::timeout, this, [this] { want(); });
}

PdfView::~PdfView()
{
    m_fileNow->store(0); // what is still waiting to be drawn isn't; nothing is waited for
}

void PdfView::clear()
{
    m_fileNow->store(++m_file); // pages of the file we leave, still waiting to be drawn, aren't
    m_path.clear();
    m_pages = 0;
    m_shown.clear();
    m_wanted.clear();
    m_making.clear();
    m_told = -1;
    m_zoom = 1;
    layoutPages();
    viewport()->update();
}

void PdfView::setFile(const QString &path)
{
    clear();
    m_path = path;
    const quint64 file = m_file;
    QPointer<PdfView> self(this);
    pagesPool()->start([self, file, path, now = m_fileNow] {
        if (now->load() != file)
            return;
        const PdfPages::Info info = PdfPages::info(path);
        QMetaObject::invokeMethod(qApp, [self, file, info] {
            if (!self || file != self->m_file)
                return;
            if (!info.ok) {
                emit self->failed();
                return;
            }
            self->m_pages = info.pages;
            self->m_pageSize = info.pageSize;
            self->layoutPages();
            self->verticalScrollBar()->setValue(0);
            emit self->loaded(info.pages);
            self->tellPage();
            self->want();
            self->viewport()->update();
        }, Qt::QueuedConnection);
    });
}

int PdfView::pageWidth() const
{
    return qMax(32, int((viewport()->width() - 2 * kMargin) * m_zoom));
}

int PdfView::pageHeight() const
{
    return qMax(32, int(pageWidth() * m_pageSize.height() / m_pageSize.width()));
}

int PdfView::pageTop(int number) const
{
    return kMargin + (number - 1) * (pageHeight() + kGap);
}

int PdfView::currentPage() const
{
    if (m_pages <= 0)
        return 0;
    // The page that has the middle of the pane's upper third.
    const int at = verticalScrollBar()->value() + viewport()->height() / 3;
    return qBound(1, 1 + (at - kMargin) / (pageHeight() + kGap), m_pages);
}

void PdfView::layoutPages()
{
    const int height = m_pages > 0 ? 2 * kMargin + m_pages * pageHeight() + (m_pages - 1) * kGap : 0;
    verticalScrollBar()->setRange(0, qMax(0, height - viewport()->height()));
    verticalScrollBar()->setPageStep(viewport()->height());
    verticalScrollBar()->setSingleStep(40);
    const int width = pageWidth() + 2 * kMargin;
    horizontalScrollBar()->setRange(0, qMax(0, width - viewport()->width()));
    horizontalScrollBar()->setPageStep(viewport()->width());
    horizontalScrollBar()->setSingleStep(40);
}

void PdfView::setZoom(qreal zoom)
{
    zoom = qBound(1.0, zoom, kMaxZoom);
    if (qFuzzyCompare(zoom, m_zoom) || m_pages <= 0)
        return;
    // The same place of the same page stays in the middle.
    const int page = currentPage();
    const qreal into = qreal(verticalScrollBar()->value() - pageTop(page)) / pageHeight();
    m_zoom = zoom;
    layoutPages();
    verticalScrollBar()->setValue(pageTop(page) + int(into * pageHeight()));
    m_again->start(); // made again at this size; until then the old pictures, stretched
    viewport()->update();
}

void PdfView::showPage(int number)
{
    if (m_pages <= 0)
        return;
    verticalScrollBar()->setValue(pageTop(qBound(1, number, m_pages)) - kMargin);
}

void PdfView::tellPage()
{
    const int page = currentPage();
    if (page == m_told)
        return;
    m_told = page;
    emit pageChanged(page, m_pages);
}

// The pages in sight, then the one after and the one before: what the
// reader gets to next.
void PdfView::want()
{
    m_wanted.clear();
    if (m_pages <= 0 || !isVisible())
        return;
    const int step = pageHeight() + kGap;
    const int top = verticalScrollBar()->value();
    const int first = qBound(1, 1 + (top - kMargin) / step, m_pages);
    const int last = qBound(1, 1 + (top + viewport()->height() - kMargin) / step, m_pages);
    const int width = qMin(PdfPages::MaxWidth, int(pageWidth() * devicePixelRatioF()));
    QList<int> order;
    for (int page = first; page <= last; ++page)
        order << page;
    if (last < m_pages)
        order << last + 1;
    if (first > 1)
        order << first - 1;
    for (int page : std::as_const(order)) {
        const auto shown = m_shown.constFind(page);
        if ((shown == m_shown.constEnd() || shown->width != width) && !m_making.contains(page))
            m_wanted << page;
    }
    // Pictures of pages far from here make room for these.
    if (m_shown.size() > kKept) {
        const QList<int> have = m_shown.keys();
        for (int page : have) {
            if (page < first - kKept / 2 || page > last + kKept / 2)
                m_shown.remove(page);
        }
    }
    pump();
}

void PdfView::pump()
{
    while (!m_wanted.isEmpty() && m_making.size() < kAtOnce) {
        const int page = m_wanted.takeFirst();
        m_making.insert(page);
        const quint64 file = m_file;
        const QString path = m_path;
        const int width = qMin(PdfPages::MaxWidth, int(pageWidth() * devicePixelRatioF()));
        QPointer<PdfView> self(this);
        pagesPool()->start([self, file, path, page, width, now = m_fileNow] {
            if (now->load() != file)
                return; // the reader has moved on (or is gone): not drawn
            const QImage image = PdfPages::page(path, page, width);
            QMetaObject::invokeMethod(qApp, [self, file, page, width, image] {
                if (self)
                    self->pageMade(file, page, width, image);
            }, Qt::QueuedConnection);
        });
    }
}

void PdfView::pageMade(quint64 file, int number, int width, const QImage &image)
{
    if (file != m_file)
        return; // of a file we have left
    m_making.remove(number);
    if (!image.isNull()) {
        Shown shown;
        shown.picture = QPixmap::fromImage(image);
        shown.width = width;
        m_shown.insert(number, shown);
        viewport()->update();
    }
    want(); // what is in sight now (it may have moved on meanwhile)
}

void PdfView::paintEvent(QPaintEvent *)
{
    QPainter p(viewport());
    if (m_pages <= 0)
        return;
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const Theme theme = currentTheme();
    const int step = pageHeight() + kGap;
    const int top = verticalScrollBar()->value();
    const int x = (pageWidth() + 2 * kMargin <= viewport()->width() ? (viewport()->width() - pageWidth()) / 2 : kMargin)
                  - horizontalScrollBar()->value();
    const int first = qBound(1, 1 + (top - kMargin) / step, m_pages);
    for (int page = first; page <= m_pages; ++page) {
        const QRect sheet(x, pageTop(page) - top, pageWidth(), pageHeight());
        if (sheet.top() > viewport()->height())
            break;
        const auto shown = m_shown.constFind(page);
        if (shown != m_shown.constEnd()) {
            p.fillRect(sheet, Qt::white);
            p.drawPixmap(sheet, shown->picture);
        } else {
            // Not made yet: a sheet of paper with its number.
            p.fillRect(sheet, Qt::white);
            p.setPen(QColor(0x9a, 0x9a, 0x9a));
            p.drawText(sheet, Qt::AlignCenter, tr("Page %1").arg(page));
        }
        p.setPen(theme.border);
        p.drawRect(sheet.adjusted(0, 0, -1, -1));
    }
}

void PdfView::resizeEvent(QResizeEvent *event)
{
    QAbstractScrollArea::resizeEvent(event);
    const int page = currentPage();
    layoutPages();
    if (page > 0 && verticalScrollBar()->value() > 0)
        verticalScrollBar()->setValue(pageTop(page) - kMargin); // the page that was read is still the one
    m_again->start();
}

void PdfView::scrollContentsBy(int, int)
{
    viewport()->update();
    tellPage();
    want();
}

void PdfView::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        // A notch of the wheel a step (touchpads send many small ones).
        m_wheel += event->angleDelta().y();
        const int notches = m_wheel / 120;
        if (notches != 0) {
            m_wheel -= notches * 120;
            setZoom(m_zoom + 0.25 * notches);
        }
        event->accept();
        return;
    }
    QAbstractScrollArea::wheelEvent(event);
}
