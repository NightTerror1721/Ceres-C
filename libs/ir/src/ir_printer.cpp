#include <ceresc/ir/ir_printer.h>
#include <format>
#include <string>

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
			case IrUnOp::Narrow: return "narrow";
			case IrUnOp::ToBool: return "tobool";
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
		{
			if (literal.elementSize == 1)
			{
				_output += std::format("global \"{}\" = \"{}\"\n", literal.name, literal.value.view());
				continue;
			}
			// A wide literal: its code units, which as bytes would print as noise.
			std::string_view bytes = literal.value.view();
			_output += std::format("global \"{}\" = u{}[", literal.name, literal.elementSize * 8);
			for (usize i = 0; i + literal.elementSize <= bytes.size(); i += literal.elementSize)
			{
				u32 unit = 0;
				for (u32 b = 0; b < literal.elementSize; ++b)
					unit |= static_cast<u32>(static_cast<u8>(bytes[i + b])) << (8 * b);
				_output += std::format("{}{}", i == 0 ? "" : ", ", unit);
			}
			_output += "]\n";
		}
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
		_output += std::format("function {}(params={}{}, locals={}) {{\n", function.name(), function.paramCount(),
			function.isVariadic() ? ", ..." : "", function.localCount());
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
				// Narrow prints its width too - "%4 = narrow.byte.u %3" - since that is the whole
				// content of the instruction: two narrows of different widths would read identically.
				std::string suffix = payload.op == IrUnOp::Narrow ? std::format(".{}", sizeName(payload.narrowSize)) : std::string{};
				_output += std::format("{} = {}{}{}{} {}\n", valueName(payload.result), opName(payload.op),
					suffix, payload.isFloat ? ".f" : "", payload.isUnsigned ? ".u" : "", valueName(payload.operand));
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
				// ".s" marks a sign-extending narrow load (`ldrsb`/`ldrsh`), a different instruction
				// from the zero-extending one and not deducible from the size alone.
				_output += std::format("{} = load.{}{}{}{} [{}]\n", valueName(payload.result), sizeName(payload.size),
					payload.isFloat ? ".f" : "", (payload.isSigned && payload.size != IrMemSize::Word) ? ".s" : "",
					payload.isVolatile ? ".v" : "", valueName(payload.address));
				break;
			}
			case IrOpcode::Store:
			{
				const auto& payload = instr.as<IrStorePayload>();
				_output += std::format("store.{}{}{} [{}], {}\n", sizeName(payload.size), payload.isFloat ? ".f" : "",
					payload.isVolatile ? ".v" : "", valueName(payload.address), valueName(payload.value));
				break;
			}
			case IrOpcode::Param:
			{
				const auto& payload = instr.as<IrParamPayload>();
				_output += std::format("param{}{}{} {}\n", payload.isWide ? ".wide" : "",
					payload.isFloat ? ".f" : "", payload.isVariadicArg ? ".var" : "",
					valueName(payload.value));
				break;
			}
			case IrOpcode::Call:
			{
				const auto& payload = instr.as<IrCallPayload>();
				// An indirect call names no symbol, so it prints the temporary it jumps through:
				// `call %7, 1` rather than `call f, 1`. A 64-bit result defines two temporaries (the
				// low word and the high, ret0/ret1), so it is printed as a pair.
				std::string target = payload.isIndirect()
					? std::string(valueName(payload.calleeValue))
					: std::string(payload.callee);
				if (payload.hasWideResult)
					_output += std::format("{}:{} = call.wide {}, {}\n",
						valueName(payload.result), valueName(payload.resultHigh), target, payload.argCount);
				else if (payload.hasResult)
					_output += payload.isFloat
						? std::format("{} = call.f {}, {}\n", valueName(payload.result), target, payload.argCount)
						: std::format("{} = call {}, {}\n", valueName(payload.result), target, payload.argCount);
				else
					_output += std::format("call {}, {}\n", target, payload.argCount);
				break;
			}
			case IrOpcode::VaStart:
			{
				const auto& payload = instr.as<IrVaStartPayload>();
				_output += std::format("{} = va_start\n", valueName(payload.result));
				break;
			}
			case IrOpcode::MachineOp:
			{
				_output += ast::machineOpMnemonic(instr.as<IrMachineOpPayload>().op);
				_output += '\n';
				break;
			}
			case IrOpcode::Builtin:
			{
				const auto& payload = instr.as<IrBuiltinPayload>();
				_output += std::format("{} = {}", valueName(payload.result), ast::builtinName(payload.builtin));
				_output += std::format(" {}", valueName(payload.a));
				if (payload.b.isValid())
					_output += std::format(", {}", valueName(payload.b));
				_output += '\n';
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
				if (payload.hasWideValue)
					_output += std::format("ret.wide {}:{}\n", valueName(payload.value), valueName(payload.highValue));
				else if (payload.hasValue)
					_output += std::format("ret{} {}\n", payload.isFloat ? ".f" : "", valueName(payload.value));
				else
					_output += "ret\n";
				break;
			}
			case IrOpcode::TableJump:
			{
				// `tbl.jmp %4 - 1, [L1, L2, L3], default L4` - the low value, the entries in order
				// (entry i is `low + i`), and where an out-of-range value goes.
				const auto& payload = instr.as<IrTableJumpPayload>();
				std::string entries;
				for (u32 i = 0; i < payload.entryCount; ++i)
				{
					if (i)
						entries += ", ";
					entries += blockName(*payload.targets[i]);
				}
				_output += std::format("tbl.jmp {} - {}, [{}], default {}\n", valueName(payload.discriminant),
					payload.low, entries, blockName(*payload.defaultTarget));
				break;
			}
		}
	}
}
