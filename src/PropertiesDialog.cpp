#include "PropertiesDialog.h"

#include "ContentType.h"

#include "Format.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QLocale>
#include <QMimeDatabase>
#include <QPointer>
#include <QPushButton>
#include <QStorageInfo>
#include <QThreadPool>
#include <QVBoxLayout>

#if defined(Q_OS_UNIX)
#include <unistd.h>
#endif

namespace {

// Access levels as Nautilus words them. Bits are r=4, w=2, x=1.
struct Level
{
    const char *text;
    int bits;
};
const Level kFileLevels[] = {
    {QT_TRANSLATE_NOOP("PropertiesDialog", "Read and write"), 6},
    {QT_TRANSLATE_NOOP("PropertiesDialog", "Read-only"), 4},
    {QT_TRANSLATE_NOOP("PropertiesDialog", "None"), 0},
};
const Level kFolderLevels[] = {
    {QT_TRANSLATE_NOOP("PropertiesDialog", "Create and delete files"), 7},
    {QT_TRANSLATE_NOOP("PropertiesDialog", "Access files"), 5},
    {QT_TRANSLATE_NOOP("PropertiesDialog", "List files only"), 4},
    {QT_TRANSLATE_NOOP("PropertiesDialog", "None"), 0},
};

QString sizeText(qint64 bytes)
{
    if (bytes < 1000)
        return formatSize(bytes);
    return PropertiesDialog::tr("%1 (%2 bytes)").arg(formatSize(bytes), QLocale().toString(bytes));
}

QString dateText(const QDateTime &when)
{
    return when.isValid() ? QLocale().toString(when, QLocale::LongFormat) : PropertiesDialog::tr("Unknown");
}

QLabel *value(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName("propValue");
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setWordWrap(true);
    return label;
}

bool ownedByMe(const QFileInfo &info)
{
#if defined(Q_OS_UNIX)
    return info.ownerId() == ::getuid();
#else
    return info.isWritable();
#endif
}

} // namespace

PropertiesDialog::PropertiesDialog(const QStringList &paths, QWidget *parent)
    : QDialog(parent)
    , m_paths(paths)
{
    setObjectName("propertiesDialog");
    setAttribute(Qt::WA_DeleteOnClose);
    const QFileInfo first(paths.value(0));
    const bool single = paths.size() == 1;
    m_isDir = single && first.isDir();
    setWindowTitle(single ? tr("“%1” Properties").arg(first.fileName().isEmpty() ? first.absoluteFilePath() : first.fileName())
                          : tr("Properties"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 22, 24, 18);
    layout->setSpacing(14);

    // Header: big icon (thumbnail for images) and name.
    auto *header = new QHBoxLayout;
    auto *icon = new QLabel(this);
    icon->setFixedSize(64, 64);
    icon->setAlignment(Qt::AlignCenter);
    QPixmap pm;
    if (single && first.isFile()) {
        QImageReader reader(first.absoluteFilePath());
        if (reader.canRead() && reader.size().isValid()) {
            reader.setScaledSize(reader.size().scaled(64, 64, Qt::KeepAspectRatio));
            pm = QPixmap::fromImage(reader.read());
        }
    }
    if (pm.isNull())
        pm = QFileIconProvider().icon(single ? first : QFileInfo(first.absolutePath())).pixmap(64, 64);
    icon->setPixmap(pm);
    auto *name = new QLabel(single ? (first.fileName().isEmpty() ? QDir::toNativeSeparators(first.absoluteFilePath()) : first.fileName())
                                   : tr("%1 items").arg(paths.size()),
                            this);
    name->setObjectName("conflictTitle");
    name->setWordWrap(true);
    name->setTextInteractionFlags(Qt::TextSelectableByMouse);
    header->addWidget(icon);
    header->addSpacing(8);
    header->addWidget(name, 1);
    layout->addLayout(header);

    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);
    form->setHorizontalSpacing(14);
    form->setVerticalSpacing(8);
    auto row = [&](const QString &label, QWidget *field) {
        auto *l = new QLabel(label, this);
        l->setObjectName("propLabel");
        form->addRow(l, field);
    };

    static const QMimeDatabase db;
    if (single && !m_isDir && first.isFile()) {
        // What the name says vs. what the file really is.
        const ContentType::Result real = ContentType::inspect(first.absoluteFilePath());
        const QString named = db.mimeTypeForName(real.nameMime).comment();
        const QString actual = db.mimeTypeForName(real.contentMime).comment();
        switch (real.verdict) {
        case ContentType::Verdict::Mismatch:
        case ContentType::Verdict::Dangerous: {
            row(tr("Type"), value(actual, this));
            row(tr("Name says"), value(named, this));
            QLabel *warning = value(ContentType::describe(real), this);
            warning->setObjectName(real.verdict == ContentType::Verdict::Dangerous ? "propDanger"
                                                                                   : "propWarning");
            row(QString(), warning);
            break;
        }
        case ContentType::Verdict::Identified:
            row(tr("Type"), value(tr("%1 (recognised from its content)").arg(actual), this));
            break;
        default:
            row(tr("Type"), value(db.mimeTypeForName(ContentType::effectiveMime(real)).comment(), this));
            break;
        }
    } else if (single) {
        row(tr("Type"), value(m_isDir ? tr("Folder") : db.mimeTypeForFile(first).comment(), this));
        if (first.isSymLink())
            row(tr("Link to"), value(QDir::toNativeSeparators(first.symLinkTarget()), this));
    }
    m_size = value(tr("Calculating…"), this);
    row(m_isDir ? tr("Contents") : tr("Size"), m_size);
    {
        // Paths have nowhere to wrap: shorten the middle, full path on hover.
        const QString where = QDir::toNativeSeparators(first.absolutePath());
        QLabel *location = value(fontMetrics().elidedText(where, Qt::ElideMiddle, 300), this);
        location->setWordWrap(false);
        location->setToolTip(where);
        row(tr("Location"), location);
    }
    if (single) {
        row(tr("Modified"), value(dateText(first.lastModified()), this));
        row(tr("Accessed"), value(dateText(first.lastRead()), this));
        if (first.birthTime().isValid())
            row(tr("Created"), value(dateText(first.birthTime()), this));
    }
    const QStorageInfo storage(first.absolutePath());
    if (storage.isValid())
        row(tr("Free space"), value(tr("%1 of %2")
                                         .arg(formatSize(storage.bytesAvailable()),
                                              formatSize(storage.bytesTotal())),
                                     this));
    layout->addLayout(form);

    if (single)
        addPermissions(form, first.absoluteFilePath());

    auto *buttons = new QHBoxLayout;
    auto *close = new QPushButton(tr("Close"), this);
    close->setObjectName("conflictPrimary");
    close->setDefault(true);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    buttons->addStretch(1);
    buttons->addWidget(close);
    layout->addStretch(1);
    layout->addLayout(buttons);
    setMinimumWidth(440);

    measure();
}

PropertiesDialog::~PropertiesDialog()
{
    *m_cancel = true; // stop measuring folders nobody is looking at
}

// Total size and item count, off the GUI thread (big folders take a while).
void PropertiesDialog::measure()
{
    bool needsWalk = false;
    qint64 bytes = 0;
    for (const QString &path : std::as_const(m_paths)) {
        const QFileInfo info(path);
        if (info.isDir() && !info.isSymLink())
            needsWalk = true;
        else
            bytes += info.size();
    }
    if (!needsWalk) {
        m_size->setText(m_paths.size() == 1 ? sizeText(bytes)
                                            : tr("%1 items, totalling %2")
                                                  .arg(m_paths.size())
                                                  .arg(formatSize(bytes)));
        return;
    }

    QPointer<QLabel> label = m_size;
    const QStringList paths = m_paths;
    const bool singleFolder = m_isDir;
    const std::shared_ptr<std::atomic_bool> cancel = m_cancel;
    QThreadPool::globalInstance()->start([label, paths, singleFolder, cancel] {
        qint64 bytes = 0;
        qint64 items = 0;
        QElapsedTimer sinceUpdate;
        sinceUpdate.start();
        auto show = [&](bool done) {
            const QString text =
                QStringLiteral("%1%2")
                    .arg(items == 1 ? PropertiesDialog::tr("1 item, totalling %1")
                                          .arg(formatSize(bytes))
                                    : PropertiesDialog::tr("%1 items, totalling %2")
                                          .arg(items)
                                          .arg(formatSize(bytes)),
                         done ? QString() : QStringLiteral(" …"));
            QMetaObject::invokeMethod(qApp, [label, text] {
                if (label)
                    label->setText(text);
            }, Qt::QueuedConnection);
        };
        for (const QString &path : paths) {
            const QFileInfo info(path);
            if (!singleFolder)
                ++items; // the selected items themselves
            if (!info.isDir() || info.isSymLink()) {
                bytes += info.size();
                continue;
            }
            QDirIterator it(path, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                            QDirIterator::Subdirectories);
            while (it.hasNext()) {
                if (*cancel)
                    return;
                it.next();
                const QFileInfo entry = it.fileInfo();
                if (singleFolder)
                    ++items;
                if (entry.isFile() && !entry.isSymLink())
                    bytes += entry.size();
                if (sinceUpdate.elapsed() > 150) {
                    show(false);
                    sinceUpdate.restart();
                }
            }
        }
        if (!*cancel)
            show(true);
    });
}

// Owner / Group / Others as simple choices, plus "executable" for files.
void PropertiesDialog::addPermissions(QFormLayout *form, const QString &path)
{
    const QFileInfo info(path);
    if (info.isSymLink())
        return;
    const QFile::Permissions p = info.permissions();
    auto bits = [&](QFile::Permission r, QFile::Permission w, QFile::Permission x) {
        return (p & r ? 4 : 0) | (p & w ? 2 : 0) | (p & x ? 1 : 0);
    };
    const bool editable = ownedByMe(info);

    auto *heading = new QLabel(tr("Permissions"), this);
    heading->setObjectName("propHeading");
    form->addRow(heading);
    form->addRow(new QLabel(tr("Owner"), this), value(info.owner().isEmpty() ? tr("Unknown") : info.owner(), this));

    auto makeCombo = [&](int current) {
        auto *combo = new QComboBox(this);
        combo->setObjectName("propCombo");
        int best = -1;
        if (m_isDir) {
            for (const Level &level : kFolderLevels) {
                combo->addItem(tr(level.text), level.bits);
                if (best < 0 && (current & level.bits) == level.bits && (current & 7) >= level.bits)
                    best = combo->count() - 1;
            }
        } else {
            for (const Level &level : kFileLevels) {
                combo->addItem(tr(level.text), level.bits);
                if (best < 0 && (current & 6) == level.bits)
                    best = combo->count() - 1;
            }
        }
        if (best < 0) { // unusual combination (e.g. write-only): show it as is
            combo->addItem(tr("Special"), current);
            best = combo->count() - 1;
        }
        combo->setCurrentIndex(best);
        combo->setEnabled(editable);
        connect(combo, &QComboBox::activated, this, &PropertiesDialog::applyPermissions);
        return combo;
    };
    m_owner = makeCombo(bits(QFile::ReadOwner, QFile::WriteOwner, QFile::ExeOwner));
    m_group = makeCombo(bits(QFile::ReadGroup, QFile::WriteGroup, QFile::ExeGroup));
    m_others = makeCombo(bits(QFile::ReadOther, QFile::WriteOther, QFile::ExeOther));
    form->addRow(new QLabel(tr("Owner access"), this), m_owner);
    form->addRow(new QLabel(info.group().isEmpty() ? tr("Group access")
                                                   : tr("Group (%1)").arg(info.group()),
                            this),
                 m_group);
    form->addRow(new QLabel(tr("Others"), this), m_others);
    if (!m_isDir) {
        m_exec = new QCheckBox(tr("Allow executing file as program"), this);
        m_exec->setObjectName("conflictCheck");
        m_exec->setChecked(p & QFile::ExeOwner);
        m_exec->setEnabled(editable);
        connect(m_exec, &QCheckBox::toggled, this, &PropertiesDialog::applyPermissions);
        form->addRow(QString(), m_exec);
    }
    if (!editable)
        form->addRow(QString(), value(tr("Only the owner can change permissions."), this));
    for (QLabel *label : findChildren<QLabel *>()) {
        if (label->objectName().isEmpty())
            label->setObjectName("propLabel");
    }
}

void PropertiesDialog::applyPermissions()
{
    const QString path = m_paths.first();
    const int owner = m_owner->currentData().toInt();
    const int group = m_group->currentData().toInt();
    const int others = m_others->currentData().toInt();
    const bool exec = m_exec && m_exec->isChecked();
    auto perms = [](int bits, QFile::Permission r, QFile::Permission w, QFile::Permission x, bool addExec) {
        QFile::Permissions p;
        if (bits & 4)
            p |= r;
        if (bits & 2)
            p |= w;
        if ((bits & 1) || (addExec && (bits & 4)))
            p |= x;
        return p;
    };
    // Files: "executable" adds x for everyone who can read it.
    QFile::Permissions p = perms(owner, QFile::ReadOwner, QFile::WriteOwner, QFile::ExeOwner, exec)
                           | perms(group, QFile::ReadGroup, QFile::WriteGroup, QFile::ExeGroup, exec)
                           | perms(others, QFile::ReadOther, QFile::WriteOther, QFile::ExeOther, exec);
    if (p & QFile::ReadOwner)
        p |= QFile::ReadUser;
    if (p & QFile::WriteOwner)
        p |= QFile::WriteUser;
    if (p & QFile::ExeOwner)
        p |= QFile::ExeUser;
    if (!QFile::setPermissions(path, p))
        m_size->setText(m_size->text() + QLatin1Char('\n') + tr("Couldn't change the permissions."));
}
