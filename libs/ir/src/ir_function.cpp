#include <ceresc/ir/ir_function.h>

namespace ceresc::ir
{
	bool BasicBlock::isTerminated() const noexcept
	{
		if (_instrs.empty())
			return false;

		switch (_instrs.back()->opcode())
		{
			case IrOpcode::Jump:
			case IrOpcode::CondJump:
			case IrOpcode::Return:
				return true;
			default:
				return false;
		}
	}
}
