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
    // Makes `relative` under the plan's folder, with what is missing above it.
    auto makeFolder = [&](const QString &relative, QString *dir) {
        *dir = plan.folder;
        for (const QString &part : relative.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
            *dir += QLatin1Char('/') + part;
            if (QFileInfo::exists(*dir))
                continue;
            if (!QDir().mkdir(*dir)) {
                outcome.error = QStringLiteral("Couldn't create the folder “%1”").arg(part);
                return false;
            }
            bool inside = false;
            for (const TransferResult &c : std::as_const(created))
                inside = inside || dir->startsWith(c.target + QLatin1Char('/'));
            if (!inside)
                created.append({QString(), *dir});
        }
        return true;
    };
    QList<Transfer> toMove, toCopy;
    QStringList toTrash;
    struct Archive
    {
        QString dir, name;
        QStringList paths;
    };
    QList<Archive> archives;
    int folders = 0;
    for (const Extensions::PlanMove &move : std::as_const(moves)) {
        if (!move.problem.isEmpty()) {
            ++outcome.skipped;
            continue;
        }
        const QString path = plan.folder + QLatin1Char('/') + move.item;
        if (move.action == QLatin1String("trash")) {
            toTrash << path; // the Trash: it can be put back
            continue;
        }
        QString dir;
        if (!makeFolder(move.folder, &dir))
            break;
        if (move.action == QLatin1String("folder")) {
            ++folders;
        } else if (move.action == QLatin1String("compress")) {
            bool known = false;
            for (Archive &archive : archives) {
                if (archive.dir == dir && archive.name == move.newName) {
                    archive.paths << path;
                    known = true;
                }
            }
            if (!known)
                archives.append({dir, move.newName, {path}});
        } else if (move.action == QLatin1String("copy")) {
            toCopy.append({path, dir, move.newName.isEmpty() ? move.item : move.newName});
        } else if (move.isMove()) {
            toMove.append({path, dir, move.newName.isEmpty() ? move.item : move.newName});
        } else {
            ++outcome.skipped; // not a step there is: nothing is done for it
        }
    }
    // The folders that were made stay undoable even when one couldn't be.
    if (!created.isEmpty())
        ops->recordUndo(FileJob::Kind::Copy, created, QStringLiteral("New Folders"));
    if (!outcome.error.isEmpty()) {
        Extensions::Host::instance()->answerPlan(plan.extension, plan.id, false, 0, int(moves.size()));
        return outcome;
    }
    // Each kind is one job of the file engine: progress, no overwriting, and its own Undo.
    const bool onlyMoves = toCopy.isEmpty() && toTrash.isEmpty() && archives.isEmpty() && folders == 0;
    outcome.moving = folders;
    if (!toCopy.isEmpty()) {
        ops->copyItems(toCopy, QStringLiteral("Copy"));
        outcome.moving += int(toCopy.size());
    }
    for (const Archive &archive : std::as_const(archives)) {
        // The format is what the name ends in (the engine gets the name whole).
        FileJob::ArchiveFormat format = FileJob::ArchiveFormat::Zip;
        for (FileJob::ArchiveFormat each : {FileJob::ArchiveFormat::TarGz, FileJob::ArchiveFormat::TarXz, FileJob::ArchiveFormat::SevenZip,
                                            FileJob::ArchiveFormat::Zip}) {
            if (archive.name.endsWith(FileJob::archiveSuffix(each), Qt::CaseInsensitive)) {
                format = each;
                break;
            }
        }
        ops->compress(archive.paths, archive.dir, archive.name, format);
        outcome.moving += int(archive.paths.size());
    }
    if (!toMove.isEmpty()) {
        ops->moveItems(toMove, onlyMoves ? QStringLiteral("Organize") : QStringLiteral("Move"));
        outcome.moving += int(toMove.size());
    }
    if (!toTrash.isEmpty()) {
        ops->trash(toTrash);
        outcome.moving += int(toTrash.size());
    }
    outcome.mixed = !onlyMoves;
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
    QString text = outcome.mixed ? (outcome.moving == 1 ? QStringLiteral("Doing 1 step… Ctrl+Z undoes it") : QStringLiteral("Doing %1 steps… Ctrl+Z undoes them, one kind at a time").arg(outcome.moving))
                   : outcome.moving == 1 ? QStringLiteral("Organizing 1 item…") : QStringLiteral("Organizing %1 items…").arg(outcome.moving);
    if (outcome.skipped > 0)
        text += QStringLiteral(" %1 left where they are: ").arg(outcome.skipped) + changed;
    return text;
}

} // namespace PlanApply
