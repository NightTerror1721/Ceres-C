#include <ceresc/codegen/frame_layout.h>
#include <algorithm>

namespace ceresc::codegen
{
	std::vector<ArgSlot> assignArgSlots(const std::vector<bool>& isFloatArg)
	{
		std::vector<ArgSlot> result;
		result.reserve(isFloatArg.size());
		u32 intUsed = 0, floatUsed = 0, stackUsed = 0;
		for (bool isFloat : isFloatArg)
		{
			if (isFloat)
			{
				if (floatUsed < 4) result.push_back(ArgSlot{ ArgSlotKind::FloatReg, floatUsed++ });
				else result.push_back(ArgSlot{ ArgSlotKind::Stack, stackUsed++ });
			}
			else
			{
				if (intUsed < 4) result.push_back(ArgSlot{ ArgSlotKind::IntReg, intUsed++ });
				else result.push_back(ArgSlot{ ArgSlotKind::Stack, stackUsed++ });
			}
		}
		return result;
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
					for (u32 k = 0; k < argCount; ++k)
					{
						const ir::IrInstr& param = *instrs[i - argCount + k];
						if (param.opcode() != ir::IrOpcode::Param)
						{
							isFloatArg.clear();
							break;
						}
						isFloatArg[k] = param.as<ir::IrParamPayload>().isFloat;
					}
					if (isFloatArg.size() != argCount)
						continue;

					std::vector<ArgSlot> slots = assignArgSlots(isFloatArg);
					u32 stackSlots = 0;
					for (const ArgSlot& slot : slots)
						if (slot.kind == ArgSlotKind::Stack)
							stackSlots = std::max(stackSlots, slot.index + 1);
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
