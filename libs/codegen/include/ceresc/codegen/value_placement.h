#pragma once

#include <ceresc/codegen/frame_layout.h>
#include <ceresc/ir/ir_function.h>
#include <ceresc/support/optimization.h>
#include <optional>
#include <span>
#include <vector>

// ValuePlacement - decides, for one IrFunction, where every value actually lives while the
// function runs: in a machine register, or in a field of its stack frame. This is §10's
// "register window" rule, and the analysis the frameless-leaf decision rests on.
//
// What it computes, in order:
//
//   1. Liveness. A standard backward dataflow over the CFG (live-in/live-out per block, iterated
//      to a fixpoint), because a linear scan over the instruction list is NOT sound here: a value
//      defined before a loop and last read inside it is still live across the whole back edge, and
//      a naive "def index .. last use index" interval would happily hand its slot to something
//      defined after that last use and clobber it on the next iteration.
//
//   2. Escape analysis for locals. A local can only live in a register if its address is never
//      taken as a value - which, in this IR, means every FrameAddr naming it is consumed solely as
//      the address operand of a Load/Store (ir_instr.h). The moment such an address is passed to
//      anything else (an argument, a stored value, pointer arithmetic), the local needs a real
//      memory home. Getting this wrong is the sharp edge frameless-leaf used to be avoided for -
//      this is the analysis that makes turning it on safe.
//
//   3. Register assignment. Locals first (they live for the whole function, so their registers are
//      reserved for its whole length), then temporaries by a per-block linear scan that frees a
//      register again after the temporary's last read.
//
//   4. Frame slots for everything left over, reusing one slot for several temporaries whose live
//      ranges do not overlap.
//
// Two register-eligibility rules carry the ABI (24-Calling-Convention.md) rather than any analysis:
//
//   - A value may only stay in a register across a stretch with no `call` in it, because a call
//     clobbers r0-r7, r12 and f0-f7. Locals therefore only get registers in a function that calls
//     nothing at all; a temporary gets one whenever its own live range is call-free.
//   - r8-r11/f8-f15 (the callee-saved half) stay unused, exactly as §10 says for v1: using them
//     would mean saving and restoring them around every function that touches one. Promoting
//     long-lived locals into them is the remaining Fase 9 item this class deliberately leaves open.
//
// With support::OptimizationOptions::registerAllocation off, every value gets its own permanent
// frame field and no register - the simplified rule the golden tests still pin at -O0 (see
// support/optimization.h's own header comment on why that path stays alive).

namespace ceresc::codegen
{
	enum class PlacementKind : u8
	{
		None,		// nothing left in the IR mentions this value, so it needs no home at all
		Register,	// lives in `index` of its own bank for as long as it is live
		Slot,		// lives in frame field `index`
		Virtual		// a FrameAddr naming a register-resident local: there is no address to compute
	};

	struct Placement
	{
		PlacementKind kind = PlacementKind::Slot;
		u32 index = 0;
		bool isFloat = false;
	};

	// One frame field: the size and bank codegen declares it with in the function's CASM `struct`.
	struct FrameSlotInfo
	{
		u32 sizeInBytes = 4;
		bool isFloat = false;
	};

	class ValuePlacement
	{
	public:
		ValuePlacement(const ir::IrFunction& function, const support::OptimizationOptions& options);

	public:
		Placement temp(ir::IrValue value) const;
		Placement local(u32 localIndex) const;

		// The local a FrameAddr temporary names, when that local lives in a register and the
		// address therefore never materializes - codegen turns the Load/Store through it into a
		// register move instead. Empty for every ordinary (real, computable) address.
		std::optional<u32> virtualAddressLocal(ir::IrValue value) const;

		// True when this function needs a stack frame at all: it spills something, passes arguments
		// on the stack, or receives a parameter there (which is read through `fp`, and only `enter`
		// sets `fp` up). A frameless function emits no enter/leave and just returns.
		bool needsFrame() const noexcept { return _needsFrame; }

		u32 outgoingSlotCount() const noexcept { return _outgoingSlotCount; }
		std::span<const FrameSlotInfo> slots() const noexcept { return _slots; }

		// Where each of this function's own parameters arrives, by the same rule a call site places
		// its arguments (frame_layout.h's assignArgSlots()).
		std::span<const ArgSlot> paramArrival() const noexcept { return _paramArrival; }

	private:
		bool _needsFrame = true;
		u32 _outgoingSlotCount = 0;
		std::vector<FrameSlotInfo> _slots;
		std::vector<Placement> _temps;   // indexed by IrValue::id
		std::vector<Placement> _locals;  // indexed by local slot index
		std::vector<u32> _virtualAddress; // per temp: the local a virtual FrameAddr names, or ~0u
		std::vector<ArgSlot> _paramArrival;
	};
}
