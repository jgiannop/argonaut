#include "QuickLook.h"
#include "TipPicture.h"

#include "Format.h"
#include "RichTextStyle.h"
#include "Theme.h"
#include "Thumbnailer.h"

#include <QApplication>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMovie>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QImageReader>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QRegularExpression>
#include "PdfPages.h"
#include "PdfView.h"
#include "PlanView.h"
#include "RichView.h"

#include <QKeyEvent>
#include <QMimeDatabase>
#include <QLineEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QThreadPool>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

// A picture scaled to fit, centred, never enlarged past 2x (small images
// stay crisp instead of becoming a blur).
class PictureView : public QWidget
{
public:
    using QWidget::QWidget;
    void setImage(const QImage &image)
    {
        m_pixmap = QPixmap::fromImage(image);
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        if (m_pixmap.isNull())
            return;
        QPainter p(this);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        const qreal dpr = devicePixelRatioF();
        QSizeF size = QSizeF(m_pixmap.size()) / dpr;
        const QSizeF room = QSizeF(rect().size()) - QSizeF(16, 16);
        if (size.width() > room.width() || size.height() > room.height())
            size.scale(room, Qt::KeepAspectRatio);
        else if (size.width() * 2 < room.width() && size.height() * 2 < room.height())
            size *= 2;
        const QRectF target(QPointF((width() - size.width()) / 2, (height() - size.height()) / 2), size);
        p.drawPixmap(target, m_pixmap, QRectF(m_pixmap.rect()));
    }

private:
    QPixmap m_pixmap;
};

QuickLook::QuickLook(Thumbnailer *thumbnailer, QWidget *parent)
    : QWidget(parent), m_thumbnailer(thumbnailer)
{
    m_resultTimer = new QTimer(this);
    m_resultTimer->setSingleShot(true);
    m_resultTimer->setInterval(200);
    connect(m_resultTimer, &QTimer::timeout, this, &QuickLook::reportResult);
    setObjectName("quickLook");
    setAttribute(Qt::WA_StyledBackground);
    setFocusPolicy(Qt::NoFocus); // the file view keeps the keyboard
    setMinimumWidth(240);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 14, 18, 14);
    layout->setSpacing(8);
    m_title = new QLabel(this);
    m_title->setObjectName("qlTitle");
    m_title->setTextInteractionFlags(Qt::NoTextInteraction);
    m_details = new QLabel(this);
    m_details->setObjectName("qlDetails");
    // A long name is cut off; it must not make the pane wider.
    m_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_details->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_details->setWordWrap(true);
    auto *close = new QToolButton(this);
    close->setObjectName("qlClose");
    close->setText(QStringLiteral("✕"));
    close->setToolTip(tr("Close Preview (Space)"));
    close->setFocusPolicy(Qt::NoFocus);
    close->setAutoRaise(true);
    connect(close, &QToolButton::clicked, this, &QuickLook::closeRequested);
    auto *top = new QHBoxLayout;
    top->addWidget(m_title, 1);
    top->addWidget(close);
    layout->addLayout(top);
    layout->addWidget(m_details);
    // A search field, for a result its extension says can be searched (a
    // list of processes). The pane itself never takes the keyboard; this
    // field does when it is clicked, and gives it back on Esc or Enter.
    m_search = new QLineEdit(this);
    m_search->setObjectName("searchEdit");
    m_search->setClearButtonEnabled(true);
    m_search->setFocusPolicy(Qt::ClickFocus);
    m_search->setMaxLength(200);
    m_search->setFixedHeight(32);
    m_search->hide();
    m_search->installEventFilter(this);
    layout->addWidget(m_search);
    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(150);
    connect(m_search, &QLineEdit::textChanged, m_searchTimer, qOverload<>(&QTimer::start));
    connect(m_searchTimer, &QTimer::timeout, this, [this] {
        if (!m_resultId.isEmpty() && m_search->isVisibleTo(this))
            Extensions::Host::instance()->searchResult(m_resultExtension, m_resultId, m_search->text());
    });

    m_pages = new QStackedWidget(this);
    m_picture = new PictureView(m_pages);
    m_movieLabel = new QLabel(m_pages);
    m_movieLabel->setAlignment(Qt::AlignCenter);
    m_pdf = new PdfView(m_pages);
    m_pdf->setObjectName("qlPdf");
    connect(m_pdf, &PdfView::pageChanged, this, [this](int page, int pages) {
        if (m_pages->currentWidget() == m_pdf)
            m_note->setText(pages == 1 ? tr("1 page") : tr("Page %1 of %2").arg(page).arg(pages));
    });
    connect(m_pdf, &PdfView::loaded, this, [this] {
        if (m_pdf->path() == m_item.path) {
            m_kind = Preview::Kind::Image;
            m_pages->setCurrentWidget(m_pdf);
        }
    });
    // Not readable as a PDF after all (damaged, or locked with a password):
    // what any other file gets.
    connect(m_pdf, &PdfView::failed, this, [this] {
        if (m_pdf->path() != m_item.path)
            return;
        m_pdf->clear();
        loadPreview(m_generation);
    });
    // Changes an extension proposes (a plan): its moves to tick, Dismiss and Apply.
    m_plan = new PlanView(m_pages);
    connect(m_plan, &PlanView::applyRequested, this, [this] {
        if (m_plan->isEmpty())
            return;
        const Extensions::Plan plan = m_plan->plan();
        const QList<Extensions::PlanMove> kept = m_plan->keptMoves();
        closePlan(false); // the pane is free again before anything moves
        emit planApplied(plan, kept);
    });
    connect(m_plan, &PlanView::dismissRequested, this, [this] { closePlan(true); });
    connect(m_plan, &PlanView::itemActivated, this, &QuickLook::planItemActivated);
    m_scroll = new QScrollArea(m_pages);
    m_scroll->setObjectName("qlScroll");
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setFocusPolicy(Qt::NoFocus);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    m_scroll->viewport()->setAutoFillBackground(false);
    m_scrollPicture = new TipPicture;
    m_scroll->setWidget(m_scrollPicture);
    m_text = new QPlainTextEdit(m_pages);
    m_text->setObjectName("qlText");
    m_text->setReadOnly(true);
    m_text->setFocusPolicy(Qt::NoFocus); // keys stay with the file view
    m_text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_rich = new RichView(m_pages);
    m_rich->setObjectName("qlText");
    m_rich->setFocusPolicy(Qt::NoFocus);
    m_rich->setOpenLinks(false);
    m_rich->setOpenExternalLinks(false);
    m_rich->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_archive = new QTreeWidget(m_pages);
    m_archive->setObjectName("qlArchive");
    m_archive->setFocusPolicy(Qt::NoFocus);
    m_archive->setHeaderLabels({tr("Name"), tr("Size")});
    m_archive->setRootIsDecorated(false);
    m_archive->setUniformRowHeights(true);
    m_archive->header()->setStretchLastSection(false);
    m_archive->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_archive->header()->resizeSection(1, 110);
    m_infoPage = new QWidget(m_pages);
    auto *info = new QVBoxLayout(m_infoPage);
    info->addStretch();
    m_infoIcon = new QLabel(m_infoPage);
    m_infoIcon->setAlignment(Qt::AlignCenter);
    m_infoText = new QLabel(m_infoPage);
    m_infoText->setObjectName("qlDetails");
    m_infoText->setAlignment(Qt::AlignCenter);
    m_infoText->setWordWrap(true);
    info->addWidget(m_infoIcon);
    info->addSpacing(10);
    info->addWidget(m_infoText);
    // Only while something is waited for (showWorking).
    m_stopWaiting = new QPushButton(tr("Stop Waiting"), m_infoPage);
    m_stopWaiting->setObjectName("prefButton");
    m_stopWaiting->setFocusPolicy(Qt::NoFocus);
    m_stopWaiting->hide();
    info->addSpacing(14);
    info->addWidget(m_stopWaiting, 0, Qt::AlignHCenter);
    info->addStretch();
    connect(m_stopWaiting, &QPushButton::clicked, this, [this] {
        if (!m_workingFor.isEmpty())
            emit workingStopRequested(m_workingFor);
    });
    m_workingTimer = new QTimer(this);
    m_workingTimer->setInterval(400);
    connect(m_workingTimer, &QTimer::timeout, this, &QuickLook::updateWorking);
    for (QWidget *page : {static_cast<QWidget *>(m_picture), static_cast<QWidget *>(m_movieLabel), static_cast<QWidget *>(m_scroll), static_cast<QWidget *>(m_pdf), static_cast<QWidget *>(m_plan),
                          static_cast<QWidget *>(m_text), static_cast<QWidget *>(m_rich), static_cast<QWidget *>(m_archive), m_infoPage})
        m_pages->addWidget(page);
    layout->addWidget(m_pages, 1);

    auto *bottom = new QHBoxLayout;
    m_note = new QLabel(this);
    m_note->setObjectName("qlDetails");
    m_note->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    bottom->addWidget(m_note, 1);
    m_open = new QPushButton(tr("Open"), this);
    m_open->setObjectName("prefButton");
    m_open->setFocusPolicy(Qt::NoFocus);
    connect(m_open, &QPushButton::clicked, this, [this] { emit openRequested(m_item.path); });
    bottom->addWidget(m_open);
    layout->addLayout(bottom);

    // An extension that previews this kind of file is asked first, briefly.
    m_extensionTimer = new QTimer(this);
    m_extensionTimer->setSingleShot(true);
    m_extensionTimer->setInterval(4000);
    connect(m_extensionTimer, &QTimer::timeout, this, [this] {
        m_extensionRequest = 0;
        loadBuiltIn(m_generation); // it didn't answer: ours instead
    });
    connect(Extensions::Host::instance(), &Extensions::Host::previewReady, this, &QuickLook::extensionAnswered);
    clear();
}

QSize QuickLook::pictureBox() const
{
    // Device pixels the picture can use (at least a good size, before the
    // window is laid out the first time).
    const qreal dpr = devicePixelRatioF();
    const QSize area = m_pages->size().expandedTo(QSize(720, 440));
    return (QSizeF(area) * dpr).toSize();
}

// Something an extension waits for (Claude's answer): the pane says so,
// with dots that move and the seconds, so the wait is seen to go on.
void QuickLook::showWorking(const QString &extension, const QString &title, const QString &text)
{
    if (hasPlan())
        return;
    if (m_workingFor != extension) {
        endWorking(m_workingFor);
        clear();
        m_workingSince.start();
    }
    m_workingFor = extension;
    m_workingText = text;
    m_open->hide();
    m_title->setText(title);
    m_details->clear();
    m_note->clear();
    m_infoIcon->clear();
    m_infoText->setStyleSheet(QString());
    m_stopWaiting->show();
    m_pages->setCurrentWidget(m_infoPage);
    m_workingTimer->start();
    updateWorking();
}

void QuickLook::endWorking(const QString &extension)
{
    if (m_workingFor.isEmpty() || m_workingFor != extension)
        return;
    forgetWorking();
    clear();
}

void QuickLook::forgetWorking()
{
    m_workingFor.clear();
    m_workingTimer->stop();
    m_stopWaiting->hide();
}

void QuickLook::updateWorking()
{
    if (m_workingFor.isEmpty())
        return;
    const qint64 seconds = m_workingSince.elapsed() / 1000;
    const QString dots(1 + int(m_workingSince.elapsed() / 400) % 3, QLatin1Char('.'));
    QString text = m_workingText + dots;
    if (seconds >= 3)
        text += QLatin1Char('\n') + tr("%1 seconds so far. It can take a minute.").arg(seconds);
    m_infoText->setText(text);
}

QString QuickLook::workingText() const
{
    return isWorking() ? m_infoText->text() : QString();
}

// A plan has the pane until it is applied or dismissed: selecting files (to
// look at what a move is about) doesn't take it away.
bool QuickLook::showPlan(const Extensions::Plan &plan)
{
    if (hasPlan())
        return false; // one at a time
    endWorking(m_workingFor); // what was waited for is here
    clear();
    m_open->hide();
    m_title->setText(plan.title.isEmpty() ? tr("Proposed changes") : plan.title);
    m_details->setText(tr("From the extension “%1” · %2").arg(plan.extensionName, tildePath(plan.folder)));
    m_note->setText(plan.note);
    m_plan->setPlan(plan);
    m_pages->setCurrentWidget(m_plan);
    return true;
}

bool QuickLook::hasPlan() const
{
    return m_pages->currentWidget() == m_plan && !m_plan->isEmpty();
}

void QuickLook::closePlan(bool dismissed)
{
    if (m_plan->isEmpty())
        return;
    const Extensions::Plan plan = m_plan->plan();
    m_plan->clear();
    clear();
    if (dismissed)
        emit planDismissed(plan);
}

void QuickLook::showItem(const Item &item)
{
    if (isHeld())
        return; // the plan stays (it is answered with Apply or Dismiss), and so does "working"
    closeResult(); // a file takes the pane back
    m_pdf->clear();
    m_item = item;
    m_title->setText(item.name);
    m_details->setText(item.details);
    m_note->clear();
    m_open->setEnabled(item.verdict != ContentType::Verdict::Dangerous);
    m_open->show();
    if (m_movie) {
        m_movie->stop();
        m_movie->deleteLater();
        m_movie = nullptr;
    }
    // The icon at once; the real preview when it's ready.
    m_infoIcon->setPixmap(item.icon.pixmap(128, 128));
    showInfo(tr("Loading…"));

    const quint64 generation = ++m_generation;
    m_fromExtension = false;
    m_extensionTimer->stop();
    m_extensionRequest = 0;
    // Never a program in disguise: that one isn't handed to anybody.
    Extensions::Host *host = Extensions::Host::instance();
    // In an extension's list that previews its own files (Git: the file's
    // changes), that extension is the one asked, whatever the file is.
    if (item.verdict != ContentType::Verdict::Dangerous && !item.listing.isEmpty()
        && host->listingPreviews(item.listing)) {
        m_extensionRequest = host->requestListingPreview(item.listing, item.path, item.mimeType, pictureBox());
        if (m_extensionRequest != 0) {
            m_extensionTimer->start();
            return;
        }
    }
    if (item.verdict != ContentType::Verdict::Dangerous && host->canPreview(item.mimeType)) {
        m_extensionRequest = host->requestPreview(item.path, item.mimeType, pictureBox());
        if (m_extensionRequest != 0) {
            m_extensionTimer->start();
            return;
        }
    }
    loadBuiltIn(generation);
}

bool QuickLook::isReadingPdf() const
{
    return m_pages->currentWidget() == m_pdf;
}

void QuickLook::loadBuiltIn(quint64 generation)
{
    // A PDF is read, all of it, where the reader can be (poppler's tools
    // and a sandbox that works); never a program in disguise.
    if (m_item.verdict != ContentType::Verdict::Dangerous && PdfPages::available()
        && QMimeDatabase().mimeTypeForName(m_item.mimeType).inherits(QStringLiteral("application/pdf"))) {
        m_pdf->setFile(m_item.path); // loaded() shows it; failed() comes back for the usual preview
        return;
    }
    loadPreview(generation);
}

void QuickLook::loadPreview(quint64 generation)
{
    const Item item = m_item;
    QPointer<QuickLook> self(this);
    const QSize box = pictureBox();
    QThreadPool::globalInstance()->start([self, generation, item, box, thumbnailer = m_thumbnailer] {
        const Preview::Result result = Preview::load(item.path, item.mimeType, item.verdict, box, thumbnailer);
        QMetaObject::invokeMethod(qApp, [self, generation, result] {
            if (self)
                self->apply(generation, result);
        }, Qt::QueuedConnection);
    });
}

void QuickLook::extensionAnswered(int request, const Extensions::PreviewReply &reply)
{
    if (request == 0 || request != m_extensionRequest)
        return; // an answer for a file we have moved on from
    m_extensionRequest = 0;
    m_extensionTimer->stop();
    if (!isVisible())
        return;
    if (!applyExtension(reply))
        loadBuiltIn(m_generation); // "not from me", or nothing usable
}

bool QuickLook::applyExtension(const Extensions::PreviewReply &reply)
{
    using Extensions::PreviewReply;
    switch (reply.kind) {
    case PreviewReply::None:
        return false;
    case PreviewReply::Text:
        m_text->setFont(reply.monospace ? QFontDatabase::systemFont(QFontDatabase::FixedFont) : font());
        m_text->setPlainText(reply.text);
        m_text->setLineWrapMode(reply.monospace ? QPlainTextEdit::NoWrap : QPlainTextEdit::WidgetWidth);
        m_pages->setCurrentWidget(m_text);
        m_kind = Preview::Kind::Text;
        break;
    case PreviewReply::Ansi: {
        // Made for the terminal interface: here, its text without the colour codes.
        static const QRegularExpression codes(QStringLiteral("\x1b\\[[0-9;?]*[ -/]*[@-~]"));
        m_text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        m_text->setPlainText(QString(reply.text).remove(codes));
        m_text->setLineWrapMode(QPlainTextEdit::NoWrap);
        m_pages->setCurrentWidget(m_text);
        m_kind = Preview::Kind::Text;
        break;
    }
    case PreviewReply::Diff:
        RichTextStyle::setDiff(m_rich->document(), reply.text, currentTheme());
        m_rich->moveCursor(QTextCursor::Start); // from the top, not where the last line landed
        m_pages->setCurrentWidget(m_rich);
        m_kind = Preview::Kind::Text;
        break;
    case PreviewReply::Markdown:
    case PreviewReply::Html:
        if (reply.kind == PreviewReply::Markdown)
            m_rich->setMarkdown(reply.text);
        else
            m_rich->setHtml(reply.text);
        RichTextStyle::apply(m_rich->document(), currentTheme());
        m_pages->setCurrentWidget(m_rich);
        m_kind = Preview::Kind::Text;
        break;
    case PreviewReply::Image: {
        QImageReader reader(reply.image);
        reader.setAutoTransform(true);
        const QSize size = reader.size();
        if (!size.isValid() || size.width() > 16384 || size.height() > 16384)
            return false; // not a picture, or one too big to be a preview
        const QSize box = pictureBox();
        if (!reply.scroll && (size.width() > box.width() || size.height() > box.height()))
            reader.setScaledSize(size.scaled(box, Qt::KeepAspectRatio));
        QImage image = reader.read();
        if (image.isNull())
            return false;
        image.setDevicePixelRatio(devicePixelRatioF());
        if (reply.scroll) {
            // A long picture (a list drawn for the pane's width): at its own
            // size, scrolled. Made again in place, it stays where it was
            // scrolled to.
            const bool again = m_pages->currentWidget() == m_scroll;
            const int at = again ? m_scroll->verticalScrollBar()->value() : 0;
            m_scrollPicture->setPixmap(QPixmap::fromImage(image));
            m_scrollPicture->setFixedSize((QSizeF(image.size()) / devicePixelRatioF()).toSize());
            m_scrollPicture->setTips(reply.tips, devicePixelRatioF());
            m_pages->setCurrentWidget(m_scroll);
            m_scroll->verticalScrollBar()->setValue(at);
            m_kind = Preview::Kind::Image;
            break;
        }
        m_picture->setImage(image);
        m_pages->setCurrentWidget(m_picture);
        m_kind = Preview::Kind::Image;
        break;
    }
    }
    m_note->setText(reply.note);
    m_infoText->setStyleSheet(QString());
    m_fromExtension = true;
    return true;
}

bool QuickLook::showResult(const QString &title, const QString &details, const Extensions::PreviewReply &content)
{
    if (isHeld())
        return false; // not over a plan that waits for its answer, or over "working"
    const bool same = !content.resultId.isEmpty() && isVisible() && content.resultId == m_resultId
                      && content.extension == m_resultExtension;
    if (same) {
        // The result on screen, made again: replaced where it is.
        m_title->setText(title);
        applyExtension(content);
        return true;
    }
    if (content.update)
        return false; // for a result that isn't shown (any more)
    clear(); // no file: nothing to open, nothing still loading
    m_open->hide(); // …so no button that would open it
    m_title->setText(title);
    m_details->setText(details);
    if (!applyExtension(content)) {
        showInfo(tr("Nothing to show"));
        return true;
    }
    if (!content.resultId.isEmpty()) {
        m_resultExtension = content.extension;
        m_resultId = content.resultId;
        reportResult();
        if (content.searchable)
            setSearchShown(true, content.searchHint);
    }
    return true;
}

void QuickLook::reportResult()
{
    if (m_resultId.isEmpty())
        return;
    // Device pixels a picture can use here (PictureView keeps a margin).
    const QSize room = (QSizeF(m_pages->size() - QSize(16, 16)) * devicePixelRatioF()).toSize().expandedTo(QSize(64, 64));
    // The theme, so a picture drawn by the extension belongs in the window.
    // Translucent tokens are laid over the background: a picture has no "under".
    const Theme theme = currentTheme();
    auto solid = [&theme](const QColor &color) {
        const qreal a = color.alphaF();
        return QColor::fromRgbF(color.redF() * a + theme.window.redF() * (1 - a), color.greenF() * a + theme.window.greenF() * (1 - a),
                                color.blueF() * a + theme.window.blueF() * (1 - a))
            .name();
    };
    Extensions::ResultLook look;
    look.dark = theme.dark;
    look.window = solid(theme.window);
    look.surface = solid(theme.sidebar);
    look.text = solid(theme.fg);
    look.dim = solid(theme.dimFg);
    look.accent = solid(theme.accent);
    look.border = solid(theme.border);
    look.font = font().family();
    Extensions::Host::instance()->resultShown(m_resultExtension, m_resultId, true, room, look);
}

void QuickLook::setSearchShown(bool on, const QString &hint)
{
    if (!on) {
        const bool had = m_search->hasFocus();
        m_searchTimer->stop();
        const QSignalBlocker blocker(m_search);
        m_search->clear();
        m_search->hide();
        if (had)
            emit keysBackRequested();
        return;
    }
    m_search->setPlaceholderText(hint.isEmpty() ? tr("Search") : hint);
    m_search->show();
}

bool QuickLook::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_search && event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent *>(event)->key();
        if (key == Qt::Key_Escape && !m_search->text().isEmpty()) {
            m_search->clear(); // the first Esc: everything again
            return true;
        }
        if (key == Qt::Key_Escape || key == Qt::Key_Return || key == Qt::Key_Enter) {
            emit keysBackRequested();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void QuickLook::closeResult()
{
    setSearchShown(false);
    if (m_resultId.isEmpty())
        return;
    m_resultTimer->stop();
    Extensions::Host::instance()->resultShown(m_resultExtension, m_resultId, false);
    m_resultExtension.clear();
    m_resultId.clear();
}

void QuickLook::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    // Another theme: the extension draws its result again in the new colours.
    if (m_resultTimer && !m_resultId.isEmpty()
        && (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange || event->type() == QEvent::FontChange))
        m_resultTimer->start();
}

void QuickLook::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (!m_resultId.isEmpty())
        m_resultTimer->start(); // the extension draws for the new size
}

bool QuickLook::hasOpenButton() const
{
    return m_open->isVisibleTo(this);
}

int QuickLook::pictureScrollRange() const
{
    return m_pages->currentWidget() == m_scroll ? m_scroll->verticalScrollBar()->maximum() : -1;
}

int QuickLook::pictureScrollValue() const
{
    return m_pages->currentWidget() == m_scroll ? m_scroll->verticalScrollBar()->value() : -1;
}

void QuickLook::scrollPictureTo(int value)
{
    m_scroll->verticalScrollBar()->setValue(value);
}

QString QuickLook::shownText() const
{
    if (m_pages->currentWidget() == m_rich)
        return m_rich->toPlainText();
    return m_pages->currentWidget() == m_text ? m_text->toPlainText() : QString();
}

void QuickLook::clear()
{
    if (isHeld())
        return; // a plan waits for its answer, or something is waited for: nothing else takes the pane
    closeResult();
    m_pdf->clear();
    ++m_generation; // drop what's still loading
    m_extensionTimer->stop();
    m_extensionRequest = 0;
    m_fromExtension = false;
    m_item = Item();
    m_kind = Preview::Kind::Generic;
    if (m_movie) {
        m_movie->stop();
        m_movie->deleteLater();
        m_movie = nullptr;
    }
    m_title->setText(tr("Preview"));
    m_details->clear();
    m_note->clear();
    m_infoIcon->clear();
    m_infoText->setStyleSheet(QString());
    m_open->setEnabled(false);
    m_open->show();
    showInfo(tr("Select a file to preview it"));
}

void QuickLook::showInfo(const QString &message)
{
    m_infoText->setText(message);
    m_pages->setCurrentWidget(m_infoPage);
}

void QuickLook::apply(quint64 generation, const Preview::Result &result)
{
    if (generation != m_generation || !isVisible())
        return; // another file was asked for meanwhile
    m_kind = result.kind;
    switch (result.kind) {
    case Preview::Kind::Image:
        if (result.animated) {
            m_movie = new QMovie(m_item.path, QByteArray(), this);
            const QSize room = m_pages->size() - QSize(16, 16);
            QSize size = result.image.size();
            if (size.width() > room.width() || size.height() > room.height())
                size.scale(room, Qt::KeepAspectRatio);
            m_movie->setScaledSize(size);
            m_movieLabel->setMovie(m_movie);
            m_movie->start();
            m_pages->setCurrentWidget(m_movieLabel);
        } else {
            m_picture->setImage(result.image);
            m_pages->setCurrentWidget(m_picture);
        }
        break;
    case Preview::Kind::Text:
        m_text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        m_text->setPlainText(result.text);
        m_text->setLineWrapMode(m_item.mimeType == QLatin1String("text/plain")
                                        || m_item.mimeType == QLatin1String("text/markdown")
                                    ? QPlainTextEdit::WidgetWidth
                                    : QPlainTextEdit::NoWrap); // code keeps its lines
        m_pages->setCurrentWidget(m_text);
        if (result.truncated)
            m_note->setText(tr("Showing the first %1").arg(formatSize(Preview::MaxTextBytes)));
        break;
    case Preview::Kind::Archive: {
        m_archive->clear();
        QList<QTreeWidgetItem *> rows;
        const QIcon folder = style()->standardIcon(QStyle::SP_DirIcon);
        const QIcon file = style()->standardIcon(QStyle::SP_FileIcon);
        for (const FileJob::ArchiveEntry &e : result.entries) {
            auto *row = new QTreeWidgetItem({e.path, e.isDir || e.size < 0 ? QString() : formatSize(e.size)});
            row->setIcon(0, e.isDir ? folder : file);
            row->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
            rows << row;
        }
        m_archive->addTopLevelItems(rows);
        m_pages->setCurrentWidget(m_archive);
        m_note->setText(result.total == 1 ? tr("1 item inside")
                        : result.truncated ? tr("%1 items inside (showing the first %2)")
                                                 .arg(QLocale().toString(result.total))
                                                 .arg(result.entries.size())
                                           : tr("%1 items inside").arg(QLocale().toString(result.total)));
        break;
    }
    case Preview::Kind::Folder:
        showInfo(result.items == 1 ? tr("Folder with 1 item")
                 : result.items == 0 ? tr("Empty folder")
                                     : tr("Folder with %1 items").arg(QLocale().toString(result.items)));
        break;
    case Preview::Kind::Blocked:
        showInfo(tr("This is a program disguised as a document.\nArgonaut doesn't preview or run it."));
        m_infoText->setStyleSheet(QStringLiteral("color: #e01b24;"));
        return;
    case Preview::Kind::Generic:
        showInfo(tr("No preview for this kind of file"));
        break;
    }
    m_infoText->setStyleSheet(QString());
}

void QuickLook::hideEvent(QHideEvent *event)
{
    closePlan(true); // the pane was closed: the plan is not applied
    forgetWorking(); // nor is the waiting shown any more (it goes on: the plan will open the pane)
    closeResult();
    ++m_generation; // drop what's still loading
    m_extensionTimer->stop();
    m_extensionRequest = 0;
    if (m_movie)
        m_movie->stop();
    QWidget::hideEvent(event);
}
