#include <ceresc/codegen/codegen.h>
#include <ceresc/ast/ast_printer.h>
#include <ceresc/sema/type_layout.h>
#include <ceresc/ast/expr.h>
#include <ceresc/ast/stmt.h>

#include <algorithm>
#include <bit>
#include <optional>
#include <vector>

// See codegen.h for where values live (ValuePlacement decides, this file emits) and for the four
// ISA-level peepholes this file applies.
//
// Every mnemonic below is checked against CeresASM's own docs/05-Instruction-Set.md and
// docs/06-Pseudo-Instructions.md - none is invented. Two auto-dispatch rules those pages document
// are used throughout instead of picking an F-prefixed opcode by hand: `add`/`sub`/`ldr`/`str`/
// `mov`/`neg`/`ifXX`/... all choose their float form (FADD/FLDR/FMOV/FNEG/FCMP/...) on their own
// once given a float register operand - the mnemonic text this file writes never changes between
// the int and float form, only which register name (`r4` vs `f4`) it names.

namespace ceresc::codegen
{
	using namespace ast;
	using namespace ir;
	using support::SourceLocation;

	namespace
	{
		// Reserved for shuttling a spilled value in and out while one IR instruction is translated -
		// never handed to a value (value_placement.cpp keeps them out of its pools for exactly this).
		constexpr u32 kScratchA = 4;
		constexpr u32 kScratchB = 5;

		// An immediate is only used where BOTH readings of the 16-bit field agree. The machine is
		// not consistent about it: ADDI/SUBI read it zero-extended (`inst.imm16()`), while
		// CMPI/IMULI/IDIVI read it sign-extended (`inst.simm16()`) - both verified in CeresASM's own
		// Ceres/libs/vm/include/ceres/vm/execution_engine.h, and 05-Instruction-Set.md does not say
		// which is which per opcode. Restricting the peephole to 0..32767 makes the distinction
		// irrelevant instead of depending on a detail this project does not own; a negative constant
		// simply materializes into a register as it always did.
		constexpr i64 kMaxImmediate = 32767;

		std::string intReg(u32 n) { return std::format("r{}", n); }
		std::string floatReg(u32 n) { return std::format("f{}", n); }
		std::string bankReg(u32 n, bool isFloat) { return isFloat ? floatReg(n) : intReg(n); }

		std::string_view loadMnemonicFor(IrMemSize size, bool isFloat, bool isSigned)
		{
			// The signedness comes from the LOADED TYPE, which only the IR knows - `ldrb`/`ldrh`
			// zero-extend and `ldrsb`/`ldrsh` sign-extend (05-Instruction-Set.md), and picking the
			// zero-extending pair unconditionally is what used to make a negative `signed char`
			// read back as a large positive number. A Word load fills the register, so there is
			// nothing to extend; `ldr` also auto-dispatches to FLDR for a float destination.
			if (isFloat) return "ldr";
			if (size == IrMemSize::Byte) return isSigned ? "ldrsb" : "ldrb";
			if (size == IrMemSize::Half) return isSigned ? "ldrsh" : "ldrh";
			return "ldr";
		}

		std::string_view storeMnemonicFor(IrMemSize size, bool isFloat)
		{
			if (isFloat) return "str";
			if (size == IrMemSize::Byte) return "strb";
			if (size == IrMemSize::Half) return "strh";
			return "str";
		}

		std::string_view storeMnemonicForSize(u32 sizeInBytes, bool isFloat)
		{
			if (isFloat) return "str";
			if (sizeInBytes == 1) return "strb";
			if (sizeInBytes == 2) return "strh";
			return "str";
		}

		// A C symbol keeps its own name in the generated CASM. That is what makes interoperability
		// work in both directions without a decoder ring: a routine written in CASM is called from C
		// under the name it was written with, and a C function is called from CASM under the name it
		// was written with. See docs/07-CASM-Interop.md.
		//
		// The cost of that choice is a collision class the assembler has to be protected from: a C
		// identifier is not guaranteed to avoid CASM's own reserved words. Rather than prefixing
		// every symbol to dodge the whole class (which is what this used to do, and which made every
		// interop declaration carry a `cc_` nobody could explain), the exact list is checked and a
		// collision is a Ceres-C diagnostic - see isReservedCasmWord() and checkSymbolNames(). The
		// list is small, and every name in it is either already a C keyword or an implausible
		// identifier, so the error is rare and the message says exactly what to do about it.
		//
		// Still a function, and still called from both the definition and every reference, because
		// of the one name that is NOT the C one: IrModule's synthesized string-literal labels
		// (".str0", ".str1", ... - ir_builder.cpp's visit(StringLiteralExpr&)). A leading `.` means
		// a LOCAL label in CASM (scoped to the nearest preceding global one, 10-Language-Syntax.md),
		// not a file-scope data symbol, and an identifier cannot start with `.` at all - so `.str0`
		// would fail to parse as a `let` name verbatim.
		std::string mangledName(std::string_view name)
		{
			if (name.starts_with(".str"))
				return std::format("__ccstr{}", name.substr(4)); // ".str7" -> "__ccstr7"
			if (!name.empty() && name.front() == '.')
				return std::format("__cc{}", name.substr(1));
			return std::string(name);
		}

		// Every word the CASM lexer refuses to read as an identifier, so a C symbol named one of
		// them can be reported here instead of turning into an assembler syntax error nobody can
		// trace back. Three groups, all verified against the assembler's own tables rather than the
		// prose: KeywordType (common_defs.h), DataType::fromString() (data_type.h), the two boolean
		// literals, and the register names the lexer reserves.
		//
		// Instruction mnemonics are deliberately NOT here: the assembler resolves a label named
		// `add` or `print` perfectly well (a mnemonic is only a mnemonic in instruction position),
		// and forbidding them would rule out a lot of ordinary C names for no reason.
		bool isReservedCasmWord(std::string_view name)
		{
			static constexpr std::string_view kKeywords[] = {
				"let", "const", "global", "import", "macro", "endmacro", "alias",
				"struct", "endstruct", "align", "org", "assert", "interrupt",
			};
			static constexpr std::string_view kDataTypes[] = {
				"u8", "u16", "u32", "i8", "i16", "i32", "f32", "ptr", "char", "bool",
				"string", "port", "irq", "byte", "half", "word",
			};
			static constexpr std::string_view kLiterals[] = { "true", "false" };
			static constexpr std::string_view kRegisterAliases[] = { "sp", "fp", "at", "lr" };

			for (std::string_view reserved : kKeywords)
				if (name == reserved) return true;
			for (std::string_view reserved : kDataTypes)
				if (name == reserved) return true;
			for (std::string_view reserved : kLiterals)
				if (name == reserved) return true;
			for (std::string_view reserved : kRegisterAliases)
				if (name == reserved) return true;

			// rN / fN, the two register banks. Only with a decimal number attached - `r` and `f1x`
			// are ordinary identifiers.
			if (name.size() >= 2 && (name.front() == 'r' || name.front() == 'f'))
			{
				u32 number = 0;
				bool allDigits = true;
				for (char c : name.substr(1))
				{
					allDigits = allDigits && (c >= '0' && c <= '9');
					if (allDigits)
						number = number * 10 + static_cast<u32>(c - '0');
				}
				if (allDigits && number < 16)
					return true;
			}
			return false;
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
			// FloatLiteralExpr keeps its value as f64 until something truncates it (expr.h), and
			// `float` is the only floating type this subset has (§3/§14) - so the narrowing is the
			// intended one, spelled out rather than left implicit for the compiler to warn about.
			if (const auto* lit = dynamic_cast<const FloatLiteralExpr*>(expr))
				return static_cast<f32>(lit->value());
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

	// ---- small helpers ----------------------------------------------------------------------------

	std::string CodeGen::sourceComment(SourceLocation location) const
	{
		if (!location.isValid())
			return {};
		const support::SourceBuffer* buffer = _sourceManager.getBuffer(location.sourceId);
		return std::format("{}:{}", buffer ? buffer->name() : std::string_view("?"), location.line);
	}

	std::string CodeGen::slotFieldName(u32 slotIndex) { return std::format("slot{}", slotIndex); }

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

	std::string CodeGen::slotAddress(u32 slotIndex) const
	{
		return std::format("[sp + {}.{}]", _frameName, slotFieldName(slotIndex));
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

	IrCmpPredicate CodeGen::invertPredicate(IrCmpPredicate predicate)
	{
		switch (predicate)
		{
			case IrCmpPredicate::Eq: return IrCmpPredicate::Ne;
			case IrCmpPredicate::Ne: return IrCmpPredicate::Eq;
			case IrCmpPredicate::Lt: return IrCmpPredicate::Ge;
			case IrCmpPredicate::Le: return IrCmpPredicate::Gt;
			case IrCmpPredicate::Gt: return IrCmpPredicate::Le;
			case IrCmpPredicate::Ge: return IrCmpPredicate::Lt;
		}
		return IrCmpPredicate::Eq;
	}

	std::optional<std::string> CodeGen::localRegister(u32 localIndex) const
	{
		Placement placement = _placement->local(localIndex);
		if (placement.kind != PlacementKind::Register)
			return std::nullopt;
		return bankReg(placement.index, placement.isFloat);
	}

	// ---- operand access ---------------------------------------------------------------------------

	std::string CodeGen::valueIn(IrValue value, u32 scratch, bool isFloat, SourceLocation loc)
	{
		Placement placement = _placement->temp(value);
		if (placement.kind == PlacementKind::Register)
			return bankReg(placement.index, placement.isFloat);

		if (placement.kind == PlacementKind::Virtual)
		{
			// A FrameAddr naming a register-resident local has no address to hand out. Every
			// legitimate reader of one is a Load/Store, which handle it directly (see their cases in
			// generateInstr) - reaching here means the escape analysis and this file disagree.
			_diagnostics.error(loc, "internal error: the address of a register-resident local escaped code generation");
			return bankReg(scratch, isFloat);
		}

		std::string reg = bankReg(scratch, isFloat);
		_emitter.instr(std::format("ldr {}, {}", reg, slotAddress(placement.index)), sourceComment(loc));
		return reg;
	}

	std::string CodeGen::defineInto(IrValue value, u32 scratch, bool isFloat)
	{
		Placement placement = _placement->temp(value);
		if (placement.kind == PlacementKind::Register)
			return bankReg(placement.index, placement.isFloat);
		return bankReg(scratch, isFloat);
	}

	void CodeGen::storeResult(IrValue value, std::string_view reg, SourceLocation loc)
	{
		Placement placement = _placement->temp(value);
		if (placement.kind != PlacementKind::Slot)
			return; // already computed into the register the value lives in
		_emitter.instr(std::format("str {}, {}", slotAddress(placement.index), reg), sourceComment(loc));
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

	std::optional<i64> CodeGen::immediateFor(IrValue value) const
	{
		if (!_options.immediateOperands || !value.isValid() || !_function)
			return std::nullopt;
		auto defs = _defCount.find(value.id);
		if (defs == _defCount.end() || defs->second != 1)
			return std::nullopt;

		for (const auto& block : _function->blocks())
		{
			for (const IrInstr* instr : block->instrs())
			{
				if (instr->opcode() != IrOpcode::Const)
					continue;
				const auto& p = instr->as<IrConstPayload>();
				if (!(p.result == value) || p.isFloat)
					continue;
				if (p.intValue < 0 || p.intValue > kMaxImmediate)
					return std::nullopt; // see kMaxImmediate: outside the range both readings agree on
				return p.intValue;
			}
		}
		return std::nullopt;
	}

	void CodeGen::collectSuppressedConstants(const IrFunction& function)
	{
		_suppressedConsts.assign(function.tempCount(), false);
		if (!_options.immediateOperands)
			return;

		// Start from "every eligible constant is suppressible", then let any reader that cannot take
		// an immediate veto it. A constant with no readers at all stays suppressed, which is right:
		// nothing would read the register either.
		std::vector<bool> candidate(function.tempCount(), false);
		for (const auto& block : function.blocks())
		{
			for (const IrInstr* instr : block->instrs())
			{
				if (instr->opcode() != IrOpcode::Const)
					continue;
				const auto& p = instr->as<IrConstPayload>();
				if (p.result.isValid() && p.result.id < candidate.size() && immediateFor(p.result))
					candidate[p.result.id] = true;
			}
		}

		auto veto = [&](IrValue value)
		{
			if (value.isValid() && value.id < candidate.size())
				candidate[value.id] = false;
		};

		for (const auto& block : function.blocks())
		{
			for (const IrInstr* instr : block->instrs())
			{
				switch (instr->opcode())
				{
					case IrOpcode::BinOp:
					{
						// Only the SECOND operand of an integer operation becomes an immediate:
						// the encoding puts it where `rt` would go (04-Instruction-Format.md), and
						// the first operand has no such field to live in.
						const auto& p = instr->as<IrBinOpPayload>();
						veto(p.lhs);
						if (p.isFloat)
							veto(p.rhs);
						break;
					}
					case IrOpcode::Cmp:
					{
						const auto& p = instr->as<IrCmpPayload>();
						veto(p.lhs);
						if (p.isFloat)
							veto(p.rhs);
						break;
					}
					case IrOpcode::CondJump:
					{
						const auto& p = instr->as<IrCondJumpPayload>();
						veto(p.lhs);
						break;
					}
					default:
						forEachOperand(*instr, veto);
						break;
				}
			}
		}

		_suppressedConsts = std::move(candidate);
	}

	// ---- comparisons ------------------------------------------------------------------------------

	void CodeGen::emitConditionalBranch(IrCmpPredicate predicate, bool isUnsigned, bool isFloat,
		IrValue lhs, IrValue rhs, std::string_view target, SourceLocation loc)
	{
		std::string a = valueIn(lhs, kScratchA, isFloat, loc);
		// A float comparison has no immediate form (FCMP takes two float registers), so the
		// immediate peephole only ever applies to the integer one.
		std::optional<i64> immediate = isFloat ? std::nullopt : immediateFor(rhs);
		std::string b = immediate ? std::format("{}", *immediate) : valueIn(rhs, kScratchB, isFloat, loc);
		_emitter.instr(std::format("{} {}, {}, {}", ifMnemonic(predicate, isUnsigned), a, b, target), sourceComment(loc));
	}

	void CodeGen::materializeCmp(const IrCmpPayload& payload, std::string_view resultReg, SourceLocation loc)
	{
		std::string comment = sourceComment(loc);
		std::string trueLabel = std::format("cmp{}_true", _nextComparisonLabel);
		std::string endLabel = std::format("cmp{}_end", _nextComparisonLabel);
		++_nextComparisonLabel;

		emitConditionalBranch(payload.predicate, payload.isUnsigned, payload.isFloat, payload.lhs, payload.rhs,
			std::format(".{}", trueLabel), loc);
		_emitter.instr(std::format("li {}, 0", resultReg), comment);
		_emitter.instr(std::format("jp .{}", endLabel), comment);
		_emitter.localLabel(trueLabel);
		_emitter.instr(std::format("li {}, 1", resultReg), comment);
		_emitter.localLabel(endLabel);
	}

	bool CodeGen::findFusableCmp(std::span<IrInstr* const> instrs, usize index, const IrCmpPayload*& cmpOut) const
	{
		cmpOut = nullptr;
		if (!_options.cmpBranchFusion || index < 2 || instrs[index]->opcode() != IrOpcode::CondJump)
			return false;

		// The exact shape lowerCondition() emits for a relational condition (ir_builder.cpp): the
		// comparison, a zero constant, then a branch testing one against the other.
		const auto& branch = instrs[index]->as<IrCondJumpPayload>();
		if (branch.predicate != IrCmpPredicate::Ne || branch.isUnsigned)
			return false;

		const IrInstr& zeroInstr = *instrs[index - 1];
		if (zeroInstr.opcode() != IrOpcode::Const)
			return false;
		const auto& zero = zeroInstr.as<IrConstPayload>();
		if (zero.isFloat || zero.intValue != 0 || !(zero.result == branch.rhs))
			return false;

		const IrInstr& cmpInstr = *instrs[index - 2];
		if (cmpInstr.opcode() != IrOpcode::Cmp)
			return false;
		const auto& cmp = cmpInstr.as<IrCmpPayload>();
		if (!(cmp.result == branch.lhs))
			return false;

		// Both intermediate values have to be private to this pair: if anything else reads the
		// comparison's 0/1 result (or that zero), it still has to be materialized.
		auto cmpUses = _useCount.find(cmp.result.id);
		auto zeroUses = _useCount.find(zero.result.id);
		if (cmpUses == _useCount.end() || cmpUses->second != 1)
			return false;
		if (zeroUses == _useCount.end() || zeroUses->second != 1)
			return false;

		cmpOut = &cmp;
		return true;
	}

	// ---- address folding --------------------------------------------------------------------

	bool CodeGen::livesInSlot(IrValue value) const
	{
		return value.isValid() && _placement->temp(value).kind == PlacementKind::Slot;
	}

	bool CodeGen::findFoldableAddress(std::span<IrInstr* const> instrs, usize index, FoldedAddress& out) const
	{
		if (!_options.addressFolding || index == 0)
			return false;

		const IrInstr& access = *instrs[index];
		IrValue addressValue;
		IrValue storedValue;
		bool isStore = false;
		bool storedIsFloat = false;
		if (access.opcode() == IrOpcode::Load)
		{
			addressValue = access.as<IrLoadPayload>().address;
		}
		else if (access.opcode() == IrOpcode::Store)
		{
			const IrStorePayload& store = access.as<IrStorePayload>();
			addressValue = store.address;
			storedValue = store.value;
			storedIsFloat = store.isFloat;
			isStore = true;
		}
		else
		{
			return false;
		}

		if (!addressValue.isValid() || _placement->virtualAddressLocal(addressValue))
			return false; // a register-resident local has no address to fold in the first place

		const IrInstr& previous = *instrs[index - 1];
		if (previous.opcode() != IrOpcode::BinOp)
			return false;
		const IrBinOpPayload& add = previous.as<IrBinOpPayload>();
		// Only `add`: no indexed form SUBTRACTS a register ("Three things are deliberately not
		// allowed", 05-Instruction-Set.md), and a float operand is never an address.
		if (add.op != IrBinOp::Add || add.isFloat || !(add.result == addressValue))
			return false;

		// Defined once and read once - the same pair of conditions findFusableCmp() needs, and for
		// the same reason: anything else means the address outlives this access, so the `add` has to
		// stay.
		auto defs = _defCount.find(addressValue.id);
		auto uses = _useCount.find(addressValue.id);
		if (defs == _defCount.end() || defs->second != 1)
			return false;
		if (uses == _useCount.end() || uses->second != 1)
			return false;

		// The displacement form is only tried for the SECOND operand, and only through
		// immediateFor(): that is exactly the set of constants collectSuppressedConstants() already
		// decided not to materialize (it vetoes a constant used as a BinOp's first operand, since
		// the encoding has no field for one there), so a folded displacement never leaves a dead
		// `li` behind - and no negative value slips through the range both readings of imm16 agree
		// on (kMaxImmediate). A constant FIRST operand (`2 + p`, which IrBuilder lowers with the
		// scaled constant on the left) still folds, just through the indexed form, which costs the
		// same one instruction and needs no special case.
		IrValue base = add.lhs;
		IrValue offset = add.rhs;
		std::optional<i64> displacement = immediateFor(offset);

		// The register budget - see this function's declaration in codegen.h.
		if (isStore && !displacement && !storedIsFloat &&
			livesInSlot(storedValue) && livesInSlot(base) && livesInSlot(offset))
		{
			return false;
		}

		out.base = base;
		out.index = displacement ? IrValue{} : offset;
		out.displacement = displacement;
		return true;
	}

	std::string CodeGen::addressOperand(const FoldedAddress& folded, u32 baseScratch, u32 indexScratch, SourceLocation loc)
	{
		std::string base = valueIn(folded.base, baseScratch, false, loc);
		if (folded.displacement)
		{
			if (*folded.displacement == 0)
				return std::format("[{}]", base);
			return std::format("[{} + {}]", base, *folded.displacement);
		}
		std::string index = valueIn(folded.index, indexScratch, false, loc);
		return std::format("[{} + {}]", base, index); // the assembler picks LDRX/STRX from the operand shapes
	}

	// ---- one IR instruction -------------------------------------------------------------------

	void CodeGen::generateInstr(std::span<IrInstr* const> instrs, usize index, u32 nextBlockId)
	{
		if (index < _skipInstr.size() && _skipInstr[index])
			return; // already consumed by the cmp/branch fusion below

		const IrInstr& instr = *instrs[index];
		SourceLocation loc = instr.location();
		std::string comment = sourceComment(loc);

		switch (instr.opcode())
		{
			case IrOpcode::Const:
			{
				const auto& p = instr.as<IrConstPayload>();
				if (p.result.isValid() && p.result.id < _suppressedConsts.size() && _suppressedConsts[p.result.id])
					break; // every reader folds it in as an immediate - see collectSuppressedConstants()
				if (p.isFloat)
				{
					// No float-immediate load exists (05-Instruction-Set.md/06-Pseudo-Instructions.md):
					// materialize the bit pattern into an int register, then reinterpret it into the
					// float bank with `mtf` (a raw copy, no numeric conversion - exactly what a bit
					// pattern needs).
					u32 bits = std::bit_cast<u32>(p.floatValue);
					emitLoadImmediate(intReg(kScratchA), static_cast<i64>(static_cast<i32>(bits)), loc);
					std::string dest = defineInto(p.result, kScratchB, true);
					_emitter.instr(std::format("mtf {}, {}", dest, intReg(kScratchA)), comment);
					storeResult(p.result, dest, loc);
				}
				else
				{
					std::string dest = defineInto(p.result, kScratchA, false);
					emitLoadImmediate(dest, p.intValue, loc);
					storeResult(p.result, dest, loc);
				}
				break;
			}

			case IrOpcode::BinOp:
			{
				const auto& p = instr.as<IrBinOpPayload>();
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

				std::string a = valueIn(p.lhs, kScratchA, p.isFloat, loc);
				std::optional<i64> immediate = p.isFloat ? std::nullopt : immediateFor(p.rhs);
				std::string b = immediate ? std::format("{}", *immediate) : valueIn(p.rhs, kScratchB, p.isFloat, loc);
				std::string dest = defineInto(p.result, kScratchA, p.isFloat);
				_emitter.instr(std::format("{} {}, {}, {}", mnemonic, dest, a, b), comment);
				storeResult(p.result, dest, loc);
				break;
			}

			case IrOpcode::UnOp:
			{
				const auto& p = instr.as<IrUnOpPayload>();
				switch (p.op)
				{
					case IrUnOp::Neg:
					{
						std::string source = valueIn(p.operand, kScratchA, p.isFloat, loc);
						std::string dest = defineInto(p.result, kScratchA, p.isFloat);
						// The float form uses the `neg` pseudo (06-Pseudo-Instructions.md: it expands
						// to the real FNEG opcode, verified correct). The integer form writes the
						// documented expansion `imul rd, rs, -1` OUT IN FULL instead of `neg rd, rs`,
						// because that pseudo is currently miscompiled by the assembler: `neg r2, r1`
						// assembles to `IMUL r2, r15, r0` (the -1 lands in the rs register field as
						// r15, and the real source register is dropped), so every negation returns
						// garbage. `imul rd, rs, -1` written by hand assembles to the right IMULI and
						// is what the pseudo is documented to mean anyway - see the note in
						// docs/03-IR-to-CASM.md. Worth revisiting once the assembler's expansion is
						// fixed; nothing here depends on keeping the workaround.
						if (p.isFloat)
							_emitter.instr(std::format("neg {}, {}", dest, source), comment); // FNEG
						else
							_emitter.instr(std::format("imul {}, {}, -1", dest, source), comment);
						storeResult(p.result, dest, loc);
						break;
					}
					case IrUnOp::Not:
					{
						std::string source = valueIn(p.operand, kScratchA, false, loc);
						std::string dest = defineInto(p.result, kScratchA, false);
						_emitter.instr(std::format("not {}, {}", dest, source), comment);
						storeResult(p.result, dest, loc);
						break;
					}
					case IrUnOp::LogicalNot:
					{
						// Unreachable from IrBuilder today - visit(UnaryExpr&)'s LogicalNot case goes
						// through materializeBoolean() instead (ir_builder.cpp), never constructing
						// this payload. Handled anyway so this switch stays exhaustive: `!x` is `x == 0`.
						std::string source = valueIn(p.operand, kScratchA, false, loc);
						std::string dest = defineInto(p.result, kScratchA, false);
						std::string trueLabel = std::format("lnot{}_true", _nextComparisonLabel);
						std::string endLabel = std::format("lnot{}_end", _nextComparisonLabel);
						++_nextComparisonLabel;
						_emitter.instr(std::format("ifeq {}, 0, .{}", source, trueLabel), comment);
						_emitter.instr(std::format("li {}, 0", dest), comment);
						_emitter.instr(std::format("jp .{}", endLabel), comment);
						_emitter.localLabel(trueLabel);
						_emitter.instr(std::format("li {}, 1", dest), comment);
						_emitter.localLabel(endLabel);
						storeResult(p.result, dest, loc);
						break;
					}
					case IrUnOp::Narrow:
					{
						// Sign-extending is one instruction either way (`sxtb`/`sxth`,
						// 05-Instruction-Set.md); zero-extending is a mask, and `and`'s immediate is
						// zero-extended to 32 bits, so 0xFFFF reaches the instruction intact rather
						// than sign-extending to all-ones the way a displacement would.
						std::string source = valueIn(p.operand, kScratchA, false, loc);
						std::string dest = defineInto(p.result, kScratchA, false);
						bool isByte = p.narrowSize == IrMemSize::Byte;
						if (p.isUnsigned)
							_emitter.instr(std::format("and {}, {}, {}", dest, source, isByte ? 255 : 65535), comment);
						else
							_emitter.instr(std::format("{} {}, {}", isByte ? "sxtb" : "sxth", dest, source), comment);
						storeResult(p.result, dest, loc);
						break;
					}
					case IrUnOp::ToBool:
					{
						// C's int-to-bool rule is "zero stays zero, anything else becomes one", which
						// unsigned `min` says exactly: min(x, 1) is 0 only when x is 0. One
						// instruction, and no branch - unlike the comparison-as-a-value synthesis the
						// missing `setcc` forces everywhere else (§9).
						std::string source = valueIn(p.operand, kScratchA, false, loc);
						std::string dest = defineInto(p.result, kScratchA, false);
						_emitter.instr(std::format("min {}, {}, 1", dest, source), comment);
						storeResult(p.result, dest, loc);
						break;
					}
					case IrUnOp::IntToFloat:
					{
						std::string source = valueIn(p.operand, kScratchA, false, loc);
						std::string dest = defineInto(p.result, kScratchB, true);
						_emitter.instr(std::format("{} {}, {}", p.isUnsigned ? "itof" : "iitof", dest, source), comment);
						storeResult(p.result, dest, loc);
						break;
					}
					case IrUnOp::FloatToInt:
					{
						std::string source = valueIn(p.operand, kScratchA, true, loc);
						std::string dest = defineInto(p.result, kScratchB, false);
						_emitter.instr(std::format("{} {}, {}", p.isUnsigned ? "ftoi" : "ftoii", dest, source), comment);
						storeResult(p.result, dest, loc);
						break;
					}
				}
				break;
			}

			case IrOpcode::Cmp:
			{
				const auto& p = instr.as<IrCmpPayload>();
				std::string dest = defineInto(p.result, kScratchA, false);
				materializeCmp(p, dest, loc);
				storeResult(p.result, dest, loc);
				break;
			}

			case IrOpcode::Copy:
			{
				const auto& p = instr.as<IrCopyPayload>();
				std::string source = valueIn(p.source, kScratchA, p.isFloat, loc);
				std::string dest = defineInto(p.result, kScratchA, p.isFloat);
				if (dest != source)
					_emitter.instr(std::format("mov {}, {}", dest, source), comment);
				storeResult(p.result, dest, loc);
				break;
			}

			case IrOpcode::FrameAddr:
			{
				const auto& p = instr.as<IrFrameAddrPayload>();
				if (_placement->temp(p.result).kind == PlacementKind::Virtual)
					break; // the local lives in a register - there is no address, and nobody needs one

				Placement local = _placement->local(p.localIndex);
				std::string dest = defineInto(p.result, kScratchA, false);
				_emitter.instr(std::format("la {}, {}", dest, slotAddress(local.index)), comment); // LEA form: [sp + Frame.field]
				storeResult(p.result, dest, loc);
				break;
			}

			case IrOpcode::GlobalAddr:
			{
				const auto& p = instr.as<IrGlobalAddrPayload>();
				std::string dest = defineInto(p.result, kScratchA, false);
				_emitter.instr(std::format("la {}, {}", dest, mangledName(p.name)), comment); // pseudo form: la rd, symbol
				storeResult(p.result, dest, loc);
				break;
			}

			case IrOpcode::Load:
			{
				const auto& p = instr.as<IrLoadPayload>();
				if (std::optional<u32> local = _placement->virtualAddressLocal(p.address))
				{
					// Reading a local that lives in a register is just a register read - there is no
					// memory access to make at all.
					std::optional<std::string> source = localRegister(*local);
					std::string dest = defineInto(p.result, kScratchB, p.isFloat);
					if (source && *source != dest)
						_emitter.instr(std::format("mov {}, {}", dest, *source), comment);
					storeResult(p.result, dest, loc);
					break;
				}

				FoldedAddress folded;
				std::string address = findFoldableAddress(instrs, index, folded)
					? addressOperand(folded, kScratchA, kScratchB, loc)
					: std::format("[{}]", valueIn(p.address, kScratchA, false, loc));
				// The destination may share a scratch register with the address it just read: every
				// load form computes its address before writing rd (05-Instruction-Set.md's LDR/LDRX,
				// and the VM's own handlers), so `ldr r5, [r4 + r5]` is well defined.
				std::string dest = defineInto(p.result, kScratchB, p.isFloat);
				_emitter.instr(std::format("{} {}, {}", loadMnemonicFor(p.size, p.isFloat, p.isSigned), dest, address), comment);
				storeResult(p.result, dest, loc);
				break;
			}

			case IrOpcode::Store:
			{
				const auto& p = instr.as<IrStorePayload>();
				if (std::optional<u32> local = _placement->virtualAddressLocal(p.address))
				{
					std::optional<std::string> dest = localRegister(*local);
					std::string source = valueIn(p.value, kScratchB, p.isFloat, loc);
					if (dest && *dest != source)
						_emitter.instr(std::format("mov {}, {}", *dest, source), comment);
					break;
				}

				FoldedAddress folded;
				std::string address;
				// Unlike a load, a store's value is a third register to READ, so it needs whichever
				// scratch the address did not take - see findFoldableAddress(), which refused the one
				// shape where that leaves nothing.
				u32 valueScratch = kScratchB;
				if (findFoldableAddress(instrs, index, folded))
				{
					address = addressOperand(folded, kScratchA, kScratchB, loc);
					bool indexed = !folded.displacement && folded.index.isValid();
					valueScratch = livesInSlot(folded.base) ? kScratchB : kScratchA;
					if (indexed && livesInSlot(folded.index))
						valueScratch = kScratchA;
				}
				else
				{
					address = std::format("[{}]", valueIn(p.address, kScratchA, false, loc));
				}
				std::string valueReg = valueIn(p.value, valueScratch, p.isFloat, loc);
				_emitter.instr(std::format("{} {}, {}", storeMnemonicFor(p.size, p.isFloat), address, valueReg), comment);
				break;
			}

			case IrOpcode::VaStart:
			{
				const auto& p = instr.as<IrVaStartPayload>();
				// Incoming stack arguments start at [fp + 8] (24-Calling-Convention.md: saved fp at
				// [fp + 0], return address at [fp + 4]). The fixed parameters that were passed on
				// the stack occupy the first words of that area, so the variadic tail begins right
				// after them - and every variadic argument is on the stack by construction
				// (docs/09-Variadic-Convention.md), so from here on it is just consecutive words.
				std::string dest = defineInto(p.result, kScratchA, false);
				_emitter.instr(std::format("la {}, [fp + {}]", dest, 8 + _fixedStackArgWords * 4), comment);
				storeResult(p.result, dest, loc);
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

				// No parallel-move hazard to worry about: the argument registers (r0-r3/f0-f3) are
				// never handed to a value in a function that makes a call at all
				// (value_placement.cpp's allocatable pools), so no source below can be one of the
				// destinations being written here.
				std::vector<ArgSlot> slots = assignArgSlots(argIsFloat,
					fixedArgCountOf(instrs.subspan(index - argCount, argCount)));
				for (u32 k = 0; k < argCount; ++k)
				{
					const ArgSlot& slot = slots[k];
					switch (slot.kind)
					{
						case ArgSlotKind::IntReg:
						case ArgSlotKind::FloatReg:
						{
							bool isFloat = slot.kind == ArgSlotKind::FloatReg;
							std::string dest = bankReg(slot.index, isFloat);
							std::string source = valueIn(argValues[k], isFloat ? kScratchB : kScratchA, isFloat, loc);
							if (source != dest)
								_emitter.instr(std::format("mov {}, {}", dest, source), comment);
							break;
						}
						case ArgSlotKind::Stack:
						{
							std::string source = valueIn(argValues[k], argIsFloat[k] ? kScratchB : kScratchA, argIsFloat[k], loc);
							_emitter.instr(std::format("str [sp + {}], {}", slot.index * 4, source), comment);
							break;
						}
					}
				}

				_emitter.instr(std::format("call {}", mangledName(p.callee)), comment);
				if (p.hasResult)
				{
					std::string returned = bankReg(0, p.isFloat);
					std::string dest = defineInto(p.result, p.isFloat ? kScratchB : kScratchA, p.isFloat);
					if (dest != returned)
						_emitter.instr(std::format("mov {}, {}", dest, returned), comment);
					storeResult(p.result, dest, loc);
				}
				break;
			}

			case IrOpcode::Jump:
			{
				const auto& p = instr.as<IrJumpPayload>();
				if (_options.fallthroughBranches && p.target->id() == nextBlockId)
					break; // the target is the very next block emitted - falling through gets there
				_emitter.instr(std::format("jp .L{}", p.target->id()), comment);
				break;
			}

			case IrOpcode::CondJump:
			{
				const auto& p = instr.as<IrCondJumpPayload>();

				IrCmpPredicate predicate = p.predicate;
				bool isUnsigned = p.isUnsigned;
				bool isFloat = false;
				IrValue lhs = p.lhs;
				IrValue rhs = p.rhs;

				const IrCmpPayload* fused = nullptr;
				if (findFusableCmp(instrs, index, fused))
				{
					// `cmp != 0` is just the comparison itself - branch on its own operands and
					// predicate, skipping the 0/1 value entirely.
					predicate = fused->predicate;
					isUnsigned = fused->isUnsigned;
					isFloat = fused->isFloat;
					lhs = fused->lhs;
					rhs = fused->rhs;
				}

				bool trueIsNext = _options.fallthroughBranches && p.trueTarget->id() == nextBlockId;
				bool falseIsNext = _options.fallthroughBranches && p.falseTarget->id() == nextBlockId;

				if (trueIsNext && !falseIsNext && !isFloat)
				{
					// Invert the test so the single branch goes to the false target and the taken
					// path falls through.
					emitConditionalBranch(invertPredicate(predicate), isUnsigned, isFloat, lhs, rhs,
						std::format(".L{}", p.falseTarget->id()), loc);
					break;
				}

				emitConditionalBranch(predicate, isUnsigned, isFloat, lhs, rhs,
					std::format(".L{}", p.trueTarget->id()), loc);
				if (!falseIsNext)
					_emitter.instr(std::format("jp .L{}", p.falseTarget->id()), comment);
				break;
			}

			case IrOpcode::Return:
			{
				const auto& p = instr.as<IrReturnPayload>();
				if (p.hasValue)
				{
					std::string returnReg = bankReg(0, p.isFloat);
					std::string source = valueIn(p.value, p.isFloat ? kScratchB : kScratchA, p.isFloat, loc);
					if (source != returnReg)
						_emitter.instr(std::format("mov {}, {}", returnReg, source), comment);
				}

				if (_generatingMain)
				{
					// `main` never returns to a caller - see the header comment on _generatingMain.
					// The return value (if any) is still moved into r0/f0 above so it stays
					// inspectable (e.g. under `ceres debug`) even though nothing outside the VM
					// reads it: `ceres run`'s own process exit code is always 0 on a clean halt,
					// never a program-chosen value (verified against Ceres/libs/driver/src/
					// machine_runner.cpp - there is no register-to-exit-code channel at all).
					if (_hasFrame)
						_emitter.instr("leave", comment);
					_emitter.instr(std::format("la {}, 0xFFFF0000", intReg(kScratchA)), comment); // SystemControlDevice, 07-IO-Devices-and-Ports.md
					_emitter.instr(std::format("li {}, 1", intReg(kScratchB)), comment);
					_emitter.instr(std::format("strb [{} + 0], {}", intReg(kScratchA), intReg(kScratchB)), comment);
					_emitter.instr("halt", comment);
				}
				else
				{
					if (_hasFrame)
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
		_function = &function;
		_placement.emplace(function, _options);
		const ValuePlacement& placement = *_placement;

		_useCount.clear();
		_defCount.clear();
		for (const auto& block : function.blocks())
		{
			for (const IrInstr* instr : block->instrs())
			{
				forEachOperand(*instr, [&](IrValue value) { if (value.isValid()) ++_useCount[value.id]; });
				IrValue result = resultOf(*instr);
				if (result.isValid())
					++_defCount[result.id];
			}
		}

		collectSuppressedConstants(function);

		_hasFrame = placement.needsFrame();
		bool hasFields = placement.outgoingSlotCount() > 0 || !placement.slots().empty();
		_frameName = hasFields ? std::format("__frame_{}", decl.name()) : std::string{};

		_emitter.blank();
		_emitter.raw(std::format("// {} - {}", decl.name(), sourceComment(decl.location())));

		if (hasFields)
		{
			_emitter.raw(std::format("struct {}", _frameName));
			for (u32 i = 0; i < placement.outgoingSlotCount(); ++i)
				_emitter.raw(std::format("    outgoing{}: u32", i));
			for (u32 i = 0; i < placement.slots().size(); ++i)
			{
				const FrameSlotInfo& slot = placement.slots()[i];
				_emitter.raw(std::format("    {}: {}", slotFieldName(i), fieldTypeName(slot.sizeInBytes, slot.isFloat)));
			}
			_emitter.raw("endstruct");
		}

		bool isEntryPoint = decl.name() == "main"; // must be `global` - 12-Labels-and-Symbols.md's "The main entry point"
		_generatingMain = isEntryPoint;
		// `global` is what publishes a symbol to the linker (12-Labels-and-Symbols.md), so it is
		// exactly C's external linkage: everything except a `static` function. `main` gets it
		// regardless - the linker looks that name up to find the entry point.
		bool exported = isEntryPoint || decl.hasExternalLinkage();
		_emitter.label(exported ? std::format("global {}", mangledName(decl.name())) : mangledName(decl.name()));

		SourceLocation entryLoc = decl.location();
		if (_hasFrame)
			_emitter.instr(hasFields ? std::format("enter {}", _frameName) : "enter", sourceComment(entryLoc));

		// Settle every parameter into wherever it lives for the rest of the function: nothing at all
		// when it already arrived in the register it was assigned, a move when it was assigned a
		// different one, a store when it lives in a frame field. A parameter that arrived on the
		// stack is read from the CALLER's frame at [fp + 8], [fp + 12], ... (24-Calling-Convention.md).
		std::span<const ArgSlot> arrivals = placement.paramArrival();
		_fixedStackArgWords = 0;
		for (const ArgSlot& arrival : arrivals)
			if (arrival.kind == ArgSlotKind::Stack)
				++_fixedStackArgWords;

		for (u32 i = 0; i < function.paramCount(); ++i)
		{
			const ArgSlot& arrival = arrivals[i];
			const IrLocalSlot& slot = function.localSlots()[i];
			Placement home = placement.local(i);
			std::string comment = sourceComment(entryLoc);

			if (home.kind == PlacementKind::None)
				continue; // nothing in the body reads this parameter - it can stay where it landed

			if (arrival.kind == ArgSlotKind::Stack)
			{
				std::string scratch = bankReg(kScratchA, slot.isFloat);
				std::string target = home.kind == PlacementKind::Register ? bankReg(home.index, home.isFloat) : scratch;
				_emitter.instr(std::format("ldr {}, [fp + {}]", target, 8 + arrival.index * 4), comment);
				if (home.kind == PlacementKind::Slot)
					_emitter.instr(std::format("{} {}, {}", storeMnemonicForSize(slot.sizeInBytes, slot.isFloat),
						slotAddress(home.index), target), comment);
				continue;
			}

			std::string arrived = bankReg(arrival.index, arrival.kind == ArgSlotKind::FloatReg);
			if (home.kind == PlacementKind::Register)
			{
				std::string target = bankReg(home.index, home.isFloat);
				if (target != arrived)
					_emitter.instr(std::format("mov {}, {}", target, arrived), comment);
				continue;
			}
			_emitter.instr(std::format("{} {}, {}", storeMnemonicForSize(slot.sizeInBytes, slot.isFloat),
				slotAddress(home.index), arrived), comment);
		}

		std::span<const std::unique_ptr<BasicBlock>> blocks = function.blocks();
		for (usize b = 0; b < blocks.size(); ++b)
		{
			_emitter.localLabel(std::format("L{}", blocks[b]->id()));
			std::span<IrInstr* const> instrs = blocks[b]->instrs();
			u32 nextBlockId = (b + 1 < blocks.size()) ? blocks[b + 1]->id() : ~0u;

			// Mark, up front, the instructions a later fusion will consume: the comparison and the
			// zero it is tested against both disappear into the branch that reads them.
			_skipInstr.assign(instrs.size(), false);
			for (usize i = 0; i < instrs.size(); ++i)
			{
				const IrCmpPayload* fused = nullptr;
				if (findFusableCmp(instrs, i, fused))
				{
					_skipInstr[i - 1] = true;
					_skipInstr[i - 2] = true;
				}
				FoldedAddress folded;
				if (findFoldableAddress(instrs, i, folded))
					_skipInstr[i - 1] = true; // the `add` disappears into the access's own operand
			}

			for (usize i = 0; i < instrs.size(); ++i)
				generateInstr(instrs, i, nextBlockId);
		}

		_skipInstr.clear();
		_suppressedConsts.clear();
		_frameName.clear();
		_generatingMain = false;
		_placement.reset();
		_function = nullptr;
	}

	// ---- globals and string literals --------------------------------------------------------------

	std::string CodeGen::scalarArrayTypeName(const Type* type)
	{
		// `int m[2][3]` is Array(Array(int, 3), 2) (libs/ast's own outermost-first construction), and
		// CASM spells the same shape `u32[2][3]` (11-Data-Types-and-Literals.md), so the dimensions
		// come out in exactly the order they are unwrapped.
		std::vector<u32> dimensions;
		const Type* element = type;
		while (element && element->isArray())
		{
			dimensions.push_back(element->arraySize());
			element = element->arrayElementType();
		}
		if (!element || element->isAggregate() || dimensions.empty())
			return {}; // not this shape - the caller falls back to a flat word array

		std::string name = fieldTypeName(element->sizeInBytes(), element->isFloat());
		for (u32 dimension : dimensions)
			name += std::format("[{}]", dimension);
		return name;
	}

	std::optional<std::string> CodeGen::scalarArrayInitText(const Type* type, const Expr* init) const
	{
		if (!type || !init)
			return std::nullopt;

		// A string literal fills a char array as bytes, terminating zero and all - CASM's own
		// `let greeting: u8[16] = "Hello, CeresVM!"` (11-Data-Types-and-Literals.md), which pads the
		// rest with zeros exactly like a short value list does.
		if (const auto* literal = dynamic_cast<const StringLiteralExpr*>(init))
		{
			if (type->isArray() && type->arrayElementType() && type->arrayElementType()->sizeInBytes() == 1)
				return std::format("\"{}\"", escapeCasmString(literal->value().view()));

			// CASM documents string syntax only for the complete character array. Nested character
			// arrays use an explicit byte list, whose short-list zero fill supplies the terminator.
			std::string text = "[";
			for (usize i = 0; i < literal->value().view().size(); ++i)
			{
				if (i != 0)
					text += ", ";
				text += std::format("{}", static_cast<u8>(literal->value().view()[i]));
			}
			return text + "]";
		}

		if (const auto* list = dynamic_cast<const InitListExpr*>(init))
		{
			if (!type->isArray())
			{
				if (list->elements().empty())
					return std::optional<std::string>("0");
				return scalarArrayInitText(type, list->elements().front());
			}
			const Type* element = type->arrayElementType();
			std::string text = "[";
			bool first = true;
			for (const Expr* value : list->elements())
			{
				std::optional<std::string> part = scalarArrayInitText(element, value);
				if (!part)
					return std::nullopt;
				if (!first)
					text += ", ";
				text += *part;
				first = false;
			}
			// A short list is fine: CASM zero-fills the remaining elements, the same rule C has.
			return text + "]";
		}

		if (type->isFloat())
		{
			std::optional<f32> value = foldGlobalFloat(init);
			return value ? std::optional<std::string>(std::format("{}", *value)) : std::nullopt;
		}
		std::optional<i64> value = foldGlobalInt(init);
		return value ? std::optional<std::string>(std::format("{}", *value)) : std::nullopt;
	}

	bool CodeGen::buildGlobalImage(const Type* type, const Expr* init, u32 offset, std::vector<u8>& image) const
	{
		if (!type)
			return false;
		if (!init)
			return true; // nothing to write - the image is already zero, which is what C promises

		u32 size = type->sizeInBytes();
		if (offset + size > image.size())
			return false; // sema already reported the overflow

		if (const auto* literal = dynamic_cast<const StringLiteralExpr*>(init))
		{
			std::string_view text = literal->value().view();
			for (usize i = 0; i < text.size() && i < size; ++i)
				image[offset + i] = static_cast<u8>(text[i]);
			return true; // the terminating zero and any padding are already zero
		}

		if (const auto* list = dynamic_cast<const InitListExpr*>(init))
		{
			if (type->isArray())
			{
				const Type* element = type->arrayElementType();
				u32 elementSize = element ? element->sizeInBytes() : 1u;
				u32 index = 0;
				for (const Expr* value : list->elements())
				{
					if (index >= type->arraySize())
						break;
					if (!buildGlobalImage(element, value, offset + index * elementSize, image))
						return false;
					++index;
				}
				return true;
			}
			if (type->isAggregate())
			{
				StructDecl* structDecl = type->structDecl();
				if (!structDecl)
					return false;
				std::span<const FieldDecl> fields = structDecl->fields();
				usize index = 0;
				for (const Expr* value : list->elements())
				{
					if (index >= fields.size())
						break;
					if (!buildGlobalImage(fields[index].type, value, offset + sema::fieldOffset(*structDecl, static_cast<u32>(index)), image))
						return false;
					++index;
				}
				return true;
			}
			// A scalar with braces - `int x = { 5 }`; sema already required exactly one value.
			return list->elements().empty() || buildGlobalImage(type, list->elements().front(), offset, image);
		}

		// One scalar, written little-endian (02-Memory.md) in its own declared width.
		u64 bits = 0;
		if (type->isFloat())
		{
			std::optional<f32> value = foldGlobalFloat(init);
			if (!value)
				return false;
			bits = std::bit_cast<u32>(*value);
		}
		else
		{
			std::optional<i64> value = foldGlobalInt(init);
			if (!value)
				return false;
			bits = static_cast<u64>(*value);
		}
		for (u32 i = 0; i < size && i < 8; ++i)
			image[offset + i] = static_cast<u8>((bits >> (8 * i)) & 0xFF);
		return true;
	}

	void CodeGen::generateAggregateGlobal(const VarDecl& decl, std::string_view symbolName, bool exported)
	{
		const Type* type = decl.type();
		// `global let NAME` - the keyword comes first, before `let`, and is what publishes the
		// symbol to the linker (12-Labels-and-Symbols.md).
		std::string let = exported ? "global let" : "let";
		std::string name{ symbolName };
		std::string typeComment = std::format("{} ({} bytes)", AstPrinter::typeName(type), type->sizeInBytes());

		// An array of scalars keeps its real shape - both because it reads better and because the
		// assembler then gives it the element type's own alignment.
		if (std::string arrayType = scalarArrayTypeName(type); !arrayType.empty())
		{
			if (!decl.initializer())
			{
				_emitter.raw(std::format("{} {}: {}", let, name, arrayType));
				return;
			}
			std::optional<std::string> values = scalarArrayInitText(type, decl.initializer());
			if (!values)
			{
				_diagnostics.error(decl.location(), "initializer for global '{}' must be a compile-time constant", decl.name());
				return;
			}
			_emitter.raw(std::format("{} {}: {} = {}", let, name, arrayType, *values));
			return;
		}

		// Anything involving a struct: one flat word array of the right byte count, four-byte
		// aligned - see generateAggregateGlobal()'s declaration in codegen.h for why not a CASM
		// `struct`. Every field offset is already a constant in the IR, so the only thing the
		// spelling loses is the field names, which the comment puts back.
		u32 words = (type->sizeInBytes() + 3) / 4;
		if (!decl.initializer())
		{
			_emitter.raw(std::format("{} {}: u32[{}]   // {}", let, name, words, typeComment));
			return;
		}

		std::vector<u8> image(static_cast<usize>(words) * 4, 0);
		if (!buildGlobalImage(type, decl.initializer(), 0, image))
		{
			_diagnostics.error(decl.location(), "initializer for global '{}' must be a compile-time constant", decl.name());
			return;
		}

		std::string values;
		for (u32 w = 0; w < words; ++w)
		{
			u32 value = static_cast<u32>(image[w * 4]) | (static_cast<u32>(image[w * 4 + 1]) << 8) |
				(static_cast<u32>(image[w * 4 + 2]) << 16) | (static_cast<u32>(image[w * 4 + 3]) << 24);
			if (w != 0)
				values += ", ";
			values += std::format("0x{:08X}", value);
		}
		_emitter.raw(std::format("{} {}: u32[{}] = [{}]   // {}", let, name, words, values, typeComment));
	}

	void CodeGen::generateGlobal(const VarDecl& decl, std::string_view symbolName, bool exported)
	{
		const Type* type = decl.type();
		if (!type)
			return;
		if (type->isArray() || type->isAggregate())
		{
			generateAggregateGlobal(decl, symbolName, exported);
			return;
		}

		std::string casmType = fieldTypeName(type->sizeInBytes(), type->isFloat());
		// `global let NAME` - the keyword comes first, before `let`, and is what publishes the
		// symbol to the linker (12-Labels-and-Symbols.md).
		std::string let = exported ? "global let" : "let";
		std::string name{ symbolName };
		if (!decl.initializer())
		{
			_emitter.raw(std::format("{} {}: {}", let, name, casmType));
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
			_emitter.raw(std::format("{} {}: {} = {}", let, name, casmType, *value));
		}
		else
		{
			std::optional<i64> value = foldGlobalInt(decl.initializer());
			if (!value)
			{
				_diagnostics.error(decl.location(), "initializer for global '{}' must be a compile-time constant", decl.name());
				return;
			}
			_emitter.raw(std::format("{} {}: {} = {}", let, name, casmType, *value));
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

	void CodeGen::checkSymbolNames(const TranslationUnit& unit, const IrModule& module)
	{
		auto check = [&](std::string_view name, support::SourceLocation location, std::string_view what)
		{
			if (!isReservedCasmWord(name))
				return;
			_diagnostics.error(location,
				"'{}' cannot be used as the name of a {}: it is a reserved word in CeresASM, and a C symbol "
				"keeps its own name in the generated assembly (see docs/07-CASM-Interop.md). Rename it.",
				name, what);
		};

		for (Decl* decl : unit.decls())
		{
			if (auto* function = dynamic_cast<FunctionDecl*>(decl))
				check(function->name(), function->location(), "function");
			else if (auto* variable = dynamic_cast<VarDecl*>(decl))
				check(variable->name(), variable->location(), "global variable");
		}
		// A `static` local becomes a file-scope CASM symbol too, under a name that already carries
		// its function's - so it cannot collide by accident, only by the user's own choice of the
		// part that comes from C.
		for (const IrStaticLocal& local : module.staticLocals())
			check(local.name, local.decl->location(), "static local variable");
	}

	std::vector<ExternalDeclaration> CodeGen::collectExternalDeclarations(const TranslationUnit& unit) const
	{
		std::vector<ExternalDeclaration> declarations;
		for (Decl* decl : unit.decls())
		{
			if (auto* function = dynamic_cast<FunctionDecl*>(decl))
			{
				// Prototypes count as much as definitions: a unit that only declares `int f(int);`
				// still needs every OTHER unit to agree on the name, and whichever one defines it
				// will contribute the same entry. The driver keeps one copy.
				if (!function->hasExternalLinkage())
					continue;
				declarations.push_back(ExternalDeclaration{ std::string(function->name()), true, function->isDefinition(), false, "@text", {} });
				continue;
			}

			auto* variable = dynamic_cast<VarDecl*>(decl);
			if (!variable || variable->storageClass() == ast::StorageClass::Static)
				continue;
			const Type* type = variable->type();
			if (!type)
				continue;

			std::string section = type->isConst() ? "@rodata" : "@bss";
			if (variable->initializer())
				section = type->isConst() ? "@rodata" : "@data";

			// The declared SHAPE has to match the definition's, since that is the whole reason the
			// declaration exists - so it is built by the same two functions that build the real one.
			std::string typeText;
			if (type->isArray() || type->isAggregate())
			{
				typeText = scalarArrayTypeName(type);
				if (typeText.empty())
					typeText = std::format("u32[{}]", (type->sizeInBytes() + 3) / 4);
			}
			else
			{
				typeText = fieldTypeName(type->sizeInBytes(), type->isFloat());
			}
			declarations.push_back(ExternalDeclaration{ std::string(variable->name()), false, !variable->isExternDeclaration(),
				variable->initializer() != nullptr, std::move(section), std::move(typeText) });
		}
		return declarations;
	}

	std::string CodeGen::generate(const TranslationUnit& unit, const IrModule& module)
	{
		checkSymbolNames(unit, module);

		// Four buckets, not two. `const` with an initializer goes to @rodata, where the machine
		// itself enforces the qualifier - a store into it raises MemoryFault instead of quietly
		// working (02-Memory.md). An `extern` declaration with no initializer defines nothing at
		// all: the storage belongs to whatever unit or CASM file does define it, and emitting a
		// second copy here would be a duplicate-symbol error at link time, or worse, two variables.
		//
		// One definition per NAME, too. A name may legally be declared several times at file scope -
		// an `extern` in a header plus the definition in the source that includes it is the ordinary
		// case, and two bare `int x;` tentative definitions are legal C as well - but it names one
		// object, and emitting a second `let` for it would be a duplicate symbol. The declaration
		// with an initializer wins; failing that, the first one seen.
		std::vector<const VarDecl*> definitions;
		std::unordered_map<std::string_view, usize> definitionIndex;
		for (Decl* decl : unit.decls())
		{
			auto* varDecl = dynamic_cast<VarDecl*>(decl);
			if (!varDecl || varDecl->isExternDeclaration())
				continue;
			auto [it, inserted] = definitionIndex.try_emplace(varDecl->name(), definitions.size());
			if (inserted)
				definitions.push_back(varDecl);
			else if (varDecl->initializer())
				definitions[it->second] = varDecl;
		}

		std::vector<const VarDecl*> dataGlobals, bssGlobals, rodataGlobals;
		for (const VarDecl* varDecl : definitions)
		{
			if (!varDecl->initializer())
				bssGlobals.push_back(varDecl);
			else if (varDecl->type() && varDecl->type()->isConst())
				rodataGlobals.push_back(varDecl);
			else
				dataGlobals.push_back(varDecl);
		}
		// A `static` local is an ordinary file-scope variable that happens to be spelled inside a
		// function - same three buckets, never `global`. Its CASM name is not its C name, so the
		// two are kept side by side for the emission loop below.
		std::unordered_map<const VarDecl*, std::string_view> staticLocalNames;
		for (const IrStaticLocal& local : module.staticLocals())
			staticLocalNames.emplace(local.decl, local.name);
		for (const IrStaticLocal& local : module.staticLocals())
		{
			if (!local.decl->initializer())
				bssGlobals.push_back(local.decl);
			else if (local.decl->type() && local.decl->type()->isConst())
				rodataGlobals.push_back(local.decl);
			else
				dataGlobals.push_back(local.decl);
		}

		auto emitBucket = [&](std::string_view section, const std::vector<const VarDecl*>& bucket)
		{
			if (bucket.empty())
				return;
			_emitter.raw(section);
			for (const VarDecl* g : bucket)
			{
				auto it = staticLocalNames.find(g);
				bool isStaticLocal = it != staticLocalNames.end();
				std::string_view symbolName = isStaticLocal ? it->second : g->name();
				// A `static` of either kind has internal linkage and is not published.
				bool exported = !isStaticLocal && g->storageClass() != ast::StorageClass::Static;
				generateGlobal(*g, symbolName, exported);
			}
			_emitter.blank();
		};

		emitBucket("@data", dataGlobals);
		emitBucket("@bss", bssGlobals);
		if (!rodataGlobals.empty() || !module.stringLiterals().empty())
		{
			_emitter.raw("@rodata");
			for (const VarDecl* g : rodataGlobals)
			{
				auto it = staticLocalNames.find(g);
				bool isStaticLocal = it != staticLocalNames.end();
				generateGlobal(*g, isStaticLocal ? it->second : g->name(),
					!isStaticLocal && g->storageClass() != ast::StorageClass::Static);
			}
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
