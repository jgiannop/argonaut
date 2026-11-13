#include "ConflictDialog.h"

#include <QCheckBox>
#include <QDateTime>
#include <QDir>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

constexpr int PreviewSize = 64;

bool isRealDir(const QFileInfo &info)
{
    return info.isDir() && !info.isSymLink();
}

// A small thumbnail for images, otherwise the file-type icon.
QPixmap previewOf(const QFileInfo &info, qreal dpr)
{
    const int px = qRound(PreviewSize * dpr);
    if (info.isFile()) {
        QImageReader reader(info.absoluteFilePath());
        reader.setAutoTransform(true);
        const QSize size = reader.size();
        if (reader.canRead() && size.isValid() && qint64(size.width()) * size.height() < 100'000'000) {
            reader.setScaledSize(size.scaled(px, px, Qt::KeepAspectRatio));
            QPixmap pm = QPixmap::fromImage(reader.read());
            if (!pm.isNull()) {
                pm.setDevicePixelRatio(dpr);
                return pm;
            }
        }
    }
    return QFileIconProvider().icon(info).pixmap(QSize(PreviewSize, PreviewSize), dpr);
}

QString describe(const QFileInfo &info)
{
    const QLocale locale;
    if (isRealDir(info)) {
        const int count = int(QDir(info.absoluteFilePath())
                                  .entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden
                                             | QDir::System)
                                  .size());
        return count == 1 ? ConflictDialog::tr("1 item") : ConflictDialog::tr("%1 items").arg(count);
    }
    return locale.formattedDataSize(info.size());
}

} // namespace

ConflictDialog::ConflictDialog(const QString &source, const QString &existing, QWidget *parent)
    : QDialog(parent)
{
    setObjectName("conflictDialog");
    const QFileInfo src(source);
    const QFileInfo dst(existing);
    const bool folders = isRealDir(src) && isRealDir(dst);
    const QString name = dst.fileName();
    const QString where = dst.absoluteDir().dirName().isEmpty()
                              ? QDir::toNativeSeparators(dst.absolutePath())
                              : dst.absoluteDir().dirName();

    setWindowTitle(folders ? tr("Merge Folder") : tr("Replace"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 22, 24, 20);
    layout->setSpacing(10);

    auto *title = new QLabel(folders ? tr("Merge folder “%1”?").arg(name)
                             : isRealDir(dst) ? tr("Replace folder “%1”?").arg(name)
                                              : tr("Replace file “%1”?").arg(name),
                             this);
    title->setObjectName("conflictTitle");
    title->setWordWrap(true);
    layout->addWidget(title);

    QString text = isRealDir(dst)
                       ? tr("Another folder with the same name already exists in “%1”.").arg(where)
                       : tr("Another file with the same name already exists in “%1”.").arg(where);
    text += QLatin1Char(' ')
            + (folders ? tr("Merging asks again before replacing anything inside it.")
                       : tr("Replacing it moves the existing one to the Trash."));
    auto *body = new QLabel(text, this);
    body->setObjectName("conflictText");
    body->setWordWrap(true);
    layout->addWidget(body);

    // Both items; the newer one is marked if the difference is real.
    const qint64 diff = dst.lastModified().msecsTo(src.lastModified()); // > 0: source newer
    constexpr qint64 Meaningful = 2000;
    auto *cards = new QHBoxLayout;
    cards->setSpacing(12);
    cards->addWidget(itemCard(tr("Existing"), existing, diff < -Meaningful));
    cards->addWidget(itemCard(folders ? tr("Merge with") : tr("Replace with"), source, diff > Meaningful));
    layout->addLayout(cards);

    // Keep both under a name of your choice.
    auto *nameLabel = new QLabel(tr("Or keep both, saving the new one as:"), this);
    nameLabel->setObjectName("conflictText");
    m_suggestedName = QFileInfo(uniqueTargetPath(dst.absolutePath(), name, isRealDir(src))).fileName();
    m_name = new QLineEdit(m_suggestedName, this);
    m_name->setObjectName("conflictName");
    layout->addSpacing(4);
    layout->addWidget(nameLabel);
    layout->addWidget(m_name);

    m_applyToAll = new QCheckBox(tr("Apply this action to all files and folders"), this);
    m_applyToAll->setObjectName("conflictCheck");
    layout->addWidget(m_applyToAll);

    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    auto *cancel = new QPushButton(tr("Cancel"), this);
    auto *skip = new QPushButton(tr("Skip"), this);
    auto *keepBoth = new QPushButton(tr("Keep Both"), this);
    auto *replace = new QPushButton(tr("Replace"), this);
    QPushButton *merge = folders ? new QPushButton(tr("Merge"), this) : nullptr;
    for (QPushButton *b : {cancel, skip, keepBoth, replace})
        b->setObjectName("conflictButton");
    QPushButton *primary = merge ? merge : replace;
    primary->setObjectName("conflictPrimary");
    primary->setDefault(true);

    buttons->addWidget(cancel);
    buttons->addStretch(1);
    buttons->addWidget(skip);
    buttons->addWidget(keepBoth);
    buttons->addWidget(replace);
    if (merge)
        buttons->addWidget(merge);
    layout->addSpacing(6);
    layout->addLayout(buttons);

    connect(cancel, &QPushButton::clicked, this, [this] { choose(FileJob::ConflictAction::Cancel); });
    connect(skip, &QPushButton::clicked, this, [this] { choose(FileJob::ConflictAction::Skip); });
    connect(keepBoth, &QPushButton::clicked, this, [this] { choose(FileJob::ConflictAction::KeepBoth); });
    connect(replace, &QPushButton::clicked, this, [this] { choose(FileJob::ConflictAction::Replace); });
    if (merge)
        connect(merge, &QPushButton::clicked, this, [this] { choose(FileJob::ConflictAction::Merge); });
    // Typing a name means "keep both with this name".
    connect(m_name, &QLineEdit::returnPressed, this, [this] { choose(FileJob::ConflictAction::KeepBoth); });

    setMinimumWidth(520);
}

QWidget *ConflictDialog::itemCard(const QString &heading, const QString &path, bool newer)
{
    const QFileInfo info(path);
    auto *card = new QWidget(this);
    card->setObjectName("conflictCard");
    card->setAttribute(Qt::WA_StyledBackground);
    auto *row = new QHBoxLayout(card);
    row->setContentsMargins(12, 10, 12, 10);
    row->setSpacing(12);

    auto *preview = new QLabel(card);
    preview->setFixedSize(PreviewSize, PreviewSize);
    preview->setAlignment(Qt::AlignCenter);
    preview->setPixmap(previewOf(info, devicePixelRatioF()));
    row->addWidget(preview);

    auto *text = new QVBoxLayout;
    text->setSpacing(2);
    auto *head = new QLabel(heading, card);
    head->setObjectName("conflictCardHeading");
    const QLocale locale;
    QString when = locale.toString(info.lastModified(), QLocale::ShortFormat);
    if (newer)
        when += QLatin1Char(' ') + tr("(newer)");
    auto *details = new QLabel(describe(info) + QLatin1Char('\n') + when, card);
    details->setObjectName("conflictText");
    text->addWidget(head);
    text->addWidget(details);
    text->addStretch(1);
    row->addLayout(text, 1);
    return card;
}

void ConflictDialog::choose(FileJob::ConflictAction action)
{
    m_resolution.action = action;
    m_resolution.applyToAll = m_applyToAll->isChecked();
    const QString typed = m_name->text().trimmed();
    if (action == FileJob::ConflictAction::KeepBoth && typed != m_suggestedName)
        m_resolution.newName = typed; // empty -> automatic "(copy)" name
    if (action == FileJob::ConflictAction::Cancel)
        reject();
    else
        accept();
}
