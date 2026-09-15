#pragma once

#include <ceresc/ir/ir_function.h>
#include <vector>

// FrameLayout - decides how many outgoing-argument stack words one IrFunction needs, and
// assignArgSlots() - shared by that computation and by CodeGen's own parameter-prologue/call-site
// lowering - decides where each argument in a list goes. See 24-Calling-Convention.md and the
// architecture plan, §10.
//
// This does NOT compute byte offsets for locals/temporaries: CodeGen emits a real CASM `struct`
// per function (23-Structs.md) and lets the real assembler compute every field's offset from it,
// the same way a hand-written program following the calling convention would - see codegen.cpp.
// That sidesteps a whole class of bug this class would otherwise own: this project's own layout
// arithmetic silently drifting out of sync with the actual assembler's (verified once against
// CeresASM/Ceres/libs/asm/src/translation_unit.cpp's own struct-layout code, but a fact about
// another project's implementation, not a contract either side promises to keep). Symbolic
// `[sp + Frame.field]` references stay correct by construction instead.
//
// Every function gets a real frame in this version - the "no frame at all" leaf optimization §10
// mentions is deliberately not implemented: proving it safe needs knowing that no local's or
// parameter's address is ever taken (`&x` on a value that would otherwise just live in a
// register), which needs a real escape analysis over the IR. Getting that wrong would generate a
// function that reads/writes through a frame slot that was never actually reserved - silent memory
// corruption, not a compile error - for the sake of an optimization the plan itself calls optional
// (§13's Fase 9 already lists moving unaddressed locals into r8-r11 as a later, clearly-scoped
// candidate). One frame shape, always correct, beats two shapes where one has a sharp edge - see
// §0's "comprensible antes que completo".
//
// A second simplification goes further than §10's own text: EVERY temporary (not just one still
// live across a `call`) gets its own permanent frame field, not only the locals/parameters a real
// VarDecl/Param introduces. §10 describes keeping a temporary in r4-r7/r12 while evaluating a
// single expression, spilling it only around a `call`; that needs a liveness analysis (which
// temporaries are still needed after which instruction) to know when a spill is actually required.
// Giving every temporary a durable memory home instead needs no liveness analysis at all - a
// `call` can never lose a value nothing was ever resident in a register to begin with - at the
// cost of more load/store traffic than the minimum. Reclaiming the tighter, register-window
// version of §10's rule is a natural Fase 9 candidate once the simpler version has real programs
// running against it.
//
// Implemented in Fase 6 of the phased plan (§13).

namespace ceresc::codegen
{
	// Where one argument (a Call's Param, or a function's own parameter) lands: the first four
	// of its own bank (int or float, counted separately - "a function taking (u32, f32, u32)
	// receives them in r0, f0, r1", 24-Calling-Convention.md) go in that bank's registers; every
	// argument beyond its own bank's fourth spot goes to the next sequential outgoing-area word,
	// in argument order regardless of which bank overflowed.
	enum class ArgSlotKind : u8 { IntReg, FloatReg, Stack };

	struct ArgSlot
	{
		ArgSlotKind kind = ArgSlotKind::IntReg;
		u32 index = 0; // a register number (0-3) for IntReg/FloatReg, or a word index (0-based) for Stack
	};

	// Assigns a slot to each argument in `isFloatArg` (declaration/evaluation order) - shared by a
	// Call site's own outgoing arguments and by a function's own incoming parameters, since the
	// rule is identical on both ends of the same call. Takes `const vector<bool>&`, not a span:
	// vector<bool>'s bit-packed specialization has no contiguous `bool*` to span over.
	std::vector<ArgSlot> assignArgSlots(const std::vector<bool>& isFloatArg);

	class FrameLayout
	{
	public:
		explicit FrameLayout(const ir::IrFunction& function);

	public:
		// How many outgoing-argument stack words (24-Calling-Convention.md's "Passing arguments")
		// this function needs to reserve, sized for the largest call it makes - 0 if it calls
		// nothing or every call it makes fits entirely in argument registers.
		u32 outgoingSlotCount() const noexcept { return _outgoingSlotCount; }

	private:
		u32 _outgoingSlotCount = 0;
	};
}
