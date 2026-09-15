#include <ceresc/ir/ir_printer.h>
#include <format>

namespace ceresc::ir
{
	std::string IrPrinter::valueName(IrValue value) { return value.isValid() ? std::format("%{}", value.id) : "<invalid>"; }
	std::string IrPrinter::blockName(const BasicBlock& block) { return std::format("L{}", block.id()); }

	std::string_view IrPrinter::opName(IrBinOp op) noexcept
	{
		switch (op)
		{
			case IrBinOp::Add: return "add";
			case IrBinOp::Sub: return "sub";
			case IrBinOp::Mul: return "mul";
			case IrBinOp::Div: return "div";
			case IrBinOp::Mod: return "mod";
			case IrBinOp::And: return "and";
			case IrBinOp::Or: return "or";
			case IrBinOp::Xor: return "xor";
			case IrBinOp::Shl: return "shl";
			case IrBinOp::Shr: return "shr";
			case IrBinOp::Sar: return "sar";
		}
		return "<unknown-binop>";
	}

	std::string_view IrPrinter::opName(IrUnOp op) noexcept
	{
		switch (op)
		{
			case IrUnOp::Neg: return "neg";
			case IrUnOp::Not: return "not";
			case IrUnOp::LogicalNot: return "lnot";
			case IrUnOp::IntToFloat: return "itof";
			case IrUnOp::FloatToInt: return "ftoi";
		}
		return "<unknown-unop>";
	}

	std::string_view IrPrinter::predName(IrCmpPredicate predicate) noexcept
	{
		switch (predicate)
		{
			case IrCmpPredicate::Eq: return "eq";
			case IrCmpPredicate::Ne: return "ne";
			case IrCmpPredicate::Lt: return "lt";
			case IrCmpPredicate::Le: return "le";
			case IrCmpPredicate::Gt: return "gt";
			case IrCmpPredicate::Ge: return "ge";
		}
		return "<unknown-pred>";
	}

	std::string_view IrPrinter::sizeName(IrMemSize size) noexcept
	{
		switch (size)
		{
			case IrMemSize::Byte: return "byte";
			case IrMemSize::Half: return "half";
			case IrMemSize::Word: return "word";
		}
		return "<unknown-size>";
	}

	std::string IrPrinter::print(const IrModule& module)
	{
		_output.clear();
		for (const auto& function : module.functions())
			printFunction(*function);
		for (const IrGlobalString& literal : module.stringLiterals())
			_output += std::format("global \"{}\" = \"{}\"\n", literal.name, literal.value.view());
		return _output;
	}

	std::string IrPrinter::print(const IrFunction& function)
	{
		_output.clear();
		printFunction(function);
		return _output;
	}

	void IrPrinter::printFunction(const IrFunction& function)
	{
		_output += std::format("function {}(params={}, locals={}) {{\n", function.name(), function.paramCount(), function.localCount());
		for (const auto& block : function.blocks())
			printBlock(*block);
		_output += "}\n";
	}

	void IrPrinter::printBlock(const BasicBlock& block)
	{
		_output += std::format("{}:\n", blockName(block));
		for (const IrInstr* instr : block.instrs())
			printInstr(*instr);
	}

	void IrPrinter::printInstr(const IrInstr& instr)
	{
		_output += "  ";
		switch (instr.opcode())
		{
			case IrOpcode::Const:
			{
				const auto& payload = instr.as<IrConstPayload>();
				if (payload.isFloat)
					_output += std::format("{} = const {}\n", valueName(payload.result), payload.floatValue);
				else
					_output += std::format("{} = const {}\n", valueName(payload.result), payload.intValue);
				break;
			}
			case IrOpcode::BinOp:
			{
				const auto& payload = instr.as<IrBinOpPayload>();
				_output += std::format("{} = {}{}{} {}, {}\n", valueName(payload.result), opName(payload.op),
					payload.isFloat ? ".f" : "", payload.isUnsigned ? ".u" : "",
					valueName(payload.lhs), valueName(payload.rhs));
				break;
			}
			case IrOpcode::UnOp:
			{
				const auto& payload = instr.as<IrUnOpPayload>();
				_output += std::format("{} = {}{}{} {}\n", valueName(payload.result), opName(payload.op),
					payload.isFloat ? ".f" : "", payload.isUnsigned ? ".u" : "", valueName(payload.operand));
				break;
			}
			case IrOpcode::Cmp:
			{
				const auto& payload = instr.as<IrCmpPayload>();
				_output += std::format("{} = cmp.{}{}{} {}, {}\n", valueName(payload.result), predName(payload.predicate),
					payload.isFloat ? ".f" : "", payload.isUnsigned ? ".u" : "",
					valueName(payload.lhs), valueName(payload.rhs));
				break;
			}
			case IrOpcode::Copy:
			{
				const auto& payload = instr.as<IrCopyPayload>();
				_output += payload.isFloat
					? std::format("{} = copy.f {}\n", valueName(payload.result), valueName(payload.source))
					: std::format("{} = {}\n", valueName(payload.result), valueName(payload.source));
				break;
			}
			case IrOpcode::FrameAddr:
			{
				const auto& payload = instr.as<IrFrameAddrPayload>();
				_output += std::format("{} = &local {}\n", valueName(payload.result), payload.localIndex);
				break;
			}
			case IrOpcode::GlobalAddr:
			{
				const auto& payload = instr.as<IrGlobalAddrPayload>();
				_output += std::format("{} = &global \"{}\"\n", valueName(payload.result), payload.name);
				break;
			}
			case IrOpcode::Load:
			{
				const auto& payload = instr.as<IrLoadPayload>();
				_output += std::format("{} = load.{}{} [{}]\n", valueName(payload.result), sizeName(payload.size),
					payload.isFloat ? ".f" : "", valueName(payload.address));
				break;
			}
			case IrOpcode::Store:
			{
				const auto& payload = instr.as<IrStorePayload>();
				_output += std::format("store.{}{} [{}], {}\n", sizeName(payload.size), payload.isFloat ? ".f" : "",
					valueName(payload.address), valueName(payload.value));
				break;
			}
			case IrOpcode::Param:
			{
				const auto& payload = instr.as<IrParamPayload>();
				_output += std::format("param{} {}\n", payload.isFloat ? ".f" : "", valueName(payload.value));
				break;
			}
			case IrOpcode::Call:
			{
				const auto& payload = instr.as<IrCallPayload>();
				if (payload.hasResult)
					_output += payload.isFloat
						? std::format("{} = call.f {}, {}\n", valueName(payload.result), payload.callee, payload.argCount)
						: std::format("{} = call {}, {}\n", valueName(payload.result), payload.callee, payload.argCount);
				else
					_output += std::format("call {}, {}\n", payload.callee, payload.argCount);
				break;
			}
			case IrOpcode::Jump:
			{
				const auto& payload = instr.as<IrJumpPayload>();
				_output += std::format("jmp {}\n", blockName(*payload.target));
				break;
			}
			case IrOpcode::CondJump:
			{
				const auto& payload = instr.as<IrCondJumpPayload>();
				_output += std::format("br.{}{}{} {}, {}, {}, {}\n", predName(payload.predicate), payload.isFloat ? ".f" : "", payload.isUnsigned ? ".u" : "",
					valueName(payload.lhs), valueName(payload.rhs), blockName(*payload.trueTarget), blockName(*payload.falseTarget));
				break;
			}
			case IrOpcode::Return:
			{
				const auto& payload = instr.as<IrReturnPayload>();
				if (payload.hasValue)
					_output += std::format("ret{} {}\n", payload.isFloat ? ".f" : "", valueName(payload.value));
				else
					_output += "ret\n";
				break;
			}
		}
	}
}
