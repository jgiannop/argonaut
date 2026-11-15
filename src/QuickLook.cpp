#include "QuickLook.h"

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

// Rich text from an extension, shown and nothing more: links don't go
// anywhere and nothing it names (pictures, style sheets) is loaded.
class RichView : public QTextBrowser
{
public:
    using QTextBrowser::QTextBrowser;

protected:
    QVariant loadResource(int, const QUrl &) override { return {}; }
};

QuickLook::QuickLook(Thumbnailer *thumbnailer, QWidget *parent)
    : QWidget(parent), m_thumbnailer(thumbnailer)
{
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

    m_pages = new QStackedWidget(this);
    m_picture = new PictureView(m_pages);
    m_movieLabel = new QLabel(m_pages);
    m_movieLabel->setAlignment(Qt::AlignCenter);
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
    info->addStretch();
    for (QWidget *page : {static_cast<QWidget *>(m_picture), static_cast<QWidget *>(m_movieLabel),
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

void QuickLook::showItem(const Item &item)
{
    m_item = item;
    m_title->setText(item.name);
    m_details->setText(item.details);
    m_note->clear();
    m_open->setEnabled(item.verdict != ContentType::Verdict::Dangerous);
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

void QuickLook::loadBuiltIn(quint64 generation)
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
        if (size.width() > box.width() || size.height() > box.height())
            reader.setScaledSize(size.scaled(box, Qt::KeepAspectRatio));
        QImage image = reader.read();
        if (image.isNull())
            return false;
        image.setDevicePixelRatio(devicePixelRatioF());
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

void QuickLook::showResult(const QString &title, const QString &details, const Extensions::PreviewReply &content)
{
    clear(); // no file: nothing to open, nothing still loading
    m_title->setText(title);
    m_details->setText(details);
    if (!applyExtension(content))
        showInfo(tr("Nothing to show"));
}

QString QuickLook::shownText() const
{
    if (m_pages->currentWidget() == m_rich)
        return m_rich->toPlainText();
    return m_pages->currentWidget() == m_text ? m_text->toPlainText() : QString();
}

void QuickLook::clear()
{
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
    ++m_generation; // drop what's still loading
    m_extensionTimer->stop();
    m_extensionRequest = 0;
    if (m_movie)
        m_movie->stop();
    QWidget::hideEvent(event);
}
