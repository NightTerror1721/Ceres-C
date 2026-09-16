#pragma once

#include <ceresc/ir/ir_function.h>
#include <ceresc/ir/ir_instr.h>
#include <span>
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
// WHICH values end up in the frame at all - and whether there is a frame to begin with - is not
// decided here either. ValuePlacement (value_placement.h) owns that: the escape analysis that makes
// §10's frameless-leaf rule safe, the liveness dataflow behind its register window, and the frame
// fields left over for whatever neither can hold. This class only answers the one question that is
// pure ABI arithmetic and needs no analysis at all - how wide the outgoing-argument area has to be.
//
// Both of those decisions have a simplified counterpart that is still reachable and still tested
// (-O0, or -fno-frameless-leaf / -fno-regalloc): a real frame for every function, and one permanent
// field per local, parameter and temporary. That is the shape this file used to describe as the
// only one, and it stays alive on purpose - it needs no analysis to be correct, so it is the thing
// you bisect against when an optimized program misbehaves. See support/optimization.h's own header
// comment, and §0's "comprensible antes que completo".
//
// Implemented in Fase 6 of the phased plan (§13); the placement optimizations are §13's Fase 9.

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
	//
	// `fixedArgCount` is where the callee's declared parameter list ends. Arguments at or past it
	// are the variadic tail and go to the outgoing stack area unconditionally - never to an
	// argument register, however many are still free (docs/09-Variadic-Convention.md). That is what
	// makes the tail findable from the callee's side: the callee knows how many stack words its own
	// fixed parameters consumed, so the next word is where the tail begins, whereas a register-
	// passed argument would be indistinguishable from a fixed one. The default means "no tail at
	// all", which is every ordinary call and every function's own incoming parameter list.
	std::vector<ArgSlot> assignArgSlots(const std::vector<bool>& isFloatArg, u32 fixedArgCount = ~0u);

	// Where the variadic tail begins in one call's run of Param instructions, or ~0u when that
	// call has none - the `fixedArgCount` to hand assignArgSlots() for those same arguments.
	u32 fixedArgCountOf(std::span<ir::IrInstr* const> params);

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
