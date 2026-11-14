#include "OrganizerDialog.h"

#include "Theme.h"

#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

QLabel *heading(const QString &text, QWidget *parent)
{
    auto *l = new QLabel(text, parent);
    l->setObjectName("prefHeading");
    l->setWordWrap(true);
    return l;
}

QLabel *hint(const QString &text, QWidget *parent)
{
    auto *l = new QLabel(text, parent);
    l->setObjectName("prefHint");
    l->setWordWrap(true);
    return l;
}

QPushButton *button(const QString &text, QWidget *parent, bool primary = false)
{
    auto *b = new QPushButton(text, parent);
    b->setObjectName(primary ? "aboutClose" : "prefButton");
    b->setCursor(Qt::PointingHandCursor);
    return b;
}

QString money(double dollars)
{
    return dollars < 0.01 ? QObject::tr("less than one cent") : QStringLiteral("$%1").arg(dollars, 0, 'f', 2);
}

} // namespace

OrganizerDialog::OrganizerDialog(const QString &folder, QWidget *parent)
    : QDialog(parent), m_client(new Claude::Client(this)), m_organizer(new Organizer(folder, m_client, this))
{
    setObjectName("connectDialog");
    const QString name = QFileInfo(folder).fileName().isEmpty() ? QDir::toNativeSeparators(folder) : QFileInfo(folder).fileName();
    setWindowTitle(tr("Organize “%1”").arg(name));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 20, 24, 20);
    m_pages = new QStackedWidget(this);
    layout->addWidget(m_pages);

    // ---- 1. Start
    auto *start = new QWidget(m_pages);
    auto *s = new QVBoxLayout(start);
    s->setContentsMargins(0, 0, 0, 0);
    s->setSpacing(8);
    m_organizer->scan();
    const int items = int(m_organizer->items().size());
    s->addWidget(heading(tr("Organize “%1” with Claude").arg(name), start));
    QString what = items == 1 ? tr("1 item") : tr("%1 items").arg(items);
    if (m_organizer->skipped() > 0)
        what += tr(" (the %1 most recent; %2 older ones are left out)").arg(Organizer::MaxItems).arg(m_organizer->skipped());
    s->addWidget(hint(tr("Claude will look at %1 here and propose folders and better names, with a short report. "
                         "Nothing changes until you review the plan and apply it, and Ctrl+Z undoes it.")
                          .arg(what),
                      start));
    s->addSpacing(6);
    auto *wishesLabel = new QLabel(tr("Your wishes (optional)"), start);
    wishesLabel->setObjectName("propHeading");
    s->addWidget(wishesLabel);
    m_wishes = new QLineEdit(start);
    m_wishes->setObjectName("connectAddress");
    m_wishes->setPlaceholderText(tr("e.g. invoices by year, keep the photos together"));
    s->addWidget(m_wishes);
    auto *modeLabel = new QLabel(tr("What Claude may see"), start);
    modeLabel->setObjectName("propHeading");
    s->addWidget(modeLabel);
    m_mode = new QComboBox(start);
    m_mode->setObjectName("propCombo");
    for (Claude::Mode m : {Claude::Mode::NamesOnly, Claude::Mode::Ask, Claude::Mode::Auto})
        m_mode->addItem(Claude::modeName(m), int(m));
    m_mode->setCurrentIndex(m_mode->findData(int(Claude::mode())));
    s->addWidget(m_mode);
    s->addWidget(hint(tr("Sent to Anthropic: the names, sizes, dates and types of these items, and (if you allow it) "
                         "the beginning of some files. Programs are never read."),
                      start));
    s->addSpacing(6);
    m_estimate = hint(QString(), start);
    s->addWidget(m_estimate);
    s->addStretch();
    auto *startButtons = new QHBoxLayout;
    auto *prefs = button(tr("Preferences…"), start);
    connect(prefs, &QPushButton::clicked, this, &OrganizerDialog::openPreferences);
    startButtons->addWidget(prefs);
    startButtons->addStretch();
    auto *cancel = button(tr("Cancel"), start);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    startButtons->addWidget(cancel);
    m_start = button(tr("Start"), start, true);
    m_start->setDefault(true);
    startButtons->addWidget(m_start);
    s->addLayout(startButtons);
    m_pages->addWidget(start);

    // ---- 2. Working
    auto *working = new QWidget(m_pages);
    auto *w = new QVBoxLayout(working);
    w->setContentsMargins(0, 0, 0, 0);
    w->addWidget(heading(tr("Organizing “%1”").arg(name), working));
    m_progress = hint(QString(), working);
    w->addWidget(m_progress);
    m_askPanel = new QWidget(working);
    m_askPanel->setObjectName("aboutCard");
    auto *a = new QVBoxLayout(m_askPanel);
    a->setContentsMargins(14, 12, 14, 12);
    m_askText = new QLabel(m_askPanel);
    m_askText->setObjectName("aboutText");
    m_askText->setWordWrap(true);
    m_askText->setTextFormat(Qt::RichText);
    a->addWidget(m_askText);
    auto *askButtons = new QHBoxLayout;
    askButtons->addStretch();
    auto *decline = button(tr("Don't share"), m_askPanel);
    auto *allow = button(tr("Allow"), m_askPanel, true);
    askButtons->addWidget(decline);
    askButtons->addWidget(allow);
    a->addLayout(askButtons);
    m_askPanel->hide();
    w->addWidget(m_askPanel);
    w->addStretch();
    auto *stopRow = new QHBoxLayout;
    stopRow->addStretch();
    auto *stop = button(tr("Cancel"), working);
    stopRow->addWidget(stop);
    w->addLayout(stopRow);
    m_pages->addWidget(working);
    connect(stop, &QPushButton::clicked, this, [this] {
        m_organizer->cancel();
        reject();
    });
    connect(allow, &QPushButton::clicked, this, [this] {
        m_askPanel->hide();
        m_organizer->allowReading(true);
    });
    connect(decline, &QPushButton::clicked, this, [this] {
        m_askPanel->hide();
        m_organizer->allowReading(false);
    });

    // ---- 3. Result
    auto *result = new QWidget(m_pages);
    auto *r = new QVBoxLayout(result);
    r->setContentsMargins(0, 0, 0, 0);
    auto *split = new QSplitter(Qt::Vertical, result);
    m_report = new QTextBrowser(split);
    m_report->setObjectName("helpPage");
    m_report->setOpenExternalLinks(false);
    m_plan = new QTreeWidget(split);
    m_plan->setObjectName("qlArchive");
    m_plan->setHeaderLabels({tr("Item"), tr("Goes to"), tr("Why")});
    m_plan->setRootIsDecorated(false);
    m_plan->header()->setSectionResizeMode(QHeaderView::Interactive);
    m_plan->header()->setStretchLastSection(true);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 1);
    r->addWidget(split, 1);
    auto *resultButtons = new QHBoxLayout;
    m_cost = hint(QString(), result);
    resultButtons->addWidget(m_cost, 1);
    auto *close = button(tr("Close"), result);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    resultButtons->addWidget(close);
    m_apply = button(QString(), result, true);
    connect(m_apply, &QPushButton::clicked, this, &QDialog::accept);
    resultButtons->addWidget(m_apply);
    r->addLayout(resultButtons);
    m_pages->addWidget(result);
    connect(m_plan, &QTreeWidget::itemChanged, this, &OrganizerDialog::updateApply);

    // ---- Organizer signals
    connect(m_organizer, &Organizer::progress, m_progress, &QLabel::setText);
    connect(m_organizer, &Organizer::readRequest, this, [this](const QStringList &files, const QString &reason) {
        QStringList shown;
        for (const QString &f : files.mid(0, 12))
            shown << QStringLiteral("<b>%1</b>").arg(f.toHtmlEscaped());
        if (files.size() > 12)
            shown << tr("and %1 more").arg(files.size() - 12);
        m_askText->setText(tr("Claude would like to read the beginning of %1:<br>%2<br><br><i>%3</i>")
                               .arg(files.size() == 1 ? tr("1 file") : tr("%1 files").arg(files.size()),
                                    shown.join(QStringLiteral(", ")), reason.toHtmlEscaped()));
        m_progress->setText(tr("Waiting for your answer…"));
        m_askPanel->show();
    });
    connect(m_organizer, &Organizer::failed, this, [this](const QString &error) {
        m_progress->setText(tr("It didn't work: %1").arg(error));
    });
    connect(m_organizer, &Organizer::finished, this, &OrganizerDialog::showResult);

    connect(m_start, &QPushButton::clicked, this, [this] {
        m_organizer->setWishes(m_wishes->text());
        m_organizer->setMode(Claude::Mode(m_mode->currentData().toInt()));
        m_pages->setCurrentIndex(1);
        m_organizer->start();
    });
    connect(m_mode, &QComboBox::currentIndexChanged, this, [this] {
        m_organizer->setMode(Claude::Mode(m_mode->currentData().toInt()));
        showStart(); // the tools change, so does the estimate
    });
    connect(this, &OrganizerDialog::openPreferences, this, [this] { QMetaObject::invokeMethod(this, &OrganizerDialog::showStart, Qt::QueuedConnection); });

    showStart();
    resize(720, 560);
}

void OrganizerDialog::showStart()
{
    m_pages->setCurrentIndex(0);
    if (m_organizer->items().isEmpty()) {
        m_estimate->setText(tr("This folder is empty."));
        m_start->setEnabled(false);
        return;
    }
    const bool subscription = m_client->connection() == Claude::Connection::Subscription;
    if (!m_client->ready()) {
        m_estimate->setText(subscription ? tr("Install Claude Code and sign in with your Claude account first "
                                              "(see Preferences → Claude).")
                                         : tr("Add your Anthropic API key in Preferences → Claude first."));
        m_start->setEnabled(false);
        return;
    }
    if (subscription) {
        // No price to show: it counts toward the plan's usage limits.
        m_start->setEnabled(true);
        m_estimate->setText(tr("Uses your Claude subscription through Claude Code, with %1. "
                               "It counts toward your plan's usage limits; there is no extra charge.")
                                .arg(Claude::modelInfo(Claude::model()).name));
        return;
    }
    m_start->setEnabled(true);
    m_estimate->setText(tr("Estimating the cost…"));
    m_organizer->estimate([this](qint64 tokens, double dollars, const QString &error) {
        if (!error.isEmpty()) {
            m_estimate->setText(tr("Couldn't estimate the cost: %1").arg(error));
            return;
        }
        m_estimate->setText(tr("About %1 tokens to send: roughly %2 with %3 (more if Claude reads some files).")
                                .arg(QLocale().toString(tokens), money(dollars), Claude::modelInfo(Claude::model()).name));
    });
}

void OrganizerDialog::showResult(const QString &report, const QList<Organizer::Move> &moves)
{
    m_moves = moves;
    const Theme t = currentTheme();
    m_report->document()->setDefaultStyleSheet(QStringLiteral("body { color: %1; } h1, h2, h3 { margin-top: 10px; }").arg(t.fg.name()));
    m_report->setMarkdown(report.isEmpty() ? tr("Claude didn't write a report.") : report);

    const QSignalBlocker blocker(m_plan);
    m_plan->clear();
    for (int i = 0; i < moves.size(); ++i) {
        const Organizer::Move &m = moves[i];
        QString to = m.folder.isEmpty() ? tr("(stays here)") : m.folder + QLatin1Char('/');
        if (!m.newName.isEmpty())
            to += m.newName;
        auto *row = new QTreeWidgetItem(m_plan, {m.item, to, m.problem.isEmpty() ? m.reason : m.problem});
        row->setData(0, Qt::UserRole, i);
        row->setToolTip(0, m.item);
        row->setToolTip(1, to);
        row->setToolTip(2, m.problem.isEmpty() ? m.reason : m.problem);
        if (m.problem.isEmpty()) {
            row->setFlags(row->flags() | Qt::ItemIsUserCheckable);
            row->setCheckState(0, Qt::Checked);
        } else {
            row->setFlags(row->flags() & ~Qt::ItemIsEnabled);
            row->setForeground(2, QColor(QStringLiteral("#e01b24")));
        }
    }
    m_plan->resizeColumnToContents(0);
    m_plan->setColumnWidth(0, qMin(m_plan->columnWidth(0), 260));
    m_plan->resizeColumnToContents(1);
    m_plan->setColumnWidth(1, qMin(m_plan->columnWidth(1), 260));
    m_cost->setText(m_client->connection() == Claude::Connection::Subscription
                        ? tr("Done with your Claude subscription.")
                        : tr("This cost %1.").arg(money(m_organizer->dollars())));
    updateApply();
    m_pages->setCurrentIndex(2);
}

void OrganizerDialog::updateApply()
{
    const int count = int(keptMoves().size());
    m_apply->setText(count == 0 ? tr("Nothing to Apply") : count == 1 ? tr("Apply 1 Change") : tr("Apply %1 Changes").arg(count));
    m_apply->setEnabled(count > 0);
}

QList<Organizer::Move> OrganizerDialog::keptMoves() const
{
    QList<Organizer::Move> kept;
    for (int i = 0; i < m_plan->topLevelItemCount(); ++i) {
        const QTreeWidgetItem *row = m_plan->topLevelItem(i);
        const int index = row->data(0, Qt::UserRole).toInt();
        if ((row->flags() & Qt::ItemIsUserCheckable) && row->checkState(0) == Qt::Checked && index < m_moves.size())
            kept << m_moves[index];
    }
    return kept;
}
