#include <ceresc/codegen/frame_layout.h>
#include <algorithm>

namespace ceresc::codegen
{
	std::vector<ArgSlot> assignArgSlots(const std::vector<bool>& isFloatArg,
		const std::vector<bool>& isWideArg, u32 fixedArgCount)
	{
		// ONE entry per ARGUMENT (not per slot) - the caller and callee both index this by argument
		// number, so a wide argument that needs two slots stays one entry whose `index` is the first
		// slot of its kind and whose `wide` flag says to read the second at `index + 1`. Splitting a
		// wide argument into two entries here would misalign every later argument.
		std::vector<ArgSlot> result;
		result.reserve(isFloatArg.size());
		u32 intUsed = 0, floatUsed = 0, stackUsed = 0;
		for (u32 i = 0; i < isFloatArg.size(); ++i)
		{
			bool isFloat = isFloatArg[i];
			bool isWide = i < isWideArg.size() && isWideArg[i];
			if (i >= fixedArgCount)
			{
				// The variadic tail: stack, whatever bank it would otherwise have used and however
				// many registers are still free - see the header comment.
				result.push_back(ArgSlot{ ArgSlotKind::Stack, stackUsed, isWide });
				stackUsed += isWide ? 2 : 1;
			}
			else if (isFloat)
			{
				if (floatUsed < 4)
					result.push_back(ArgSlot{ ArgSlotKind::FloatReg, floatUsed++, isWide });
				else
				{
					result.push_back(ArgSlot{ ArgSlotKind::Stack, stackUsed, isWide });
					stackUsed += isWide ? 2 : 1;
				}
			}
			else
			{
				// A 64-bit argument needs two consecutive registers; it fits only when the bank has
				// TWO left, and otherwise the whole pair goes to the stack (so the callee always
				// reads the second word at `index + 1` of the same kind).
				u32 needed = isWide ? 2u : 1u;
				if (intUsed + needed <= 4)
				{
					result.push_back(ArgSlot{ ArgSlotKind::IntReg, intUsed, isWide });
					intUsed += needed;
				}
				else
				{
					result.push_back(ArgSlot{ ArgSlotKind::Stack, stackUsed, isWide });
					stackUsed += needed;
				}
			}
		}
		return result;
	}

	u32 fixedArgCountOf(std::span<ir::IrInstr* const> params)
	{
		// The first Param carrying isVariadicArg is where the tail starts; none means no tail. The
		// flag is set per argument by IrBuilder, which is the only place that knows the callee's
		// declared arity, so both this file and codegen.cpp read the split off the IR itself rather
		// than each re-resolving the callee by name.
		for (u32 i = 0; i < params.size(); ++i)
			if (params[i]->opcode() == ir::IrOpcode::Param && params[i]->as<ir::IrParamPayload>().isVariadicArg)
				return i;
		return ~0u;
	}

	namespace
	{
		// The single largest outgoing area this function's own calls need - see assignArgSlots()'s
		// header comment. A Call's Param instructions are always the `argCount` instructions
		// immediately preceding it in the same block: IrBuilder fully evaluates every argument
		// expression (which may itself open its own blocks, e.g. a ternary argument) before
		// emitting any Param for this call, then emits all of this call's Params back to back right
		// before it (ir_builder.cpp's visit(CallExpr&)) - so this never has to look past a
		// control-flow edge to find them.
		u32 computeOutgoingSlotCount(const ir::IrFunction& function)
		{
			u32 maxSlots = 0;
			for (const auto& block : function.blocks())
			{
				std::span<ir::IrInstr* const> instrs = block->instrs();
				for (usize i = 0; i < instrs.size(); ++i)
				{
					if (instrs[i]->opcode() != ir::IrOpcode::Call)
						continue;

					const auto& call = instrs[i]->as<ir::IrCallPayload>();
					u32 argCount = call.argCount;
					if (argCount > i)
						continue; // malformed IR (shouldn't happen from IrBuilder) - skip rather than underflow

					std::vector<bool> isFloatArg(argCount);
					std::vector<bool> isWideArg(argCount);
					for (u32 k = 0; k < argCount; ++k)
					{
						const ir::IrInstr& param = *instrs[i - argCount + k];
						if (param.opcode() != ir::IrOpcode::Param)
						{
							isFloatArg.clear();
							break;
						}
						isFloatArg[k] = param.as<ir::IrParamPayload>().isFloat;
						isWideArg[k] = param.as<ir::IrParamPayload>().isWide;
					}
					if (isFloatArg.size() != argCount)
						continue;

					std::span<ir::IrInstr* const> params = instrs.subspan(i - argCount, argCount);
					std::vector<ArgSlot> slots = assignArgSlots(isFloatArg, isWideArg, fixedArgCountOf(params));
					u32 stackSlots = 0;
					for (const ArgSlot& slot : slots)
						if (slot.kind == ArgSlotKind::Stack)
							stackSlots = std::max(stackSlots, slot.index + (slot.wide ? 2u : 1u));
					maxSlots = std::max(maxSlots, stackSlots);
				}
			}
			return maxSlots;
		}
	}

	FrameLayout::FrameLayout(const ir::IrFunction& function) :
		_outgoingSlotCount(computeOutgoingSlotCount(function))
	{}
}
