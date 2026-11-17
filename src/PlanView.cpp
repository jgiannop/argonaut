#include "PlanView.h"

#include "RichTextStyle.h"
#include "RichView.h"
#include "Theme.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {
constexpr int MoveRole = Qt::UserRole + 1; // the move's number in the plan
}

PlanView::PlanView(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    m_report = new RichView(this);
    m_report->setObjectName("qlText");
    m_report->setFocusPolicy(Qt::NoFocus);
    m_report->setOpenLinks(false);
    m_report->setOpenExternalLinks(false);
    m_report->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_report, 2);

    m_moves = new QTreeWidget(this);
    m_moves->setObjectName("qlArchive");
    m_moves->setFocusPolicy(Qt::NoFocus); // the keys stay with the files
    m_moves->setHeaderHidden(true);
    m_moves->setRootIsDecorated(false);
    m_moves->setUniformRowHeights(false); // a move is two or three lines
    m_moves->setWordWrap(true);
    m_moves->setTextElideMode(Qt::ElideNone); // every line of a move is read: it wraps
    m_moves->setSelectionMode(QAbstractItemView::SingleSelection);
    m_moves->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(m_moves, 5);
    connect(m_moves, &QTreeWidget::itemChanged, this, [this] { updateCount(); });
    connect(m_moves, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item) {
        const int number = item->data(0, MoveRole).toInt();
        if (number >= 0 && number < m_plan.moves.size())
            if (!m_plan.moves.at(number).item.isEmpty()) // (a new folder has no item to show yet)
                emit itemActivated(m_plan.folder + QLatin1Char('/') + m_plan.moves.at(number).item);
    });

    auto *bottom = new QHBoxLayout;
    m_count = new QLabel(this);
    m_count->setObjectName("qlDetails");
    m_dismiss = new QPushButton(tr("Dismiss"), this);
    m_dismiss->setObjectName("prefButton");
    m_dismiss->setFocusPolicy(Qt::NoFocus);
    m_apply = new QPushButton(tr("Apply"), this);
    m_apply->setObjectName("conflictPrimary");
    m_apply->setFocusPolicy(Qt::NoFocus);
    bottom->addWidget(m_count, 1);
    bottom->addWidget(m_dismiss);
    bottom->addWidget(m_apply);
    layout->addLayout(bottom);
    connect(m_dismiss, &QPushButton::clicked, this, &PlanView::dismissRequested);
    connect(m_apply, &QPushButton::clicked, this, &PlanView::applyRequested);
}

void PlanView::clear()
{
    m_plan = Extensions::Plan();
    m_report->clear();
    m_moves->clear();
    updateCount();
}

void PlanView::setPlan(const Extensions::Plan &plan)
{
    m_plan = plan;
    // What the extension found, as text: nothing it names is loaded, and
    // the theme decides how it looks.
    m_report->setVisible(!plan.report.trimmed().isEmpty());
    m_report->setMarkdown(plan.report);
    RichTextStyle::apply(m_report->document(), currentTheme());

    const QSignalBlocker blocker(m_moves);
    m_moves->clear();
    const Theme theme = currentTheme();
    for (int i = 0; i < plan.moves.size(); ++i) {
        const Extensions::PlanMove &move = plan.moves.at(i);
        // What it is done to, then what becomes of it (where it goes, its new name, the Trash, an archive)
        // with why, then what is wrong with it.
        const QString where = move.becomes();
        QString text = move.what() + QStringLiteral("\n    → ") + (where.isEmpty() ? tr("stays") : where);
        if (!move.reason.isEmpty())
            text += QStringLiteral(" · ") + move.reason;
        auto *row = new QTreeWidgetItem(m_moves);
        row->setData(0, MoveRole, i);
        if (move.problem.isEmpty()) {
            row->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
            row->setCheckState(0, Qt::Checked);
        } else {
            // Not something Argonaut does: said, and it can't be ticked.
            text += QStringLiteral("\n    ") + tr("Can't be done: %1").arg(move.problem);
            row->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            row->setCheckState(0, Qt::Unchecked);
            row->setForeground(0, QColor(theme.dark ? "#ff7b63" : "#c01c28"));
        }
        row->setText(0, text);
        row->setToolTip(0, text);
    }
    updateCount();
}

QList<Extensions::PlanMove> PlanView::keptMoves() const
{
    QList<Extensions::PlanMove> kept;
    for (int i = 0; i < m_moves->topLevelItemCount(); ++i) {
        const QTreeWidgetItem *row = m_moves->topLevelItem(i);
        const int number = row->data(0, MoveRole).toInt();
        if (row->checkState(0) == Qt::Checked && (row->flags() & Qt::ItemIsUserCheckable) && number < m_plan.moves.size()
            && m_plan.moves.at(number).problem.isEmpty())
            kept.append(m_plan.moves.at(number));
    }
    return kept;
}

int PlanView::moveCount() const
{
    return m_moves->topLevelItemCount();
}

QString PlanView::moveText(int row) const
{
    return row >= 0 && row < moveCount() ? m_moves->topLevelItem(row)->text(0) : QString();
}

bool PlanView::isMoveTicked(int row) const
{
    return row >= 0 && row < moveCount() && m_moves->topLevelItem(row)->checkState(0) == Qt::Checked;
}

bool PlanView::canMoveBeTicked(int row) const
{
    return row >= 0 && row < moveCount() && (m_moves->topLevelItem(row)->flags() & Qt::ItemIsUserCheckable);
}

void PlanView::setMoveTicked(int row, bool on)
{
    if (canMoveBeTicked(row))
        m_moves->topLevelItem(row)->setCheckState(0, on ? Qt::Checked : Qt::Unchecked);
}

void PlanView::updateCount()
{
    const int kept = int(keptMoves().size());
    const int all = int(m_plan.moves.size());
    m_count->setText(all == 0 ? tr("Nothing to move") : tr("%1 of %2 moves").arg(kept).arg(all));
    m_apply->setEnabled(kept > 0);
}
