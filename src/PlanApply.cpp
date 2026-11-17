#include "PlanApply.h"

#include "FileJob.h"
#include "FileOperations.h"

#include <QDir>
#include <QFileInfo>

namespace PlanApply {

Outcome run(FileOperations *ops, const Extensions::Plan &plan, const QList<Extensions::PlanMove> &kept)
{
    Outcome outcome;
    QList<Extensions::PlanMove> moves = kept;
    Extensions::Host::checkPlan(plan.folder, &moves);

    QList<TransferResult> created; // the top-most folders made for the plan
    QList<Transfer> transfers;
    for (const Extensions::PlanMove &move : std::as_const(moves)) {
        if (!move.problem.isEmpty()) {
            ++outcome.skipped;
            continue;
        }
        QString dir = plan.folder;
        bool made = true;
        for (const QString &part : move.folder.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
            dir += QLatin1Char('/') + part;
            if (QFileInfo::exists(dir))
                continue;
            if (!QDir().mkdir(dir)) {
                outcome.error = QStringLiteral("Couldn't create the folder “%1”").arg(part);
                made = false;
                break;
            }
            bool inside = false;
            for (const TransferResult &c : std::as_const(created))
                inside = inside || dir.startsWith(c.target + QLatin1Char('/'));
            if (!inside)
                created.append({QString(), dir});
        }
        if (!made)
            break;
        transfers.append({plan.folder + QLatin1Char('/') + move.item, dir, move.newName.isEmpty() ? move.item : move.newName});
    }
    // The folders that were made stay undoable even when one couldn't be.
    if (!created.isEmpty())
        ops->recordUndo(FileJob::Kind::Copy, created, QStringLiteral("New Folders"));
    if (!outcome.error.isEmpty()) {
        Extensions::Host::instance()->answerPlan(plan.extension, plan.id, false, 0, int(moves.size()));
        return outcome;
    }
    outcome.moving = int(transfers.size());
    if (!transfers.isEmpty())
        ops->moveItems(transfers, QStringLiteral("Organize"));
    Extensions::Host::instance()->answerPlan(plan.extension, plan.id, false, outcome.moving, outcome.skipped);
    return outcome;
}

QString message(const Outcome &outcome)
{
    if (!outcome.error.isEmpty())
        return outcome.error;
    const QString changed = QStringLiteral("the folder has changed since the plan was made");
    if (outcome.moving == 0)
        return outcome.skipped > 0 ? QStringLiteral("Nothing was moved: ") + changed : QStringLiteral("Nothing to move");
    QString text = outcome.moving == 1 ? QStringLiteral("Organizing 1 item…") : QStringLiteral("Organizing %1 items…").arg(outcome.moving);
    if (outcome.skipped > 0)
        text += QStringLiteral(" %1 left where they are: ").arg(outcome.skipped) + changed;
    return text;
}

} // namespace PlanApply
