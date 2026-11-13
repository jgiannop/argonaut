#include "OpenWithDialog.h"

#include <QCheckBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

QIcon appIcon(const OpenWith::App &app)
{
    if (app.icon.startsWith(QLatin1Char('/')))
        return QIcon(app.icon);
    return QIcon::fromTheme(app.icon, QIcon::fromTheme(QStringLiteral("application-x-executable")));
}

OpenWithDialog::OpenWithDialog(const QStringList &files, const QString &mimeType,
                               const QString &typeName, QWidget *parent)
    : QDialog(parent)
{
    setObjectName("openWithDialog");
    setWindowTitle(tr("Open With"));
    QString defaultId;
    m_apps = OpenWith::appsFor(mimeType, &defaultId);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(22, 20, 22, 18);
    layout->setSpacing(10);
    auto *title = new QLabel(files.size() == 1
                                 ? tr("Open “%1” with").arg(QFileInfo(files.first()).fileName())
                                 : tr("Open %1 files with").arg(files.size()),
                             this);
    title->setObjectName("conflictTitle");
    title->setWordWrap(true);
    layout->addWidget(title);

    m_list = new QListWidget(this);
    m_list->setObjectName("appList");
    m_list->setIconSize(QSize(28, 28));
    m_list->setFrameShape(QFrame::NoFrame);
    for (const OpenWith::App &app : std::as_const(m_apps)) {
        auto *item = new QListWidgetItem(appIcon(app), app.id == defaultId
                                                           ? tr("%1 (default)").arg(app.name)
                                                           : app.name,
                                         m_list);
        item->setToolTip(app.id);
    }
    if (m_apps.isEmpty()) {
        auto *none = new QListWidgetItem(tr("No applications can open this type of file"), m_list);
        none->setFlags(Qt::NoItemFlags);
    }
    layout->addWidget(m_list, 1);

    m_default = new QCheckBox(tr("Always use for “%1” files").arg(typeName), this);
    m_default->setObjectName("conflictCheck");
    layout->addWidget(m_default);

    auto *buttons = new QHBoxLayout;
    auto *cancel = new QPushButton(tr("Cancel"), this);
    cancel->setObjectName("conflictButton");
    m_open = new QPushButton(tr("Open"), this);
    m_open->setObjectName("conflictPrimary");
    m_open->setDefault(true);
    m_open->setEnabled(false);
    buttons->addStretch(1);
    buttons->addWidget(cancel);
    buttons->addWidget(m_open);
    layout->addLayout(buttons);

    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_open, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_list, &QListWidget::currentRowChanged, this,
            [this](int row) { m_open->setEnabled(row >= 0 && row < m_apps.size()); });
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this] {
        if (m_open->isEnabled())
            accept();
    });
    if (!m_apps.isEmpty())
        m_list->setCurrentRow(0);
    resize(420, 460);
}

OpenWith::App OpenWithDialog::chosen() const
{
    const int row = m_list->currentRow();
    return row >= 0 && row < m_apps.size() ? m_apps.at(row) : OpenWith::App{};
}

bool OpenWithDialog::makeDefault() const
{
    return m_default->isChecked();
}
