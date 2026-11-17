#pragma once

#include "Extensions.h"

class FileOperations;

// Carrying out the changes an extension proposed (a plan) that the user
// kept: the one way for both interfaces, so what the window does and what
// the terminal does can't drift apart.
namespace PlanApply {

struct Outcome
{
    int moving = 0;  // steps that were started (items moved, copied, compressed, trashed; folders made)
    bool mixed = false; // not only moves and renames: several jobs, each with its own Undo
    int skipped = 0; // kept by the user, but no longer possible
    QString error;   // a folder couldn't be made: nothing was moved
};

// Checks the kept moves again against the folder as it is now (it may have
// changed while the plan was looked at), makes the folders (undoable on
// their own, "New Folders"), starts one move job with the renames
// ("Organize": Ctrl+Z puts everything back), and tells the extension how
// many were applied and skipped.
Outcome run(FileOperations *ops, const Extensions::Plan &plan, const QList<Extensions::PlanMove> &kept);

// What to tell the user about it.
QString message(const Outcome &outcome);

} // namespace PlanApply
