#include <ceresc/codegen/codegen.h>
#include <ceresc/ast/ast_printer.h>
#include <ceresc/ast/expr.h>
#include <ceresc/ast/stmt.h>

#include <algorithm>
#include <bit>
#include <optional>
#include <vector>

// See codegen.h for the overall register-allocation rule (every local/parameter/temporary lives
// in its own frame field, always) and why a Cmp is never fused into a following CondJump.
//
// Every mnemonic below is checked against CeresASM's own docs/05-Instruction-Set.md and
// docs/06-Pseudo-Instructions.md - none is invented. Two auto-dispatch rules those pages document
// are used throughout instead of picking an F-prefixed opcode by hand: `add`/`sub`/`ldr`/`str`/
// `neg`/`ifXX`/... all choose their float form (FADD/FLDR/FNEG/FCMP/...) on their own once given a
// float register operand - the mnemonic text this file writes never changes between the int and
// float form, only which register name (`r4` vs `f4`) it names.

namespace ceresc::codegen
{
	using namespace ast;
	using namespace ir;
	using support::SourceLocation;

	namespace
	{
		constexpr u32 kScratchA = 4;
		constexpr u32 kScratchB = 5;

		std::string intReg(u32 n) { return std::format("r{}", n); }
		std::string floatReg(u32 n) { return std::format("f{}", n); }
		std::string_view storeMnemonicFor(u32 sizeInBytes, bool isFloat)
		{
			if (isFloat) return "str";
			if (sizeInBytes == 1) return "strb";
			if (sizeInBytes == 2) return "strh";
			return "str";
		}

		// A C identifier is not guaranteed to avoid CASM's own reserved words
		// (10-Language-Syntax.md: `let`/`const`/`global`/`import`/`macro`/`endmacro`/`alias`/
		// `struct`/`endstruct`, every type name including `byte`/`half`/`word`, `true`/`false`) - a
		// Ceres-C function or global variable happening to be named exactly one of those would
		// otherwise fail to assemble with a confusing syntax error instead of a Ceres-C diagnostic.
		// Prefixing every user symbol sidesteps the whole collision class outright, without having
		// to enumerate and track CASM's reserved-word list here. `main` is the one exception: it
		// must keep its literal spelling, since the linker looks up a symbol named exactly `main`
		// to set the program's entry point (12-Labels-and-Symbols.md) - the CASM keyword list has no
		// entry that collides with it anyway.
		//
		// Also handles IrModule's own synthesized string-literal labels (".str0", ".str1", ... -
		// ir_builder.cpp's visit(StringLiteralExpr&)): a leading `.` means a LOCAL label in CASM
		// (scoped to the nearest preceding global one, 10-Language-Syntax.md), not a file-scope data
		// symbol, and an identifier cannot start with `.` at all - so `.str0` would fail to parse as
		// a `let` name verbatim. Both this function and generateStringLiterals()/the GlobalAddr case
		// below go through this same mangling, so a reference and its declaration always agree.
		std::string mangledName(std::string_view name)
		{
			if (name == "main")
				return std::string(name);
			if (!name.empty() && name.front() == '.')
				return std::format("cc{}", name.substr(1));
			return std::format("cc_{}", name);
		}

		// Finds the FunctionDecl matching `name` among `unit`'s top-level declarations - IrModule
		// only carries a function's lowered body (ir_function.h), never the original AST node, so
		// CodeGen needs this to recover parameter count/types for the frame/prologue.
		const FunctionDecl* findFunctionDecl(const TranslationUnit& unit, std::string_view name)
		{
			for (Decl* decl : unit.decls())
				if (auto* func = dynamic_cast<FunctionDecl*>(decl); func && func->isDefinition() && func->name() == name)
					return func;
			return nullptr;
		}

		// A small compile-time constant evaluator for a global's own initializer - `.data`/`.bss`
		// need a literal at ASSEMBLE time, not a runtime instruction. Deliberately narrow (a
		// literal, optionally with a leading unary minus): sema::Sema and IrBuilder each already
		// carry their own similarly narrow evaluator for a different narrow purpose (enum values/
		// switch labels; folding `sizeof` and friends into IR - see ir_builder.h's own note on why
		// that one doesn't just call into sema's), and a global initializer's job here is narrower
		// still. `nullopt` asks the caller to diagnose rather than guess.
		std::optional<i64> foldGlobalInt(const Expr* expr)
		{
			if (const auto* lit = dynamic_cast<const IntLiteralExpr*>(expr))
				return static_cast<i64>(lit->value());
			if (const auto* lit = dynamic_cast<const CharLiteralExpr*>(expr))
				return static_cast<i64>(lit->value());
			if (const auto* lit = dynamic_cast<const BoolLiteralExpr*>(expr))
				return lit->value() ? i64(1) : i64(0);
			if (const auto* unary = dynamic_cast<const UnaryExpr*>(expr); unary && unary->op() == UnaryOp::Negate)
			{
				std::optional<i64> operand = foldGlobalInt(unary->operand());
				return operand ? std::optional<i64>(-*operand) : std::nullopt;
			}
			return std::nullopt;
		}

		std::optional<f32> foldGlobalFloat(const Expr* expr)
		{
			if (const auto* lit = dynamic_cast<const FloatLiteralExpr*>(expr))
				return lit->value();
			if (const auto* lit = dynamic_cast<const IntLiteralExpr*>(expr))
				return static_cast<f32>(lit->value());
			if (const auto* unary = dynamic_cast<const UnaryExpr*>(expr); unary && unary->op() == UnaryOp::Negate)
			{
				std::optional<f32> operand = foldGlobalFloat(unary->operand());
				return operand ? std::optional<f32>(-*operand) : std::nullopt;
			}
			return std::nullopt;
		}

		// Escapes a decoded string literal's bytes back into valid CASM string-literal text
		// (11-Data-Types-and-Literals.md's own escapes) - the PooledString this reads from already
		// decoded `\n`/`\t`/etc. into real bytes (libs/lexer's own job), so re-emitting it verbatim
		// into a NEW .casm string literal would be wrong the moment it contains one of the
		// characters that needed escaping in the first place.
		std::string escapeCasmString(std::string_view value)
		{
			std::string result;
			result.reserve(value.size());
			for (char c : value)
			{
				switch (c)
				{
					case '\n': result += "\\n"; break;
					case '\t': result += "\\t"; break;
					case '\r': result += "\\r"; break;
					case '\\': result += "\\\\"; break;
					case '"':  result += "\\\""; break;
					case '\0': result += "\\0"; break;
					default:   result += c; break;
				}
			}
			return result;
		}
	}

	std::string CodeGen::sourceComment(SourceLocation location) const
	{
		if (!location.isValid())
			return {};
		const support::SourceBuffer* buffer = _sourceManager.getBuffer(location.sourceId);
		return std::format("{}:{}", buffer ? buffer->name() : std::string_view("?"), location.line);
	}

	std::string CodeGen::localFieldName(u32 slotIndex) { return std::format("local{}", slotIndex); }
	std::string CodeGen::tempFieldName(u32 tempId) { return std::format("t{}", tempId); }

	std::string CodeGen::fieldTypeName(u32 sizeInBytes, bool isFloat)
	{
		if (isFloat)
			return "f32";
		switch (sizeInBytes)
		{
			case 1: return "u8";
			case 2: return "u16";
			case 4: return "u32";
			default: return std::format("u32[{}]", (sizeInBytes + 3) / 4);
		}
	}

	std::string_view CodeGen::ifMnemonic(IrCmpPredicate predicate, bool isUnsigned)
	{
		switch (predicate)
		{
			case IrCmpPredicate::Eq: return "ifeq";
			case IrCmpPredicate::Ne: return "ifne";
			case IrCmpPredicate::Lt: return isUnsigned ? "ifbl" : "ifls";
			case IrCmpPredicate::Le: return isUnsigned ? "ifbe" : "ifle";
			case IrCmpPredicate::Gt: return isUnsigned ? "ifab" : "ifgr";
			case IrCmpPredicate::Ge: return isUnsigned ? "ifae" : "ifge";
		}
		return "ifeq";
	}

	std::string CodeGen::tempAddress(IrValue value) const
	{
		return std::format("[sp + {}.{}]", _frameName, tempFieldName(value.id));
	}

	std::string CodeGen::localAddress(u32 slotIndex) const
	{
		return std::format("[sp + {}.{}]", _frameName, localFieldName(slotIndex));
	}

	void CodeGen::loadTemp(IrValue value, std::string_view reg, bool isFloat, SourceLocation loc)
	{
		if (!value.isValid())
			return;
		(void)isFloat; // `ldr`/`str` auto-dispatch on `reg`'s own bank - see the header comment above
		_emitter.instr(std::format("ldr {}, {}", reg, tempAddress(value)), sourceComment(loc));
	}

	void CodeGen::storeTemp(IrValue value, std::string_view reg, bool isFloat, SourceLocation loc)
	{
		if (!value.isValid())
			return;
		(void)isFloat;
		_emitter.instr(std::format("str {}, {}", tempAddress(value), reg), sourceComment(loc));
	}

	void CodeGen::emitLoadImmediate(std::string_view reg, i64 rawValue, SourceLocation loc)
	{
		i32 value = static_cast<i32>(rawValue); // wrap to 32 bits - matches C `int` overflow semantics
		std::string comment = sourceComment(loc);
		if (value >= 0 && value <= 0xFFFF)
			_emitter.instr(std::format("li {}, {}", reg, value), comment);
		else
			_emitter.instr(std::format("la {}, {}", reg, value), comment);
	}

	void CodeGen::materializeCmp(const IrCmpPayload& payload, std::string_view resultReg, SourceLocation loc)
	{
		std::string comment = sourceComment(loc);
		std::string a = payload.isFloat ? floatReg(kScratchA) : intReg(kScratchA);
		std::string b = payload.isFloat ? floatReg(kScratchB) : intReg(kScratchB);
		loadTemp(payload.lhs, a, payload.isFloat, loc);
		loadTemp(payload.rhs, b, payload.isFloat, loc);

		std::string trueLabel = std::format("cmp{}_true", _nextComparisonLabel);
		std::string endLabel = std::format("cmp{}_end", _nextComparisonLabel);
		++_nextComparisonLabel;

		_emitter.instr(std::format("{} {}, {}, .{}", ifMnemonic(payload.predicate, payload.isUnsigned), a, b, trueLabel), comment);
		_emitter.instr(std::format("li {}, 0", resultReg), comment);
		_emitter.instr(std::format("jp .{}", endLabel), comment);
		_emitter.localLabel(trueLabel);
		_emitter.instr(std::format("li {}, 1", resultReg), comment);
		_emitter.localLabel(endLabel);
	}

	// ---- one IR instruction -------------------------------------------------------------------

	void CodeGen::generateInstr(std::span<IrInstr* const> instrs, usize index)
	{
		const IrInstr& instr = *instrs[index];
		SourceLocation loc = instr.location();
		std::string comment = sourceComment(loc);

		switch (instr.opcode())
		{
			case IrOpcode::Const:
			{
				const auto& p = instr.as<IrConstPayload>();
				if (p.isFloat)
				{
					// No float-immediate load exists (05-Instruction-Set.md/06-Pseudo-Instructions.md):
					// materialize the bit pattern into an int register, then reinterpret it into the
					// float bank with `mtf` (a raw copy, no numeric conversion - exactly what a bit
					// pattern needs).
					u32 bits = std::bit_cast<u32>(p.floatValue);
					emitLoadImmediate(intReg(kScratchA), static_cast<i64>(static_cast<i32>(bits)), loc);
					_emitter.instr(std::format("mtf {}, {}", floatReg(kScratchA), intReg(kScratchA)), comment);
					storeTemp(p.result, floatReg(kScratchA), true, loc);
				}
				else
				{
					emitLoadImmediate(intReg(kScratchA), p.intValue, loc);
					storeTemp(p.result, intReg(kScratchA), false, loc);
				}
				break;
			}

			case IrOpcode::BinOp:
			{
				const auto& p = instr.as<IrBinOpPayload>();
				std::string a = p.isFloat ? floatReg(kScratchA) : intReg(kScratchA);
				std::string b = p.isFloat ? floatReg(kScratchB) : intReg(kScratchB);
				loadTemp(p.lhs, a, p.isFloat, loc);
				loadTemp(p.rhs, b, p.isFloat, loc);

				std::string_view mnemonic;
				switch (p.op)
				{
					case IrBinOp::Add: mnemonic = "add"; break;
					case IrBinOp::Sub: mnemonic = "sub"; break;
					// FMUL/FDIV are the float forms of `mul`/`div` (unsigned), not `imul`/`idiv` -
					// 05-Instruction-Set.md's own table lists them there.
					case IrBinOp::Mul: mnemonic = (p.isFloat || p.isUnsigned) ? "mul" : "imul"; break;
					case IrBinOp::Div: mnemonic = (p.isFloat || p.isUnsigned) ? "div" : "idiv"; break;
					case IrBinOp::Mod: mnemonic = p.isUnsigned ? "mod" : "imod"; break; // never float - sema requires integer operands
					case IrBinOp::And: mnemonic = "and"; break;
					case IrBinOp::Or:  mnemonic = "or";  break;
					case IrBinOp::Xor: mnemonic = "xor"; break;
					case IrBinOp::Shl: mnemonic = "shl"; break;
					case IrBinOp::Shr: mnemonic = "shr"; break;
					case IrBinOp::Sar: mnemonic = "sar"; break;
				}
				_emitter.instr(std::format("{} {}, {}, {}", mnemonic, a, a, b), comment);
				storeTemp(p.result, a, p.isFloat, loc);
				break;
			}

			case IrOpcode::UnOp:
			{
				const auto& p = instr.as<IrUnOpPayload>();
				switch (p.op)
				{
					case IrUnOp::Neg:
					{
						std::string r = p.isFloat ? floatReg(kScratchA) : intReg(kScratchA);
						loadTemp(p.operand, r, p.isFloat, loc);
						_emitter.instr(std::format("neg {}, {}", r, r), comment); // pseudo: imul r,r,-1 (int) / FNEG (float)
						storeTemp(p.result, r, p.isFloat, loc);
						break;
					}
					case IrUnOp::Not:
					{
						std::string r = intReg(kScratchA);
						loadTemp(p.operand, r, false, loc);
						_emitter.instr(std::format("not {}, {}", r, r), comment);
						storeTemp(p.result, r, false, loc);
						break;
					}
					case IrUnOp::LogicalNot:
					{
						// Unreachable from IrBuilder today - visit(UnaryExpr&)'s LogicalNot case goes
						// through materializeBoolean() instead (ir_builder.cpp), never constructing
						// this payload. Handled anyway so this switch stays exhaustive: `!x` is `x == 0`.
						std::string r = intReg(kScratchA);
						loadTemp(p.operand, r, false, loc);
						std::string trueLabel = std::format("lnot{}_true", _nextComparisonLabel);
						std::string endLabel = std::format("lnot{}_end", _nextComparisonLabel);
						++_nextComparisonLabel;
						_emitter.instr(std::format("ifeq {}, 0, .{}", r, trueLabel), comment);
						_emitter.instr(std::format("li {}, 0", r), comment);
						_emitter.instr(std::format("jp .{}", endLabel), comment);
						_emitter.localLabel(trueLabel);
						_emitter.instr(std::format("li {}, 1", r), comment);
						_emitter.localLabel(endLabel);
						storeTemp(p.result, r, false, loc);
						break;
					}
					case IrUnOp::IntToFloat:
					{
						loadTemp(p.operand, intReg(kScratchA), false, loc);
						_emitter.instr(std::format("{} {}, {}", p.isUnsigned ? "itof" : "iitof", floatReg(kScratchA), intReg(kScratchA)), comment);
						storeTemp(p.result, floatReg(kScratchA), true, loc);
						break;
					}
					case IrUnOp::FloatToInt:
					{
						loadTemp(p.operand, floatReg(kScratchA), true, loc);
						_emitter.instr(std::format("{} {}, {}", p.isUnsigned ? "ftoi" : "ftoii", intReg(kScratchA), floatReg(kScratchA)), comment);
						storeTemp(p.result, intReg(kScratchA), false, loc);
						break;
					}
				}
				break;
			}

			case IrOpcode::Cmp:
			{
				const auto& p = instr.as<IrCmpPayload>();
				std::string r = intReg(kScratchA);
				materializeCmp(p, r, loc);
				storeTemp(p.result, r, false, loc);
				break;
			}

			case IrOpcode::Copy:
			{
				const auto& p = instr.as<IrCopyPayload>();
				std::string r = p.isFloat ? floatReg(kScratchA) : intReg(kScratchA);
				loadTemp(p.source, r, p.isFloat, loc);
				storeTemp(p.result, r, p.isFloat, loc);
				break;
			}

			case IrOpcode::FrameAddr:
			{
				const auto& p = instr.as<IrFrameAddrPayload>();
				std::string r = intReg(kScratchA);
				_emitter.instr(std::format("la {}, {}", r, localAddress(p.localIndex)), comment); // LEA form: [sp + Frame.field]
				storeTemp(p.result, r, false, loc);
				break;
			}

			case IrOpcode::GlobalAddr:
			{
				const auto& p = instr.as<IrGlobalAddrPayload>();
				std::string r = intReg(kScratchA);
				_emitter.instr(std::format("la {}, {}", r, mangledName(p.name)), comment); // pseudo form: la rd, symbol
				storeTemp(p.result, r, false, loc);
				break;
			}

			case IrOpcode::Load:
			{
				const auto& p = instr.as<IrLoadPayload>();
				std::string addrReg = intReg(kScratchA);
				loadTemp(p.address, addrReg, false, loc);
				std::string destReg = p.isFloat ? floatReg(kScratchB) : intReg(kScratchB);
				// All Load byte/half/word forms are unsigned in this version (§10/§14 of the
				// architecture plan - a documented v1 simplification, not an oversight): `ldrb`/
				// `ldrh` never `ldrsb`/`ldrsh`. `ldr` auto-dispatches to FLDR for a float destination.
				std::string_view mnemonic = p.isFloat ? "ldr" : (p.size == IrMemSize::Byte ? "ldrb" : p.size == IrMemSize::Half ? "ldrh" : "ldr");
				_emitter.instr(std::format("{} {}, [{}]", mnemonic, destReg, addrReg), comment);
				storeTemp(p.result, destReg, p.isFloat, loc);
				break;
			}

			case IrOpcode::Store:
			{
				const auto& p = instr.as<IrStorePayload>();
				std::string addrReg = intReg(kScratchA);
				loadTemp(p.address, addrReg, false, loc);
				std::string valueReg = p.isFloat ? floatReg(kScratchB) : intReg(kScratchB);
				loadTemp(p.value, valueReg, p.isFloat, loc);
				std::string_view mnemonic = p.isFloat ? "str" : (p.size == IrMemSize::Byte ? "strb" : p.size == IrMemSize::Half ? "strh" : "str");
				_emitter.instr(std::format("{} [{}], {}", mnemonic, addrReg, valueReg), comment);
				break;
			}

			case IrOpcode::Param:
				break; // handled when its Call is reached, below - see generateInstr()'s header comment

			case IrOpcode::Call:
			{
				const auto& p = instr.as<IrCallPayload>();
				if (p.argCount > index)
				{
					_diagnostics.error(loc, "malformed IR: call has more arguments than preceding parameter instructions");
					break;
				}
				u32 argCount = p.argCount;

				std::vector<bool> argIsFloat(argCount);
				std::vector<IrValue> argValues(argCount);
				for (u32 k = 0; k < argCount; ++k)
				{
					const IrInstr& paramInstr = *instrs[index - argCount + k];
					if (paramInstr.opcode() != IrOpcode::Param)
					{
						_diagnostics.error(loc, "malformed IR: call arguments are not contiguous parameter instructions");
						return;
					}
					const auto& param = paramInstr.as<IrParamPayload>();
					argIsFloat[k] = param.isFloat;
					argValues[k] = param.value;
				}

				std::vector<ArgSlot> slots = assignArgSlots(argIsFloat);
				for (u32 k = 0; k < argCount; ++k)
				{
					const ArgSlot& slot = slots[k];
					switch (slot.kind)
					{
						case ArgSlotKind::IntReg:
							loadTemp(argValues[k], intReg(slot.index), false, loc);
							break;
						case ArgSlotKind::FloatReg:
							loadTemp(argValues[k], floatReg(slot.index), true, loc);
							break;
						case ArgSlotKind::Stack:
						{
							std::string r = argIsFloat[k] ? floatReg(kScratchA) : intReg(kScratchA);
							loadTemp(argValues[k], r, argIsFloat[k], loc);
							_emitter.instr(std::format("str [sp + {}], {}", slot.index * 4, r), comment);
							break;
						}
					}
				}

				_emitter.instr(std::format("call {}", mangledName(p.callee)), comment);
				if (p.hasResult)
					storeTemp(p.result, p.isFloat ? floatReg(0) : intReg(0), p.isFloat, loc);
				break;
			}

			case IrOpcode::Jump:
			{
				const auto& p = instr.as<IrJumpPayload>();
				_emitter.instr(std::format("jp .L{}", p.target->id()), comment);
				break;
			}

			case IrOpcode::CondJump:
			{
				// Always plain ints: lowerCondition()'s only two shapes are a 0/1 boolean already
				// materialized by Cmp/materializeBoolean(), or a general "value != 0" truthiness
				// check - neither ever compares float registers directly (ir_builder.cpp).
				const auto& p = instr.as<IrCondJumpPayload>();
				std::string a = intReg(kScratchA);
				std::string b = intReg(kScratchB);
				loadTemp(p.lhs, a, false, loc);
				loadTemp(p.rhs, b, false, loc);
				_emitter.instr(std::format("{} {}, {}, .L{}", ifMnemonic(p.predicate, p.isUnsigned), a, b, p.trueTarget->id()), comment);
				_emitter.instr(std::format("jp .L{}", p.falseTarget->id()), comment);
				break;
			}

			case IrOpcode::Return:
			{
				const auto& p = instr.as<IrReturnPayload>();
				if (p.hasValue)
					loadTemp(p.value, p.isFloat ? floatReg(0) : intReg(0), p.isFloat, loc);

				if (_generatingMain)
				{
					// `main` never returns to a caller - see the header comment on _generatingMain.
					// The return value (if any) is still moved into r0/f0 above so it stays
					// inspectable (e.g. under `ceres debug`) even though nothing outside the VM
					// reads it: `ceres run`'s own process exit code is always 0 on a clean halt,
					// never a program-chosen value (verified against Ceres/libs/driver/src/
					// machine_runner.cpp - there is no register-to-exit-code channel at all).
					_emitter.instr("leave", comment);
					std::string cmdAddr = intReg(kScratchA);
					std::string cmdValue = intReg(kScratchB);
					_emitter.instr(std::format("la {}, 0xFFFF0000", cmdAddr), comment); // SystemControlDevice, 07-IO-Devices-and-Ports.md
					_emitter.instr(std::format("li {}, 1", cmdValue), comment);
					_emitter.instr(std::format("strb [{} + 0], {}", cmdAddr, cmdValue), comment);
					_emitter.instr("halt", comment);
				}
				else
				{
					_emitter.instr("leave", comment);
					_emitter.instr("ret", comment);
				}
				break;
			}
		}
	}

	// ---- functions ------------------------------------------------------------------------------

	void CodeGen::generateFunction(const FunctionDecl& decl, const IrFunction& function)
	{
		FrameLayout layout(function);
		_outgoingSlotCount = layout.outgoingSlotCount();
		std::span<const IrLocalSlot> locals = function.localSlots();
		u32 tempCount = function.tempCount();
		bool hasFields = (_outgoingSlotCount + static_cast<u32>(locals.size()) + tempCount) > 0;
		_frameName = hasFields ? std::format("__frame_{}", decl.name()) : std::string{};

		_emitter.blank();
		_emitter.raw(std::format("// {} - {}", decl.name(), sourceComment(decl.location())));

		if (hasFields)
		{
			_emitter.raw(std::format("struct {}", _frameName));
			for (u32 i = 0; i < _outgoingSlotCount; ++i)
				_emitter.raw(std::format("    outgoing{}: u32", i));
			for (u32 i = 0; i < locals.size(); ++i)
				_emitter.raw(std::format("    {}: {}", localFieldName(i), fieldTypeName(locals[i].sizeInBytes, locals[i].isFloat)));
			// Every temporary is one word regardless of bank (frame_layout.h's own note) - declared
			// as `u32` uniformly rather than tracking each one's float-ness solely to pick a
			// cosmetic field type that lays out identically either way (f32 and u32 are both 4
			// bytes, 4-byte aligned - CeresASM's data_type.h): codegen.cpp never reads a temp
			// field's declared TYPE back, only its computed OFFSET, so nothing depends on this.
			for (u32 i = 0; i < tempCount; ++i)
				_emitter.raw(std::format("    {}: u32", tempFieldName(i)));
			_emitter.raw("endstruct");
		}

		bool isEntryPoint = decl.name() == "main"; // must be `global` - 12-Labels-and-Symbols.md's "The main entry point"
		_generatingMain = isEntryPoint;
		_emitter.label(isEntryPoint ? std::format("global {}", mangledName(decl.name())) : mangledName(decl.name()));

		SourceLocation entryLoc = decl.location();
		_emitter.instr(hasFields ? std::format("enter {}", _frameName) : "enter", sourceComment(entryLoc));

		// Copy every incoming parameter into its own frame field immediately - see codegen.h's own
		// header comment on why every local/parameter gets a durable memory home uniformly, even
		// the first four (which arrive in registers) and the fifth-and-up (which already have a
		// valid address in the CALLER's frame at `[fp + 8]`, `[fp + 12]`, ... but get copied down
		// here too, for one uniform addressing story for the rest of this function's body).
		std::vector<bool> paramIsFloat(function.paramCount());
		for (u32 i = 0; i < function.paramCount(); ++i)
			paramIsFloat[i] = locals[i].isFloat;
		std::vector<ArgSlot> paramSlots = assignArgSlots(paramIsFloat);
		for (u32 i = 0; i < function.paramCount(); ++i)
		{
			const ArgSlot& slot = paramSlots[i];
			std::string destAddr = localAddress(i);
			switch (slot.kind)
			{
				case ArgSlotKind::IntReg:
					_emitter.instr(std::format("{} {}, {}", storeMnemonicFor(locals[i].sizeInBytes, false), destAddr, intReg(slot.index)), sourceComment(entryLoc));
					break;
				case ArgSlotKind::FloatReg:
					_emitter.instr(std::format("{} {}, {}", storeMnemonicFor(locals[i].sizeInBytes, true), destAddr, floatReg(slot.index)), sourceComment(entryLoc));
					break;
				case ArgSlotKind::Stack:
				{
					std::string r = paramIsFloat[i] ? floatReg(kScratchA) : intReg(kScratchA);
					_emitter.instr(std::format("ldr {}, [fp + {}]", r, 8 + slot.index * 4), sourceComment(entryLoc));
					_emitter.instr(std::format("{} {}, {}", storeMnemonicFor(locals[i].sizeInBytes, paramIsFloat[i]), destAddr, r), sourceComment(entryLoc));
					break;
				}
			}
		}

		for (const auto& block : function.blocks())
		{
			_emitter.localLabel(std::format("L{}", block->id()));
			std::span<IrInstr* const> instrs = block->instrs();
			for (usize i = 0; i < instrs.size(); ++i)
				generateInstr(instrs, i);
		}

		_frameName.clear();
		_generatingMain = false;
	}

	// ---- globals and string literals --------------------------------------------------------------

	void CodeGen::generateGlobal(const VarDecl& decl)
	{
		const Type* type = decl.type();
		if (!type)
			return;
		if (type->isArray() || type->isStruct())
		{
			_diagnostics.error(decl.location(), "aggregate global variables of type '{}' are not supported in this version", AstPrinter::typeName(type));
			return;
		}

		std::string_view casmType = fieldTypeName(type->sizeInBytes(), type->isFloat());
		std::string name = mangledName(decl.name());
		if (!decl.initializer())
		{
			_emitter.raw(std::format("let {}: {}", name, casmType));
			return;
		}

		if (type->isFloat())
		{
			std::optional<f32> value = foldGlobalFloat(decl.initializer());
			if (!value)
			{
				_diagnostics.error(decl.location(), "initializer for global '{}' must be a compile-time constant", decl.name());
				return;
			}
			_emitter.raw(std::format("let {}: {} = {}", name, casmType, *value));
		}
		else
		{
			std::optional<i64> value = foldGlobalInt(decl.initializer());
			if (!value)
			{
				_diagnostics.error(decl.location(), "initializer for global '{}' must be a compile-time constant", decl.name());
				return;
			}
			_emitter.raw(std::format("let {}: {} = {}", name, casmType, *value));
		}
	}

	void CodeGen::generateStringLiterals(const IrModule& module)
	{
		for (const IrGlobalString& literal : module.stringLiterals())
		{
			std::string_view value = literal.value.view();
			_emitter.raw(std::format("let {}: u8[{}] = \"{}\"", mangledName(literal.name), value.size() + 1, escapeCasmString(value)));
		}
	}

	// ---- entry point ------------------------------------------------------------------------------

	std::string CodeGen::generate(const TranslationUnit& unit, const IrModule& module)
	{
		std::vector<const VarDecl*> initializedGlobals, uninitializedGlobals;
		for (Decl* decl : unit.decls())
		{
			if (auto* varDecl = dynamic_cast<VarDecl*>(decl))
				(varDecl->initializer() ? initializedGlobals : uninitializedGlobals).push_back(varDecl);
		}

		if (!initializedGlobals.empty())
		{
			_emitter.raw("@data");
			for (const VarDecl* g : initializedGlobals)
				generateGlobal(*g);
			_emitter.blank();
		}
		if (!uninitializedGlobals.empty())
		{
			_emitter.raw("@bss");
			for (const VarDecl* g : uninitializedGlobals)
				generateGlobal(*g);
			_emitter.blank();
		}
		if (!module.stringLiterals().empty())
		{
			_emitter.raw("@rodata");
			generateStringLiterals(module);
			_emitter.blank();
		}

		_emitter.raw("@text");
		for (const auto& function : module.functions())
		{
			const FunctionDecl* decl = findFunctionDecl(unit, function->name());
			if (decl)
				generateFunction(*decl, *function);
			else
				_diagnostics.error({}, "internal error: no declaration found for generated function '{}'", function->name());
		}

		return _emitter.take();
	}
}
