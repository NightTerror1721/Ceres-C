#pragma once

#include <ceresc/ir/ir_function.h>
#include <ceresc/support/arena.h>
#include <ceresc/support/optimization.h>

// optimize() - the IR-level optimization pipeline (§13's Fase 9, now on by default - see
// support/optimization.h's own header comment on why every pass here stays switchable off).
//
// Runs entirely between IrBuilder::build() and libs/codegen, over the IR alone: nothing here needs
// the AST, sema, or any knowledge of the target machine. Every pass rewrites blocks in place
// through BasicBlock::replaceInstrs()/IrFunction::retainBlocks(), allocating any replacement
// instruction in the same Arena the IR already lives in.
//
// The passes, in the order the pipeline applies them:
//
//   inlining              splices a small, single-block, call-free callee into its caller, so every
//                         pass below then sees the callee's body as ordinary caller code
//   constant folding      an operation whose operands are all known constants becomes a Const
//   algebraic             x+0 / x-0 / x*1 / x*0 / x/1 / x<<0 / x>>0 collapse to a copy or a zero
//   copy propagation      replaces a copied temporary with its original value where safe
//   load forwarding       reuses a known same-width store instead of loading the local again
//   dead stores           drops a store to a non-escaping local overwritten before any read
//   branch simplification a CondJump whose two operands are known constants becomes a plain Jump
//   jump threading        a branch whose target only jumps somewhere else goes straight there
//   unreachable blocks    a block no path from the entry reaches is dropped
//   dead code             a pure instruction whose result nothing reads is dropped
//
// optimize() runs the exact sequence. Everything after inlining runs to its kMaxRounds fixpoint;
// unused-function elimination then removes bodies no remaining call can reach. Folding creates
// dead constants, dead-code removal exposes empty blocks, threading exposes unreachable ones, and
// so on round the loop.
//
// Two deliberate non-optimizations, both for the same reason - the machine this targets is not an
// abstract one:
//
//   - A Load is NEVER treated as dead, even with an unread result. Device registers live at
//     ordinary addresses in this machine (07-IO-Devices-and-Ports.md) and reading one can have a
//     real side effect; `volatile`, the thing that would let a program say so, is explicitly out of
//     v1 (§3). Dropping an unread load is what a C compiler would do and what this one must not,
//     until there is a `volatile` to honour.
//   - A division or modulo by a constant zero is never folded. The VM does not fault on it: it
//     sets the Trap flag and leaves the destination register untouched (05-Instruction-Set.md,
//     §14's own note). Folding it to any value at all would invent a result the hardware never
//     produces.

namespace ceresc::ir
{
	// Applies every pass `options` enables to `module`, in place. A no-op when `options` has them
	// all off (support::OptimizationOptions::none(), i.e. -O0), which is exactly what makes the
	// unoptimized IR still reachable for comparison.
	void optimize(IrModule& module, support::Arena& arena, const support::OptimizationOptions& options);
}
