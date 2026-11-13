#include "OperationsPanel.h"

#include "FileJob.h"
#include "FileOperations.h"

#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPointer>
#include <QProgressBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

constexpr int ShowDelayMs = 400;
constexpr int PanelWidth = 400;
constexpr int Margin = 10;

QString displayName(const QString &path)
{
    const QString name = QFileInfo(path).fileName();
    return name.isEmpty() ? QDir::toNativeSeparators(path) : name;
}

QString titleFor(const FileJob *job)
{
    const QList<Transfer> &t = job->transfers();
    const int n = int(t.size());
    const QString item = n == 1 ? displayName(t.first().source) : QString();
    const QString dest = t.isEmpty() ? QString() : displayName(t.first().targetDir);
    switch (job->kind()) {
    case FileJob::Kind::Copy:
        return n == 1 ? OperationsPanel::tr("Copying “%1” to “%2”").arg(item, dest)
                      : OperationsPanel::tr("Copying %n items to “%1”", nullptr, n).arg(dest);
    case FileJob::Kind::Move:
        return n == 1 ? OperationsPanel::tr("Moving “%1” to “%2”").arg(item, dest)
                      : OperationsPanel::tr("Moving %n items to “%1”", nullptr, n).arg(dest);
    case FileJob::Kind::Trash:
        return n == 1 ? OperationsPanel::tr("Moving “%1” to the Trash").arg(item)
                      : OperationsPanel::tr("Moving %n items to the Trash", nullptr, n);
    case FileJob::Kind::Restore:
        return n == 1 ? OperationsPanel::tr("Restoring “%1” from the Trash").arg(item)
                      : OperationsPanel::tr("Restoring %n items from the Trash", nullptr, n);
    }
    return {};
}

QString timeLeft(qint64 seconds)
{
    auto unit = [](qint64 n, const QString &one, const QString &many) {
        return n == 1 ? one : many.arg(n);
    };
    if (seconds < 60)
        return unit(qMax<qint64>(1, seconds), OperationsPanel::tr("1 second left"),
                    OperationsPanel::tr("%1 seconds left"));
    if (seconds < 3600)
        return unit((seconds + 30) / 60, OperationsPanel::tr("1 minute left"),
                    OperationsPanel::tr("%1 minutes left"));
    return unit((seconds + 1800) / 3600, OperationsPanel::tr("1 hour left"),
                OperationsPanel::tr("%1 hours left"));
}

} // namespace

OperationsPanel::OperationsPanel(FileOperations *operations, QWidget *parent)
    : QFrame(parent)
    , m_layout(new QVBoxLayout(this))
{
    setObjectName("opsPanel");
    m_layout->setContentsMargins(14, 10, 10, 10);
    m_layout->setSpacing(10);
    hide();

    connect(operations, &FileOperations::jobStarted, this, &OperationsPanel::addJob);
    connect(operations, &FileOperations::jobFinished, this, &OperationsPanel::removeJob);
}

void OperationsPanel::setTheme(const Theme &theme)
{
    m_theme = theme;
    for (const Row &row : std::as_const(m_rows))
        row.cancel->setIcon(glyphIcon(Glyph::Close, m_theme.fg));
}

void OperationsPanel::addJob(FileJob *job)
{
    Row row;
    row.widget = new QWidget(this);
    auto *h = new QHBoxLayout(row.widget);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(10);

    auto *text = new QVBoxLayout;
    text->setSpacing(4);
    row.title = new QLabel(titleFor(job), row.widget);
    row.title->setObjectName("opsTitle");
    row.title->setWordWrap(true);
    row.detail = new QLabel(tr("Preparing…"), row.widget);
    row.detail->setObjectName("opsDetail");
    row.bar = new QProgressBar(row.widget);
    row.bar->setObjectName("opsProgress");
    row.bar->setTextVisible(false);
    row.bar->setRange(0, 0); // busy until measured
    row.bar->setFixedHeight(6);
    text->addWidget(row.title);
    text->addWidget(row.bar);
    text->addWidget(row.detail);
    h->addLayout(text, 1);

    row.cancel = new QToolButton(row.widget);
    row.cancel->setObjectName("flat");
    row.cancel->setToolTip(tr("Cancel"));
    row.cancel->setIcon(glyphIcon(Glyph::Close, m_theme.fg));
    row.cancel->setIconSize(QSize(16, 16));
    row.cancel->setFixedSize(30, 30);
    row.cancel->setFocusPolicy(Qt::NoFocus);
    connect(row.cancel, &QToolButton::clicked, job, [job, row] {
        job->cancel();
        row.detail->setText(tr("Canceling…"));
        row.cancel->setEnabled(false);
    });
    h->addWidget(row.cancel, 0, Qt::AlignVCenter);

    row.widget->hide();
    m_layout->addWidget(row.widget);
    m_rows.insert(job, row);
    connect(job, &FileJob::progressChanged, this, [this, job] { updateJob(job); });

    // Only show jobs that take a noticeable time.
    QPointer<FileJob> guard(job);
    QTimer::singleShot(ShowDelayMs, this, [this, guard] {
        if (!guard || !m_rows.contains(guard))
            return;
        m_rows.value(guard).widget->show();
        updateVisibility();
    });
}

void OperationsPanel::updateJob(FileJob *job)
{
    const auto it = m_rows.constFind(job);
    if (it == m_rows.constEnd() || job->isCanceled())
        return;
    const Row &row = *it;
    const FileJob::Progress p = job->progress();
    if (p.measuring)
        return;

    const QLocale locale;
    if (job->kind() == FileJob::Kind::Trash) {
        row.bar->setRange(0, qMax(1, p.itemsTotal));
        row.bar->setValue(p.itemsDone);
        row.detail->setText(tr("%1 of %2 items").arg(p.itemsDone).arg(p.itemsTotal));
        return;
    }

    row.bar->setRange(0, 1000);
    row.bar->setValue(p.bytesTotal > 0 ? int(p.bytesDone * 1000 / p.bytesTotal) : 0);
    QString detail = tr("%1 of %2")
                         .arg(locale.formattedDataSize(p.bytesDone),
                              locale.formattedDataSize(p.bytesTotal));
    const qint64 elapsed = job->elapsedMs();
    if (elapsed > 1500 && p.bytesDone > 0 && p.bytesTotal > p.bytesDone) {
        const qint64 remaining =
            qint64(double(p.bytesTotal - p.bytesDone) * double(elapsed) / double(p.bytesDone) / 1000.0);
        detail += QStringLiteral(" — ") + timeLeft(remaining);
    }
    row.detail->setText(detail);
}

void OperationsPanel::removeJob(FileJob *job)
{
    const Row row = m_rows.take(job);
    if (row.widget)
        row.widget->deleteLater();
    // Let the layout drop the row before resizing.
    QTimer::singleShot(0, this, &OperationsPanel::updateVisibility);
}

void OperationsPanel::updateVisibility()
{
    bool any = false;
    for (const Row &row : std::as_const(m_rows))
        any = any || !row.widget->isHidden();
    setVisible(any);
    if (any) {
        reposition();
        raise();
    }
    emit layoutChanged();
}

void OperationsPanel::reposition()
{
    QWidget *host = parentWidget();
    if (!host)
        return;
    const int width = qMin(PanelWidth, host->width() - 2 * Margin);
    resize(width, sizeHint().height());
    move(Margin, host->height() - height() - Margin);
}
