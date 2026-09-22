#pragma once

#include "types.h"
#include <span>
#include <string_view>

// OptimizationOptions - which optimizations the IR pass pipeline (libs/ir's ir_optimizer.h) and
// the code generator (libs/codegen) are allowed to apply, plus the -O level that presets them.
//
// Every optimization here is ON by default (see forLevel()'s O1, which is what the driver uses
// when no -O flag is given). Each one can still be switched off individually, and -O0 switches all
// of them off at once, because every one of them has a SIMPLER counterpart that this compiler
// deliberately keeps working and testable:
//
//   - frameless leaf functions   <-> one uniform frame shape for every function
//   - register windows/slot reuse <-> one permanent frame field per local/parameter/temporary
//   - cmp+branch fusion          <-> a materialized 0/1 value, then a branch against zero
//   - folded addressing modes    <-> one `add` per element address, then a load through it
//   - constant folding/DCE/...   <-> the IR exactly as IrBuilder emitted it
//
// Those simplified forms are not dead weight: they are what makes a miscompilation bisectable
// ("does it still happen at -O0?"), what the golden tests pin on both sides so the optimized and
// simplified paths can be compared line by line, and what keeps every stage explainable on its own
// - §0's "comprensible antes que completo". §13's Fase 9 lists precisely these as the optimizations
// to add "once the simpler version has real programs running against it"; keeping the simple
// version selectable is how that promise stays honest instead of being overwritten.
//
// Living in libs/support rather than libs/codegen because three different libraries read it:
// libs/ir (IrBuilder's local-slot reuse and the ir_optimizer passes), libs/codegen (placement,
// peepholes) and libs/driver (the -O/-f command line). support is the one library all three
// already depend on (§1's dependency diagram).

namespace ceresc::support
{
	enum class OptimizationLevel : u8
	{
		O0, // every optimization off - the simplified, uniform, easiest-to-explain output
		O1, // everything except inlining (the default when no -O flag is given)
		O2  // everything, inlining included
	};

	struct OptimizationOptions
	{
		// ---- IR-level (libs/ir, ir_optimizer.h) -------------------------------------------------
		bool constantFolding = true;			// `2 + 3` -> `5`, including through a Cmp
		bool algebraicSimplification = true;	// `x + 0`, `x * 1`, `x * 0`, `x - 0`, `x / 1`
		bool branchSimplification = true;		// a CondJump whose operands are both known constants
		bool deadCodeElimination = true;		// a pure instruction whose result nothing reads
		bool unreachableBlockElimination = true;// a block no path from the entry can reach
		bool jumpThreading = true;				// a branch to a block that only jumps somewhere else
		bool inlining = false;					// splice a small leaf function into its caller (O2)
		bool localSlotReuse = true;				// two locals in disjoint lexical scopes share a frame slot
		bool copyPropagation = true;			// read a copy's source directly instead of the copy
		bool loadForwarding = true;				// reuse a just-stored value instead of loading it back
		bool deadStoreElimination = true;		// drop stores to a local nothing ever reads
		bool unusedFunctionElimination = true;	// drop functions no reachable call can arrive at
		bool jumpTables = true;					// a dense switch becomes a jump table, a sparse one a binary search
		bool strengthReduction = true;			// *2^k -> shl, /2^k -> shr/sar, %2^k -> and/bias

		// ---- codegen-level (libs/codegen) -------------------------------------------------------
		bool framelessLeaf = true;				// no enter/leave at all when nothing needs the frame
		bool registerAllocation = true;			// values live in registers, spilled slots get reused
		bool cmpBranchFusion = true;			// Cmp + CondJump-against-zero -> one ifXX
		bool immediateOperands = true;			// fold a constant operand into addi/ifXX/... directly
		bool fallthroughBranches = true;		// drop a jump whose target is the next block emitted
		bool addressFolding = true;				// fold an address computation into the load/store that reads it

		// Defined below optimizationFlags(), which it walks to build O0 - see its own note.
		static OptimizationOptions forLevel(OptimizationLevel level) noexcept;

		// Every optimization off - the simplified behaviour described in this header's own comment.
		static OptimizationOptions none() noexcept;
	};

	// One `-f<name>` / `-fno-<name>` switch: the spelling on the command line and the field it
	// toggles. A table rather than a chain of string comparisons in the option parser, so the
	// driver's `--help` text and its parsing can never disagree about which names exist.
	struct OptimizationFlag
	{
		std::string_view name;
		bool OptimizationOptions::* field;
		std::string_view description;
	};

	inline std::span<const OptimizationFlag> optimizationFlags() noexcept
	{
		static constexpr OptimizationFlag kFlags[] = {
			{ "const-fold",         &OptimizationOptions::constantFolding,          "fold operations on compile-time constants" },
			{ "algebraic",          &OptimizationOptions::algebraicSimplification,  "simplify x+0, x*1, x*0, x-0, x/1" },
			{ "branch-simplify",    &OptimizationOptions::branchSimplification,     "resolve a branch whose condition is constant" },
			{ "dce",                &OptimizationOptions::deadCodeElimination,      "drop instructions whose result nothing reads" },
			{ "unreachable-blocks", &OptimizationOptions::unreachableBlockElimination, "drop blocks no path can reach" },
			{ "jump-threading",     &OptimizationOptions::jumpThreading,            "retarget branches through jump-only blocks" },
			{ "inline",             &OptimizationOptions::inlining,                 "splice small leaf functions into their callers" },
			{ "local-slot-reuse",   &OptimizationOptions::localSlotReuse,           "share a frame slot between locals in disjoint scopes" },
			{ "copy-propagation",   &OptimizationOptions::copyPropagation,          "read a copy's source directly instead of the copy" },
			{ "load-forwarding",    &OptimizationOptions::loadForwarding,           "reuse a just-stored value instead of loading it back" },
			{ "dead-stores",        &OptimizationOptions::deadStoreElimination,     "drop stores to a local nothing ever reads" },
			{ "unused-functions",   &OptimizationOptions::unusedFunctionElimination,"drop functions no reachable call can arrive at" },
			{ "jump-tables",        &OptimizationOptions::jumpTables,               "lower a dense switch to a jump table, a sparse one to a binary search" },
			{ "strength-reduction", &OptimizationOptions::strengthReduction,        "turn *2^k, /2^k and %2^k into shifts and masks" },
			{ "frameless-leaf",     &OptimizationOptions::framelessLeaf,            "omit the stack frame when a function needs none" },
			{ "regalloc",           &OptimizationOptions::registerAllocation,       "keep values in registers; reuse spilled frame slots" },
			{ "cmp-branch-fusion",  &OptimizationOptions::cmpBranchFusion,          "fuse a comparison into the branch that reads it" },
			{ "immediates",         &OptimizationOptions::immediateOperands,        "use a constant operand directly as an immediate" },
			{ "fallthrough",        &OptimizationOptions::fallthroughBranches,      "drop a jump to the block emitted right after it" },
			{ "address-folding",    &OptimizationOptions::addressFolding,           "use [base + index] / [base + N] instead of a separate add" },
		};
		return kFlags;
	}

	inline OptimizationOptions OptimizationOptions::forLevel(OptimizationLevel level) noexcept
	{
		OptimizationOptions options; // the member initializers above already are O1
		switch (level)
		{
			case OptimizationLevel::O0:
				// Walked from the table rather than written out field by field, so that "-O0 means
				// every optimization off" is true by construction: a newly added optimization cannot
				// be forgotten here and quietly stay on at -O0, which would silently cost the
				// simplified path its whole purpose as the thing you bisect against.
				for (const OptimizationFlag& flag : optimizationFlags())
					options.*(flag.field) = false;
				break;
			case OptimizationLevel::O1:
				break;
			case OptimizationLevel::O2:
				options.inlining = true;
				break;
		}
		return options;
	}

	inline OptimizationOptions OptimizationOptions::none() noexcept { return forLevel(OptimizationLevel::O0); }
}
