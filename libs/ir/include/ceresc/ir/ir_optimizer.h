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
//   strength reduction    x*2^k / x/2^k / x%2^k become a shift, a mask, or a small bias sequence
//   algebraic             x+0 / x-0 / x*1 / x*0 / x/1 / x<<0 / x>>0 collapse to a copy or a zero
//   copy propagation      replaces a copied temporary with its original value where safe
//   CSE                   reuses a pure expression already computed earlier in the same block
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
//   - A VOLATILE Load is never treated as dead, even with an unread result. Device registers live
//     at ordinary addresses in this machine (07-IO-Devices-and-Ports.md) and reading one can have a
//     real side effect, so a load the program marked observable stays. Every load used to stay, for
//     want of anything that could tell the two apart; `volatile` is what tells them apart, and it
//     is recorded on the access itself (IrLoadPayload::isVolatile) rather than only on a local, so
//     an indirect access carries it too. An unmarked load reads ordinary memory and goes.
//   - A division or modulo by a constant zero is never folded. The VM does not fault on it: it
//     sets the Trap flag and leaves the destination register untouched (05-Instruction-Set.md,
//     §14's own note). Folding it to any value at all would invent a result the hardware never
//     produces.

namespace ceresc::ir
{
	// What the pipeline did, filled in only when the caller asks (the driver's --stats): how many
	// instructions the module held before and after, how many calls were spliced in, and how many
	// jump tables the lowering left behind. Deliberately coarse - a per-pass breakdown would have to
	// be threaded through every pass for a number nobody acts on.
	struct OptimizationStats
	{
		u32 functions = 0;          // live functions after optimization
		u32 instructionsBefore = 0; // IrInstr across every function, before any pass
		u32 instructionsAfter = 0;  // ... and after
		u32 inlinedCalls = 0;       // call sites the inliner spliced away
		u32 jumpTables = 0;         // IrOpcode::TableJump instructions left in the module
	};

	// Applies every pass `options` enables to `module`, in place. A no-op when `options` has them
	// all off (support::OptimizationOptions::none(), i.e. -O0), which is exactly what makes the
	// unoptimized IR still reachable for comparison. `stats`, when given, receives the counts above.
	void optimize(IrModule& module, support::Arena& arena, const support::OptimizationOptions& options,
		OptimizationStats* stats = nullptr);
}
