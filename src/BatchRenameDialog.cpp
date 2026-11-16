#include "BatchRenameDialog.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QCollator>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace {

enum Order { NameAscending, NameDescending, OldestFirst, NewestFirst };

} // namespace

BatchRenameDialog::BatchRenameDialog(const QStringList &paths, QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("Rename %1 Items").arg(paths.size()));
    for (const QString &path : paths) {
        const QFileInfo info(path);
        m_sources.append({path, info.fileName(), info.isDir() && !info.isSymLink(), info.lastModified()});
    }

    auto *layout = new QVBoxLayout(this);

    // Mode: template or find and replace.
    auto *modeRow = new QHBoxLayout;
    auto *templateMode = new QRadioButton(tr("Rename using a template"), this);
    auto *replaceMode = new QRadioButton(tr("Find and replace text"), this);
    m_mode = new QButtonGroup(this);
    m_mode->addButton(templateMode, 0);
    m_mode->addButton(replaceMode, 1);
    modeRow->addWidget(templateMode);
    modeRow->addWidget(replaceMode);
    modeRow->addStretch();
    layout->addLayout(modeRow);

    m_pages = new QStackedWidget(this);
    layout->addWidget(m_pages);

    // Template page.
    auto *templatePage = new QWidget(m_pages);
    auto *templateForm = new QFormLayout(templatePage);
    templateForm->setContentsMargins(0, 0, 0, 0);
    auto *templateRow = new QHBoxLayout;
    m_template = new QLineEdit(templatePage);
    m_template->setObjectName(QStringLiteral("template"));
    m_template->setClearButtonEnabled(true);
    templateRow->addWidget(m_template, 1);
    auto *addTag = new QToolButton(templatePage);
    addTag->setText(tr("Add"));
    addTag->setPopupMode(QToolButton::InstantPopup);
    auto *tagMenu = new QMenu(addTag);
    tagMenu->addAction(tr("Original name"), this, [this] { insertTag(Naming::originalNameTag()); });
    tagMenu->addSeparator();
    const QStringList numbers = Naming::numberTags();
    const QStringList examples = {tr("1, 2, 3"), tr("01, 02, 03"), tr("001, 002, 003")};
    for (int i = 0; i < numbers.size(); ++i)
        tagMenu->addAction(tr("Number: %1").arg(examples[i]), this,
                           [this, tag = numbers[i]] { insertTag(tag); });
    addTag->setMenu(tagMenu);
    templateRow->addWidget(addTag);
    templateForm->addRow(tr("Name"), templateRow);

    auto *numberRow = new QHBoxLayout;
    m_start = new QSpinBox(templatePage);
    m_start->setRange(0, 999999);
    m_start->setValue(1);
    numberRow->addWidget(m_start);
    numberRow->addSpacing(12);
    numberRow->addWidget(new QLabel(tr("Order"), templatePage));
    m_order = new QComboBox(templatePage);
    m_order->addItem(tr("Name (A–Z)"), NameAscending);
    m_order->addItem(tr("Name (Z–A)"), NameDescending);
    m_order->addItem(tr("Modified, oldest first"), OldestFirst);
    m_order->addItem(tr("Modified, newest first"), NewestFirst);
    numberRow->addWidget(m_order, 1);
    templateForm->addRow(tr("Start at"), numberRow);
    m_pages->addWidget(templatePage);

    // Find and replace page.
    auto *replacePage = new QWidget(m_pages);
    auto *replaceForm = new QFormLayout(replacePage);
    replaceForm->setContentsMargins(0, 0, 0, 0);
    m_find = new QLineEdit(replacePage);
    m_replace = new QLineEdit(replacePage);
    m_caseSensitive = new QCheckBox(tr("Match case"), replacePage);
    replaceForm->addRow(tr("Find"), m_find);
    replaceForm->addRow(tr("Replace with"), m_replace);
    replaceForm->addRow(QString(), m_caseSensitive);
    m_pages->addWidget(replacePage);

    // Preview.
    m_preview = new QTreeWidget(this);
    m_preview->setColumnCount(2);
    m_preview->setHeaderLabels({tr("Original name"), tr("New name")});
    m_preview->setRootIsDecorated(false);
    m_preview->setSelectionMode(QAbstractItemView::NoSelection);
    m_preview->setFocusPolicy(Qt::NoFocus);
    m_preview->header()->setSectionResizeMode(QHeaderView::Stretch);
    m_preview->header()->setSectionsMovable(false);
    m_preview->setMinimumHeight(220);
    layout->addWidget(m_preview, 1);

    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    layout->addWidget(m_summary);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_rename = buttons->addButton(tr("Rename"), QDialogButtonBox::AcceptRole);
    m_rename->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    // Remember the last choices, like Nautilus.
    QSettings settings;
    m_template->setText(settings.value(QStringLiteral("rename/template"),
                                       Naming::originalNameTag() + QStringLiteral(" [01]"))
                            .toString());
    m_order->setCurrentIndex(qBound(0, settings.value(QStringLiteral("rename/order")).toInt(), 3));
    const int mode = settings.value(QStringLiteral("rename/mode")).toInt() == 1 ? 1 : 0;
    m_mode->button(mode)->setChecked(true);
    // Only the shown page takes room (the stack would use the tallest).
    auto showPage = [this](int id) {
        m_pages->setCurrentIndex(id);
        for (int i = 0; i < m_pages->count(); ++i)
            m_pages->widget(i)->setSizePolicy(QSizePolicy::Preferred,
                                              i == id ? QSizePolicy::Preferred : QSizePolicy::Ignored);
        m_pages->updateGeometry();
    };
    showPage(mode);

    connect(m_mode, &QButtonGroup::idClicked, this, [this, showPage](int id) {
        showPage(id);
        (id == 0 ? m_template : m_find)->setFocus();
        updatePreview();
    });
    for (QLineEdit *edit : {m_template, m_find, m_replace})
        connect(edit, &QLineEdit::textChanged, this, &BatchRenameDialog::updatePreview);
    connect(m_start, &QSpinBox::valueChanged, this, &BatchRenameDialog::updatePreview);
    connect(m_caseSensitive, &QCheckBox::toggled, this, &BatchRenameDialog::updatePreview);
    connect(m_order, &QComboBox::currentIndexChanged, this, [this] {
        sortSources();
        updatePreview();
    });
    connect(this, &QDialog::accepted, this, [this] {
        QSettings remembered;
        remembered.setValue(QStringLiteral("rename/mode"), m_mode->checkedId());
        remembered.setValue(QStringLiteral("rename/template"), m_template->text());
        remembered.setValue(QStringLiteral("rename/order"), m_order->currentIndex());
    });

    sortSources();
    updatePreview();
    (mode == 0 ? m_template : m_find)->setFocus();
    resize(640, 480);
}

QList<Naming::RenameItem> BatchRenameDialog::items() const
{
    return m_items;
}

void BatchRenameDialog::insertTag(const QString &tag)
{
    m_template->insert(tag);
    m_template->setFocus();
}

void BatchRenameDialog::sortSources()
{
    QLocale locale;
    if (locale.language() == QLocale::C)
        locale = QLocale(QLocale::English); // numeric order needs a real locale
    QCollator collator(locale);
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    const int order = m_order->currentData().toInt();
    std::stable_sort(m_sources.begin(), m_sources.end(), [&](const Source &a, const Source &b) {
        switch (order) {
        case NameDescending:
            return collator.compare(a.name, b.name) > 0;
        case OldestFirst:
            return a.modified < b.modified;
        case NewestFirst:
            return a.modified > b.modified;
        default:
            return collator.compare(a.name, b.name) < 0;
        }
    });
}

void BatchRenameDialog::updatePreview()
{
    Naming::RenameOptions options;
    if (m_mode->checkedId() == 1) {
        options.mode = Naming::RenameOptions::Mode::Replace;
        options.text = m_find->text();
        options.replacement = m_replace->text();
        options.caseSensitive = m_caseSensitive->isChecked();
    } else {
        options.mode = Naming::RenameOptions::Mode::Template;
        options.text = m_template->text();
        options.start = m_start->value();
    }

    m_items.clear();
    for (int i = 0; i < m_sources.size(); ++i) {
        const Source &s = m_sources[i];
        m_items.append({s.path, Naming::renamed(s.name, s.isDir, i, options)});
    }
    QStringList problems;
    const bool ok = Naming::checkBatch(m_items, &problems);

    int changed = 0;
    m_preview->clear();
    const QColor problemColor(QStringLiteral("#e01b24"));
    for (int i = 0; i < m_items.size(); ++i) {
        const QString &oldName = m_sources[i].name;
        const QString &newName = m_items[i].newName;
        if (newName != oldName)
            ++changed;
        auto *row = new QTreeWidgetItem(m_preview, {oldName, newName});
        row->setToolTip(0, oldName);
        row->setToolTip(1, problems[i].isEmpty() ? newName : problems[i]);
        if (!problems[i].isEmpty())
            row->setForeground(1, problemColor);
        else if (newName == oldName)
            row->setForeground(1, palette().color(QPalette::Disabled, QPalette::Text));
    }

    QString summary;
    if (!ok) {
        const int bad = int(std::count_if(problems.cbegin(), problems.cend(),
                                          [](const QString &p) { return !p.isEmpty(); }));
        const auto first = std::find_if(problems.cbegin(), problems.cend(),
                                        [](const QString &p) { return !p.isEmpty(); });
        summary = (bad == 1 ? tr("1 name can't be used: %1").arg(*first)
                            : tr("%1 names can't be used. First: %2").arg(bad).arg(*first));
    } else if (changed == 0) {
        summary = tr("Nothing changes yet.");
    } else {
        summary = changed == 1 ? tr("1 item will be renamed.") : tr("%1 items will be renamed.").arg(changed);
    }
    m_summary->setText(summary);
    m_rename->setEnabled(ok && changed > 0);
}
