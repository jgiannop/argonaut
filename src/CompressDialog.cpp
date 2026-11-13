#include "CompressDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

namespace {

struct FormatChoice
{
    FileJob::ArchiveFormat format;
    const char *label;
    const char *key; // QSettings value
};

const FormatChoice kFormats[] = {
    {FileJob::ArchiveFormat::Zip, QT_TRANSLATE_NOOP("CompressDialog", ".zip — works everywhere"), "zip"},
    {FileJob::ArchiveFormat::TarXz, QT_TRANSLATE_NOOP("CompressDialog", ".tar.xz — smaller, Linux and macOS"), "tar.xz"},
    {FileJob::ArchiveFormat::TarGz, QT_TRANSLATE_NOOP("CompressDialog", ".tar.gz — fast, Linux and macOS"), "tar.gz"},
    {FileJob::ArchiveFormat::SevenZip, QT_TRANSLATE_NOOP("CompressDialog", ".7z — smallest, needs 7-Zip on Windows"), "7z"},
};

} // namespace

CompressDialog::CompressDialog(const QStringList &paths, const QString &targetDir, QWidget *parent)
    : QDialog(parent), m_targetDir(targetDir)
{
    setWindowTitle(tr("Compress"));
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(tr("Archive name"), this));

    auto *row = new QHBoxLayout;
    m_name = new QLineEdit(this);
    // One item: its name ("Photos"); several: "Archive", like Nautilus.
    QString base = paths.size() == 1 ? QFileInfo(paths.first()).fileName() : tr("Archive");
    if (paths.size() == 1 && QFileInfo(paths.first()).isFile()) {
        const int dot = int(base.lastIndexOf(QLatin1Char('.')));
        if (dot > 0)
            base = base.left(dot);
    }
    m_name->setText(base);
    m_name->selectAll();
    row->addWidget(m_name, 1);
    m_format = new QComboBox(this);
    const QString saved = QSettings().value("archive/format", "zip").toString();
    for (const FormatChoice &choice : kFormats) {
        m_format->addItem(tr(choice.label), int(choice.format));
        if (saved == QLatin1String(choice.key))
            m_format->setCurrentIndex(m_format->count() - 1);
    }
    row->addWidget(m_format);
    layout->addLayout(row);

    m_problem = new QLabel(this);
    m_problem->setObjectName("dim");
    layout->addWidget(m_problem);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_create = buttons->addButton(tr("Create"), QDialogButtonBox::AcceptRole);
    m_create->setDefault(true);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        QSettings().setValue("archive/format", QLatin1String(kFormats[m_format->currentIndex()].key));
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_name, &QLineEdit::textChanged, this, &CompressDialog::validate);
    connect(m_format, &QComboBox::currentIndexChanged, this, &CompressDialog::validate);
    validate();
    resize(480, sizeHint().height());
}

QString CompressDialog::archiveName() const
{
    return m_name->text().trimmed() + FileJob::archiveSuffix(format());
}

FileJob::ArchiveFormat CompressDialog::format() const
{
    return FileJob::ArchiveFormat(m_format->currentData().toInt());
}

void CompressDialog::validate()
{
    const QString name = m_name->text().trimmed();
    QString problem;
    if (name.isEmpty())
        problem = tr("Type a name.");
    else if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')))
        problem = tr("Names can't contain “/” or “\\”.");
    else if (name == QLatin1String(".") || name == QLatin1String(".."))
        problem = tr("“%1” isn't a valid name.").arg(name);
    else if (QFileInfo::exists(m_targetDir + QLatin1Char('/') + archiveName()))
        problem = tr("“%1” already exists; a number will be added.").arg(archiveName());
    m_problem->setText(problem);
    m_problem->setVisible(!problem.isEmpty());
    m_create->setEnabled(problem.isEmpty() || problem.endsWith(tr("a number will be added.")));
}
