#pragma once

// Per-function stack frame layout: incoming args (above fp), outgoing args, spill slots (one per
// temporary live across a `call`), locals - in that order, each field aligned to its own type,
// exactly the layout CeresASM's docs/24-Calling-Convention.md recommends by hand.
//
// A function is allowed to skip the frame entirely (no proc_enter/proc_leave, just `ret`) only
// when it has <=4 parameters, no locals wider than r4-r7 and makes no call - the one explicit
// optimization allowed in v1 (§10). See the architecture plan, §10.
//
// Implemented in Fase 6 of the phased plan (§13).

namespace ceresc::codegen
{
}
