#include "QuickLook.h"

#include "Format.h"
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
#include <QStackedWidget>
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
                          static_cast<QWidget *>(m_text), static_cast<QWidget *>(m_archive), m_infoPage})
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

void QuickLook::clear()
{
    ++m_generation; // drop what's still loading
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
    if (m_movie)
        m_movie->stop();
    QWidget::hideEvent(event);
}
