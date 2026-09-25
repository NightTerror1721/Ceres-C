#include <ceresc/codegen/codegen.h>
#include <ceresc/ast/ast_printer.h>
#include <ceresc/sema/type_layout.h>
#include <ceresc/ast/expr.h>
#include <ceresc/ast/stmt.h>

#include <algorithm>
#include <bit>
#include <optional>
#include <unordered_set>
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
	// Shorthand for the ids these messages are classified by - every error() and warning()
	// call below names one. See support/diagnostic_id.h.
	using DiagId = support::DiagnosticId;

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

		// `dest = (T)source` for an integer T narrower than a word, in one instruction - the
		// same two forms IrUnOp::Narrow emits, and for the same reason: sign-extending is
		// `sxtb`/`sxth`, zero-extending is a mask whose immediate `and` takes unsigned, so
		// 0xFFFF arrives intact.
		std::string narrowToWidth(std::string_view dest, std::string_view source, u32 sizeInBytes, bool isSigned)
		{
			if (isSigned)
				return std::format("{} {}, {}", sizeInBytes == 1 ? "sxtb" : "sxth", dest, source);
			return std::format("and {}, {}, {}", dest, source, sizeInBytes == 1 ? 255 : 65535);
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

		// The labels the unit's declarations were given with `__asm__("...")`, by C name. Any one declaration of a name
		// is enough: sema has already checked that they agree.
		using AsmLabelMap = std::unordered_map<std::string_view, std::string>;

		AsmLabelMap asmLabelsOf(const TranslationUnit& unit)
		{
			AsmLabelMap labels;
			for (Decl* decl : unit.decls())
				if (decl && decl->asmLabel())
					labels.emplace(decl->name(), std::string(decl->asmLabel().view()));
			return labels;
		}

		// What a C symbol is called in the CASM: its explicit label, else a `__c_` prefix when the name is a word
		// CeresASM reserves (`at`, `half`, `global`, `r5`...), else the name as it is.
		std::string casmSymbolName(const AsmLabelMap& labels, std::string_view name)
		{
			if (const auto it = labels.find(name); it != labels.end())
				return it->second;
			if (isReservedCasmWord(name))
				return std::format("__c_{}", name);
			return mangledName(name);
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
			// What sema already folded: `2 + 3`, `sizeof(a) / sizeof(a[0])`, an enumerator (Expr::constantValue).
			if (expr && expr->constantValue())
				return expr->constantValue();
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
			// `(void*)0`, which is what NULL expands to, and `(char)65`: a cast does not change the
			// bits a constant is written as - the image is written at the declared width anyway.
			if (const auto* cast = dynamic_cast<const CastExpr*>(expr))
				return foldGlobalInt(cast->operand());
			return std::nullopt;
		}

		std::optional<f32> foldGlobalFloat(const Expr* expr)
		{
			if (expr && expr->constantValue())
				return static_cast<f32>(*expr->constantValue());   // `float f = 2 * 3;`: an integer constant, converted
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
			if (const auto* cast = dynamic_cast<const CastExpr*>(expr))       // `(float)3`
				return foldGlobalFloat(cast->operand());
			return std::nullopt;
		}

		// An integer type: its constants fold by C's integer rules, not IEEE's.
		bool isIntegerType(const Type* type) noexcept
		{
			return type && !type->isFloating() && !type->isPointer() && !type->isArray() && !type->isAggregate() &&
				!type->isFunction() && !type->isVoid();
		}

		// A value converted to an integer type: wrapped to its width and signedness (a bool is 0 or 1).
		i64 wrapToInteger(i64 value, const Type* type) noexcept
		{
			if (type->isBool())
				return value != 0 ? 1 : 0;
			const u32 bits = type->sizeInBytes() * 8;
			if (bits == 0 || bits >= 64)
				return value;
			const u64 mask = (u64(1) << bits) - 1;
			u64 low = static_cast<u64>(value) & mask;
			if (type->isSigned() && (low >> (bits - 1)) != 0)
				low |= ~mask;
			return static_cast<i64>(low);
		}

		// An integer-typed piece of a double initializer - the `3/2` of `1.0 + 3/2`, the `(char)300` - folded the way C
		// evaluates it: integer division, wrapping at the type's width. Sema folds only whole initializers, so the pieces
		// of a floating one come here unfolded. Anything this does not model is not folded at all.
		std::optional<f64> foldGlobalDouble(const Expr* expr);

		std::optional<i64> foldGlobalInteger(const Expr* expr)
		{
			const Type* type = expr ? expr->type() : nullptr;
			if (!isIntegerType(type))
				return std::nullopt;
			if (const auto* lit = dynamic_cast<const IntLiteralExpr*>(expr))
				return wrapToInteger(static_cast<i64>(lit->value()), type);
			if (const auto* cast = dynamic_cast<const CastExpr*>(expr))
			{
				const Type* from = cast->operand() ? cast->operand()->type() : nullptr;
				if (from && from->isFloating())
				{
					// Out of range is undefined in C; refusing is the honest answer.
					std::optional<f64> value = foldGlobalDouble(cast->operand());
					if (!value || !(*value > -9223372036854775808.0 && *value < 9223372036854775808.0))
						return std::nullopt;
					return wrapToInteger(static_cast<i64>(*value), type);
				}
				std::optional<i64> value = foldGlobalInteger(cast->operand());
				return value ? std::optional<i64>(wrapToInteger(*value, type)) : std::nullopt;
			}
			if (const auto* unary = dynamic_cast<const UnaryExpr*>(expr);
				unary && (unary->op() == UnaryOp::Negate || unary->op() == UnaryOp::BitwiseNot))
			{
				std::optional<i64> operand = foldGlobalInteger(unary->operand());
				if (!operand)
					return std::nullopt;
				const u64 bits = static_cast<u64>(*operand);
				return wrapToInteger(static_cast<i64>(unary->op() == UnaryOp::Negate ? u64(0) - bits : ~bits), type);
			}
			if (const auto* binary = dynamic_cast<const BinaryExpr*>(expr))
			{
				const BinaryOp op = binary->op();
				const bool shift = op == BinaryOp::Shl || op == BinaryOp::Shr;
				const bool arithmetic = op == BinaryOp::Add || op == BinaryOp::Sub || op == BinaryOp::Mul || op == BinaryOp::Div ||
					op == BinaryOp::Mod || op == BinaryOp::BitAnd || op == BinaryOp::BitOr || op == BinaryOp::BitXor;
				std::optional<i64> lhs = (shift || arithmetic) ? foldGlobalInteger(binary->lhs()) : std::nullopt;
				std::optional<i64> rhs = (shift || arithmetic) ? foldGlobalInteger(binary->rhs()) : std::nullopt;
				if (!lhs || !rhs)
					return expr->constantValue() ? std::optional<i64>(wrapToInteger(*expr->constantValue(), type)) : std::nullopt;
				// Both operands in the result's type (the usual arithmetic conversions); a shift converts only its left.
				const i64 a = wrapToInteger(*lhs, type);
				const i64 b = shift ? *rhs : wrapToInteger(*rhs, type);
				const u64 ua = static_cast<u64>(a);
				const u64 ub = static_cast<u64>(b);
				const bool isSigned = type->isSigned();
				const u32 width = type->sizeInBytes() * 8;
				switch (op)
				{
					case BinaryOp::Add: return wrapToInteger(static_cast<i64>(ua + ub), type);
					case BinaryOp::Sub: return wrapToInteger(static_cast<i64>(ua - ub), type);
					case BinaryOp::Mul: return wrapToInteger(static_cast<i64>(ua * ub), type);
					case BinaryOp::BitAnd: return wrapToInteger(static_cast<i64>(ua & ub), type);
					case BinaryOp::BitOr: return wrapToInteger(static_cast<i64>(ua | ub), type);
					case BinaryOp::BitXor: return wrapToInteger(static_cast<i64>(ua ^ ub), type);
					case BinaryOp::Div:
					case BinaryOp::Mod:
						if (b == 0 || (isSigned && a == INT64_MIN && b == -1))
							return std::nullopt;   // undefined, or the one quotient i64 cannot hold
						if (isSigned)
							return wrapToInteger(op == BinaryOp::Div ? a / b : a % b, type);
						return wrapToInteger(static_cast<i64>(op == BinaryOp::Div ? ua / ub : ua % ub), type);
					case BinaryOp::Shl:
					case BinaryOp::Shr:
						if (b < 0 || b >= static_cast<i64>(width))
							return std::nullopt;
						if (op == BinaryOp::Shl)
							return wrapToInteger(static_cast<i64>(ua << b), type);
						return wrapToInteger(isSigned ? a >> b : static_cast<i64>(ua >> b), type);
					default: return std::nullopt;
				}
			}
			// A name (an enumerator), sizeof, a comparison...: sema's value, when it has one.
			if (expr->constantValue())
				return wrapToInteger(*expr->constantValue(), type);
			return std::nullopt;
		}

		// A double initializer (-fsoft-double), folded in the host's own binary64: the same IEEE arithmetic, rounded
		// the same way, as __f64_* would do at run time.
		std::optional<f64> foldGlobalDouble(const Expr* expr)
		{
			if (expr && isIntegerType(expr->type()))
			{
				std::optional<i64> value = foldGlobalInteger(expr);
				if (!value)
					return std::nullopt;
				const bool unsignedWide = !expr->type()->isSigned() && expr->type()->sizeInBytes() == 8;
				return unsignedWide ? static_cast<f64>(static_cast<u64>(*value)) : static_cast<f64>(*value);
			}
			if (expr && expr->constantValue())
				return static_cast<f64>(*expr->constantValue());
			if (const auto* lit = dynamic_cast<const FloatLiteralExpr*>(expr))
				return lit->isSingle() ? static_cast<f64>(static_cast<f32>(lit->value())) : lit->value();
			if (const auto* lit = dynamic_cast<const IntLiteralExpr*>(expr))
				return lit->isUnsigned() ? static_cast<f64>(lit->value()) : static_cast<f64>(static_cast<i64>(lit->value()));
			if (const auto* unary = dynamic_cast<const UnaryExpr*>(expr); unary && unary->op() == UnaryOp::Negate)
			{
				std::optional<f64> operand = foldGlobalDouble(unary->operand());
				return operand ? std::optional<f64>(-*operand) : std::nullopt;
			}
			if (const auto* cast = dynamic_cast<const CastExpr*>(expr))
			{
				// A cast to float on the way rounds to float, as it would at run time.
				std::optional<f64> operand = foldGlobalDouble(cast->operand());
				if (operand && cast->type() && cast->type()->isFloat())
					return static_cast<f64>(static_cast<f32>(*operand));
				return operand;
			}
			if (const auto* binary = dynamic_cast<const BinaryExpr*>(expr))
			{
				std::optional<f64> a = foldGlobalDouble(binary->lhs());
				std::optional<f64> b = foldGlobalDouble(binary->rhs());
				if (!a || !b)
					return std::nullopt;
				switch (binary->op())
				{
					case BinaryOp::Add: return *a + *b;
					case BinaryOp::Sub: return *a - *b;
					case BinaryOp::Mul: return *a * *b;
					case BinaryOp::Div: return *a / *b;
					default: return std::nullopt;
				}
			}
			return std::nullopt;
		}

		// The text of a float initializer. CASM types a literal by how it is SPELLED, and std::format
		// prints 1.0f as "1": the assembler then refuses it for an f32 ("Expected a literal value of type
		// f32"). So every finite value that would read as an integer gets a ".0".
		std::string floatLiteralText(f32 value)
		{
			std::string text = std::format("{}", value);
			if (text.find_first_of(".eEn") == std::string::npos) // no point, no exponent, not inf/nan
				text += ".0";
			return text;
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

		// A string literal's own .rodata object: a u8 array spelled as a CASM string, or for a wide
		// literal a u16/u32 array of its code units, zero unit last. CASM aligns each to its element.
		std::string casmStringLet(std::string_view name, std::string_view bytes, u32 elementSize)
		{
			if (elementSize <= 1)
				return std::format("let {}: u8[{}] = \"{}\"", name, bytes.size() + 1, escapeCasmString(bytes));
			const usize count = bytes.size() / elementSize;
			std::string text = std::format("let {}: u{}[{}] = [", name, elementSize * 8, count + 1);
			for (usize i = 0; i < count; ++i)
				text += std::format("{}, ", support::codeUnitAt(bytes, i, elementSize));
			return text + "0]";
		}


	}

	// ---- small helpers ----------------------------------------------------------------------------

	std::string CodeGen::sourceComment(SourceLocation location) const
	{
		if (!location.isValid())
			return {};
		// Back to the file the line was WRITTEN in, before naming it: everything from the lexer down
		// carries a position in the preprocessor's expanded buffer, and the whole value of these
		// comments is that a reader can go and look at the line they name.
		if (_lineMap)
			location = _lineMap->toOriginal(location);
		const support::SourceBuffer* buffer = _sourceManager.getBuffer(location.sourceId);
		return std::format("{}:{}", buffer ? buffer->name() : std::string_view("?"), location.line);
	}

	std::string CodeGen::slotFieldName(u32 slotIndex) { return std::format("slot{}", slotIndex); }

	std::string CodeGen::blockLabel(const ir::BasicBlock& block) const
	{
		// A function with a jump table names its blocks at file scope, so the table (a `.rodata`
		// symbol emitted outside the function's local-label scope) can reference them. Every other
		// function keeps the `.L<id>` local label it always had.
		if (_globalBlockLabels)
			return std::format("{}{}", _blockLabelPrefix, block.id());
		return std::format(".L{}", block.id());
	}

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

	std::string CodeGen::slotAddress(u32 slotIndex)
	{
		// A load or store displacement is a signed 16-bit field. A function with thousands of locals
		// (every -O0 local has its own slot) has slots beyond 32 KiB; those are reached through a
		// register instead of failing to assemble.
		constexpr u32 kMaxDisplacement = 32767 - 3;
		if (slotIndex >= _slotOffsets.size() || _slotOffsets[slotIndex] <= kMaxDisplacement)
			return std::format("[sp + {}.{}]", _frameName, slotFieldName(slotIndex));

		_emitter.instr(std::format("la at, {}", _slotOffsets[slotIndex]), "far frame slot");
		_emitter.instr("add at, at, sp", "far frame slot");
		return "[at + 0]";
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

		if (placement.kind == PlacementKind::Alias)
			return bankReg(placement.index, placement.isFloat);

		if (placement.kind == PlacementKind::Virtual)
		{
			// A FrameAddr naming a register-resident local has no address to hand out. Every
			// legitimate reader of one is a Load/Store, which handle it directly (see their cases in
			// generateInstr) - reaching here means the escape analysis and this file disagree.
			_diagnostics.error(DiagId::RegisterAddressEscaped, loc, "internal error: the address of a register-resident local escaped code generation");
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

	const IrReturnPayload* CodeGen::findTailCall(std::span<IrInstr* const> instrs, usize index) const
	{
		if (!_options.tailCalls || _generatingMain || _generatingInterrupt)
			return nullptr;
		if (index + 1 >= instrs.size() || instrs[index]->opcode() != IrOpcode::Call)
			return nullptr;

		const IrInstr& next = *instrs[index + 1];
		if (next.opcode() != IrOpcode::Return)
			return nullptr;

		const auto& call = instrs[index]->as<IrCallPayload>();
		if (call.isIndirect() || !call.inlineAsm.empty())
			return nullptr;
		// F3.4: a wide return needs two registers (ret0/ret1), while the tail call's `jp` leaves the
		// result in r0 only - and a wide argument needs two registers, which the argument-move pass
		// for a tail call does not do (checked where the arguments are read, above).
		if (call.hasWideResult)
			return nullptr;

		const auto& ret = next.as<IrReturnPayload>();
		if (call.hasResult != ret.hasValue)
			return nullptr;
		if (call.hasResult)
		{
			if (!(call.result == ret.value) || call.isFloat != ret.isFloat)
				return nullptr;
			// The result has to be private to this Return: a second reader would need the value
			// after the jump, which a tail call cannot provide. One definition too, since this IR
			// reuses temporary ids (see the header's note on that).
			auto uses = _useCount.find(call.result.id);
			auto defs = _defCount.find(call.result.id);
			if (uses == _useCount.end() || uses->second != 1)
				return nullptr;
			if (defs == _defCount.end() || defs->second != 1)
				return nullptr;
		}

		// Every argument must fit in an argument register: an outgoing stack word lives in the
		// caller's frame, which `leave` would have destroyed by the time the callee reads it.
		if (call.argCount > index)
			return nullptr;
		std::vector<bool> argIsFloat(call.argCount);
		std::vector<bool> argIsWide(call.argCount);
		for (u32 k = 0; k < call.argCount; ++k)
		{
			const IrInstr& paramInstr = *instrs[index - call.argCount + k];
			if (paramInstr.opcode() != IrOpcode::Param)
				return nullptr;
			argIsFloat[k] = paramInstr.as<IrParamPayload>().isFloat;
			argIsWide[k] = paramInstr.as<IrParamPayload>().isWide;
			if (argIsWide[k])
				return nullptr; // a wide argument needs two registers, which the tail call does not move
		}
		std::vector<ArgSlot> slots = assignArgSlots(argIsFloat, argIsWide,
			fixedArgCountOf(instrs.subspan(index - call.argCount, call.argCount)));
		for (const ArgSlot& slot : slots)
			if (slot.kind == ArgSlotKind::Stack)
				return nullptr;

		// A tail call `leave`s first, which reclaims the caller's frame. An argument that is the
		// address of a local in that frame (or derives from one, `&s->field`) would then point at
		// reclaimed stack - the callee writes and reads through it after the frame is gone. Refuse
		// the call when any argument value transitively derives from a FrameAddr.
		if (argReferencesFrame(instrs.subspan(index - call.argCount, call.argCount)))
			return nullptr;

		return &ret;
	}

	// True when any of `params` is, or derives from, the address of a frame local: mark every
	// temporary defined by a FrameAddr, then propagate through the address arithmetic (Copy, BinOp,
	// UnOp) that a `&s->field` produces. A Load of such an address yields a VALUE, not an address,
	// so it stops the propagation - which is exactly the distinction that matters here.
	bool CodeGen::argReferencesFrame(std::span<IrInstr* const> params) const
	{
		std::unordered_set<u32> frameDerived;
		for (const auto& block : _function->blocks())
		{
			for (const IrInstr* instr : block->instrs())
			{
				bool derived = instr->opcode() == IrOpcode::FrameAddr;
				if (!derived)
				{
					derived = instr->opcode() == IrOpcode::Copy
						? frameDerived.contains(instr->as<IrCopyPayload>().source.id)
						: false;
				}
				if (!derived && instr->opcode() == IrOpcode::BinOp)
				{
					const auto& p = instr->as<IrBinOpPayload>();
					derived = frameDerived.contains(p.lhs.id) || frameDerived.contains(p.rhs.id);
				}
				if (!derived && instr->opcode() == IrOpcode::UnOp)
					derived = frameDerived.contains(instr->as<IrUnOpPayload>().operand.id);
				if (derived)
					if (IrValue result = resultOf(*instr); result.isValid())
						frameDerived.insert(result.id);
			}
		}
		for (const IrInstr* paramInstr : params)
			if (IrValue v = paramInstr->as<IrParamPayload>().value; v.isValid() && frameDerived.contains(v.id))
				return true;
		return false;
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
				_emitter.instr(std::format("la {}, {}", dest, casmName(p.name)), comment); // pseudo form: la rd, symbol
				storeResult(p.result, dest, loc);
				break;
			}

			case IrOpcode::Load:
			{
				const auto& p = instr.as<IrLoadPayload>();
				if (std::optional<u32> local = _placement->virtualAddressLocal(p.address))
				{
					// Reading a local that lives in a register is just a register read - there is no
					// memory access to make at all. When the result is an alias of that register, not
					// even a move: the value is already where every later read expects it.
					if (_placement->temp(p.result).kind == PlacementKind::Alias)
						break;
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
				// [fp + 0], return address at [fp + 4]) - plus the callee-saved copies this function
				// pushed below its frame, which shift the whole incoming area up by one word each.
				// The fixed parameters that were passed on the stack occupy the first words of that
				// area, so the variadic tail begins right after them - and every variadic argument is
				// on the stack by construction (docs/09-Variadic-Convention.md), so from here on it
				// is just consecutive words.
				std::string dest = defineInto(p.result, kScratchA, false);
				_emitter.instr(std::format("la {}, [fp + {}]", dest, 8 + 4 * _calleeSavedWords + _fixedStackArgWords * 4), comment);
				storeResult(p.result, dest, loc);
				break;
			}

			case IrOpcode::Param:
				break; // handled when its Call is reached, below - see generateInstr()'s header comment

			case IrOpcode::Call:
			{
				const auto& p = instr.as<IrCallPayload>();
				if (p.callee == "__cc_memset")
					_usesMemset = true; // the routine is emitted at the end of @text, not linked in
				if (p.callee == "__cc_memcpy")
					_usesMemcpy = true;
				if (p.callee == "__cc_strlen")
					_usesStrlen = true;
				if (p.callee == "__cc_memchr_index")
					_usesMemchrIndex = true;
				if (p.callee == "__cc_div64")
					_usesDiv64 = true; // a 64-bit division/remainder site (F3.2)
				if (!p.inlineAsm.empty())
				{
					// The author's own text, a line at a time: a line ending in ':' is a label and stands at the left
					// margin, anything else is an instruction. Leading and trailing blanks do not matter.
					std::string_view text = p.inlineAsm;
					while (!text.empty())
					{
						const usize end = text.find('\n');
						std::string_view line = text.substr(0, end);
						text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
						while (!line.empty() && (line.front() == ' ' || line.front() == '\t' || line.front() == '\r'))
							line.remove_prefix(1);
						while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
							line.remove_suffix(1);
						if (line.empty())
							continue;
						if (line.back() == ':')
							_emitter.raw(line);
						else
							_emitter.instr(line, comment);
					}
					break;
				}
				if (p.argCount > index)
				{
					_diagnostics.error(DiagId::MalformedIrCall, loc, "malformed IR: call has more arguments than preceding parameter instructions");
					break;
				}
				u32 argCount = p.argCount;

				std::vector<bool> argIsFloat(argCount);
				std::vector<bool> argIsWide(argCount);
				std::vector<IrValue> argValues(argCount);
				for (u32 k = 0; k < argCount; ++k)
				{
					const IrInstr& paramInstr = *instrs[index - argCount + k];
					if (paramInstr.opcode() != IrOpcode::Param)
					{
						_diagnostics.error(DiagId::MalformedIrCall, loc, "malformed IR: call arguments are not contiguous parameter instructions");
						return;
					}
					const auto& param = paramInstr.as<IrParamPayload>();
					argIsFloat[k] = param.isFloat;
					argIsWide[k] = param.isWide;
					argValues[k] = param.value;
				}

				// No parallel-move hazard to worry about: the argument registers (r0-r3/f0-f3) are
				// never handed to a value in a function that makes a call at all
				// (value_placement.cpp's allocatable pools), so no source below can be one of the
				// destinations being written here.
				std::vector<ArgSlot> slots = assignArgSlots(argIsFloat, argIsWide,
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
							if (slot.wide)
							{
								// F3.4: the pair's address is in a register, and the two words are
								// loaded from it (offset 0 = low, 4 = high) straight into the two
								// argument registers. The address register is read before either
								// destination is written, which the address is always allowed to be.
								std::string base = valueIn(argValues[k], kScratchA, false, loc);
								std::string destHigh = bankReg(slot.index + 1, false);
								_emitter.instr(std::format("ldr {}, [{}]", dest, base), comment);
								_emitter.instr(std::format("ldr {}, [{} + 4]", destHigh, base), comment);
								break;
							}
							std::string source = valueIn(argValues[k], isFloat ? kScratchB : kScratchA, isFloat, loc);
							if (source != dest)
								_emitter.instr(std::format("mov {}, {}", dest, source), comment);
							break;
						}
						case ArgSlotKind::Stack:
						{
							if (slot.wide)
							{
								// The pair's address is in a register; its two words go to the two
								// outgoing stack words in order (low then high).
								std::string base = valueIn(argValues[k], kScratchA, false, loc);
								std::string scratch = intReg(kScratchB);
								_emitter.instr(std::format("ldr  {}, [{}]", scratch, base), comment);
								_emitter.instr(std::format("str  [sp + {}], {}", slot.index * 4, scratch), comment);
								_emitter.instr(std::format("ldr  {}, [{} + 4]", scratch, base), comment);
								_emitter.instr(std::format("str  [sp + {}], {}", (slot.index + 1) * 4, scratch), comment);
								break;
							}
							std::string source = valueIn(argValues[k], argIsFloat[k] ? kScratchB : kScratchA, argIsFloat[k], loc);
							_emitter.instr(std::format("str [sp + {}], {}", slot.index * 4, source), comment);
							break;
						}
					}
				}

				if (findTailCall(instrs, index) != nullptr)
				{
					// `return f(args)`: the arguments are in place, so restore the caller's frame
					// and callee-saved registers exactly as the epilogue would, then jump. The
					// callee's own `ret` pops the return address OUR caller pushed, so no new frame
					// is opened and the call costs no push/pop pair. The result needs no move: it is
					// already in r0/f0, which is where our caller reads it.
					if (_hasFrame)
						_emitter.instr("leave", comment);
					if (_calleeSavedFloatMask)
						_emitter.instr(std::format("fpopm 0x{:04X}", _calleeSavedFloatMask), comment);
					if (_calleeSavedIntMask)
						_emitter.instr(std::format("popm 0x{:04X}", _calleeSavedIntMask), comment);
					_emitter.instr(std::format("jp {}", casmName(p.callee)), comment);
					break;
				}

				if (p.isIndirect())
				{
					// `call rN` is the same mnemonic with a register operand - the assembler picks
					// CALLR from the operand's shape (05-Instruction-Set.md). r12 is the register to
					// put the address in: it is allocatable (value_placement.cpp's pools), and the
					// rule that nothing holds an allocatable register across a call is exactly what
					// makes it free at the moment of one. r4/r5 stay pure scratch and are never
					// handed out, so shuttling the address through one cannot disturb the arguments
					// already sitting in r0-r3.
					std::string target = valueIn(p.calleeValue, kScratchA, false, loc);
					if (target != intReg(12))
						_emitter.instr(std::format("mov {}, {}", intReg(12), target), comment);
					_emitter.instr(std::format("call {}", intReg(12)), comment);
				}
				else
				{
					_emitter.instr(std::format("call {}", casmName(p.callee)), comment);
				}
				if (p.hasResult || p.hasWideResult)
				{
					// The callee's result is in ret0 (r0, or f0 for a float) - and, for a wide result,
					// ret1 (r1). Both destinations are resolved before either move, because the
					// optimizer assigns r0/r1 like any other register: a low result whose home IS r1
					// would be written over the high word before it was read, and the pair can even come
					// out swapped. A wide result's `result` is the low word and `resultHigh` the high
					// one (wide results are never float, so ret0 is r0 on that path).
					const std::string ret0 = bankReg(0, p.isFloat);
					const std::string r1 = intReg(1);
					std::string dLow = p.hasResult
						? defineInto(p.result, p.isFloat ? kScratchB : kScratchA, p.isFloat)
						: std::string{};
					std::string dHigh = p.hasWideResult ? defineInto(p.resultHigh, kScratchB, false) : std::string{};
					if (p.hasWideResult && dLow == r1 && dHigh == ret0)
					{
						// Swapped: rotate through a scratch holding neither return word.
						_emitter.instr(std::format("mov {}, {}", intReg(kScratchA), ret0), comment);
						_emitter.instr(std::format("mov {}, {}", ret0, r1), comment);
						_emitter.instr(std::format("mov {}, {}", r1, intReg(kScratchA)), comment);
					}
					else if (p.hasWideResult && dLow == r1)
					{
						// Writing the low word into r1 would clobber the high word: save high first.
						_emitter.instr(std::format("mov {}, {}", dHigh, r1), comment);
						_emitter.instr(std::format("mov {}, {}", r1, ret0), comment);
					}
					else if (p.hasWideResult && dHigh == ret0)
					{
						// Writing the high word into r0 would clobber the low word: save low first.
						_emitter.instr(std::format("mov {}, {}", dLow, ret0), comment);
						_emitter.instr(std::format("mov {}, {}", ret0, r1), comment);
					}
					else
					{
						if (p.hasResult && dLow != ret0)
							_emitter.instr(std::format("mov {}, {}", dLow, ret0), comment);
						if (p.hasWideResult && dHigh != r1)
							_emitter.instr(std::format("mov {}, {}", dHigh, r1), comment);
					}
					if (p.hasResult)
						storeResult(p.result, dLow, loc);
					if (p.hasWideResult)
						storeResult(p.resultHigh, dHigh, loc);
				}
				break;
			}

			case IrOpcode::MachineOp:
			{
				// One instruction, written out as it stands. `halt` here is the one that suspends until
				// an interrupt arrives, not main's shutdown sequence - that one goes through the system
				// control device first (see the Return case above).
				_emitter.instr(std::string(ast::machineOpMnemonic(instr.as<IrMachineOpPayload>().op)), comment);
				break;
			}

			case IrOpcode::Builtin:
			{
				// One machine instruction, named by the builtin. The two scratch registers hold the
				// operands while the single instruction is written, exactly like BinOp/UnOp - none of
				// these builtins accumulates into a register it also reads (which is why `fma` is not
				// among them; see ast::Builtin's note), so aliasing dest with an operand is safe.
				const auto& p = instr.as<IrBuiltinPayload>();
				using ast::Builtin;

				// The stack pointer has no operand: it is `mov rd, sp` and nothing else.
				if (p.builtin == Builtin::StackPointer)
				{
					std::string dest = defineInto(p.result, kScratchA, false);
					_emitter.instr(std::format("mov {}, sp", dest), comment);
					storeResult(p.result, dest, loc);
					break;
				}

				// Nor do the flags, and nothing moves them into a register: `push` with no operand
				// is PUSHF, and the word comes straight back off the stack.
				if (p.builtin == Builtin::Flags)
				{
					std::string dest = defineInto(p.result, kScratchA, false);
					_emitter.instr("push", comment);
					_emitter.instr(std::format("pop {}", dest), comment);
					storeResult(p.result, dest, loc);
					break;
				}

				std::string_view mnemonic;
				switch (p.builtin)
				{
					case Builtin::Clz:          mnemonic = "clz"; break;
					case Builtin::Ctz:          mnemonic = "ctz"; break;
					case Builtin::Popcount:     mnemonic = "popcnt"; break;
					case Builtin::Bswap:        mnemonic = "bswap"; break;
					case Builtin::Abs:          mnemonic = "abs"; break;
					case Builtin::Rotl:         mnemonic = "rol"; break;
					case Builtin::Rotr:         mnemonic = "ror"; break;
					case Builtin::MulhUnsigned: mnemonic = "mulh"; break;
					case Builtin::MulhSigned:   mnemonic = "imulh"; break;
					// `abs` (not `fabs`) is the mnemonic for the float absolute value too: the assembler
					// picks ABS or FABS from the register bank, exactly as it picks ADDI over ADD.
					case Builtin::Fabs:         mnemonic = "abs"; break;
					case Builtin::Fmod:         mnemonic = "mod"; break; // FDIV's companion, F-typed
					case Builtin::Sqrt:         mnemonic = "sqrt"; break;
					case Builtin::Floor:        mnemonic = "ffloor"; break;
					case Builtin::Ceil:         mnemonic = "fceil"; break;
					case Builtin::Trunc:        mnemonic = "ftrunc"; break;
					case Builtin::Rint:         mnemonic = "fround"; break; // ties to even: C's rint/nearbyint
					case Builtin::Fmin:         mnemonic = "fmin"; break;
					case Builtin::Fmax:         mnemonic = "fmax"; break;
					case Builtin::Copysign:     mnemonic = "fcopysign"; break;
					case Builtin::Rcp:          mnemonic = "frecipe"; break;
					case Builtin::Rsqrt:        mnemonic = "frsqrte"; break;
					case Builtin::Fclass:       mnemonic = "fclass"; break;
					case Builtin::FloatBits:    mnemonic = "mff"; break;
					case Builtin::FloatFromBits: mnemonic = "mtf"; break;
					case Builtin::MinSigned:     mnemonic = "imin"; break;
					case Builtin::MaxSigned:     mnemonic = "imax"; break;
					case Builtin::MinUnsigned:   mnemonic = "min"; break;
					case Builtin::MaxUnsigned:   mnemonic = "max"; break;
					// Not machine instructions: IrBuilder lowers `expect` to its operand,
					// `constant_p` to a constant, and the overflow builtins to arithmetic plus a
					// comparison, so none of these reaches here. Listed so the switch stays
					// exhaustive over the machine builtins above, with the guard below as a backstop.
					case Builtin::Expect:
					case Builtin::ConstantP:
					case Builtin::AddOverflow:
					case Builtin::SubOverflow:
					case Builtin::MulOverflow:
					case Builtin::Memcpy:       // calls by the time they reach here (ir_builder.cpp)
					case Builtin::Memset:
					case Builtin::StackPointer: // handled above, before this switch
					case Builtin::Flags:        // ... and so is this one
						break;
				}
				if (mnemonic.empty())
					break;

				bool resultFloat = ast::builtinResultIsFloat(p.builtin);
				bool sourceFloat = ast::builtinSourceIsFloat(p.builtin);

				std::string a = valueIn(p.a, kScratchA, sourceFloat, loc);
				std::string dest = defineInto(p.result, kScratchB, resultFloat);
				if (p.b.isValid())
				{
					std::string b = valueIn(p.b, kScratchB, sourceFloat, loc);
					_emitter.instr(std::format("{} {}, {}, {}", mnemonic, dest, a, b), comment);
				}
				else
				{
					_emitter.instr(std::format("{} {}, {}", mnemonic, dest, a), comment);
				}
				storeResult(p.result, dest, loc);
				break;
			}

			case IrOpcode::Jump:
			{
				const auto& p = instr.as<IrJumpPayload>();
				if (_options.fallthroughBranches && p.target->id() == nextBlockId)
					break; // the target is the very next block emitted - falling through gets there
				_emitter.instr(std::format("jp {}", blockLabel(*p.target)), comment);
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
						blockLabel(*p.falseTarget), loc);
					break;
				}

				emitConditionalBranch(predicate, isUnsigned, isFloat, lhs, rhs,
					blockLabel(*p.trueTarget), loc);
				if (!falseIsNext)
					_emitter.instr(std::format("jp {}", blockLabel(*p.falseTarget)), comment);
				break;
			}

			case IrOpcode::TableJump:
			{
				// The dispatch: normalize the discriminant to a zero-based index, bounds-check it
				// unsigned (a value below `low` wraps to a huge unsigned and lands in `default` too),
				// load the target address from the `.rodata` table and jump through it. `r4`/`r5` are
				// the two scratch registers; the table's own base and the loaded address share `r5`.
				const auto& p = instr.as<IrTableJumpPayload>();
				std::string index = valueIn(p.discriminant, kScratchA, false, loc);
				std::string tableLabel = std::format("__ccjt_{}_{}", casmName(_function->name()), _nextJumpTableId++);

				if (p.low != 0)
				{
					if (p.low > 0 && p.low <= 0xFFFF)
						_emitter.instr(std::format("sub {}, {}, {}", intReg(kScratchA), index, p.low), comment);
					else
					{
						emitLoadImmediate(intReg(kScratchB), p.low, loc);
						_emitter.instr(std::format("sub {}, {}, {}", intReg(kScratchA), index, intReg(kScratchB)), comment);
					}
					index = intReg(kScratchA);
				}
				else if (index != intReg(kScratchA))
				{
					_emitter.instr(std::format("mov {}, {}", intReg(kScratchA), index), comment);
					index = intReg(kScratchA);
				}

				// `ifXX`'s immediate form is CMPI, whose 16-bit field is read SIGN-extended (unlike
				// SUBI/ADDI below, which zero-extend) - so the safe ceiling here is 0x7FFF, not
				// 0xFFFF. Beyond it the count goes in a register. IrBuilder caps a table at 256
				// entries today, so this is the guard for a future caller, not a live path.
				if (p.entryCount <= 0x7FFF)
					_emitter.instr(std::format("ifae {}, {}, {}", index, p.entryCount, blockLabel(*p.defaultTarget)), comment);
				else
				{
					emitLoadImmediate(intReg(kScratchB), p.entryCount, loc);
					_emitter.instr(std::format("ifae {}, {}, {}", index, intReg(kScratchB), blockLabel(*p.defaultTarget)), comment);
				}

				_emitter.instr(std::format("la {}, {}", intReg(kScratchB), tableLabel), comment);
				_emitter.instr(std::format("shl {}, {}, 2", index, index), comment);
				_emitter.instr(std::format("ldr {}, [{} + {}]", intReg(kScratchB), intReg(kScratchB), index), comment);
				_emitter.instr(std::format("jp {}", intReg(kScratchB)), comment);

				JumpTable table;
				table.label = std::move(tableLabel);
				table.entries.reserve(p.entryCount);
				for (u32 i = 0; i < p.entryCount; ++i)
					table.entries.push_back(blockLabel(*p.targets[i]));
				_jumpTables.push_back(std::move(table));
				break;
			}

			case IrOpcode::Return:
			{
				const auto& p = instr.as<IrReturnPayload>();
				if (p.hasWideValue)
				{
					// F3.4: the low word goes in ret0 (r0) and the high word in ret1 (r1). Both sources
					// are resolved BEFORE either move: the optimizer assigns r0/r1 like any other
					// register, so a high word already sitting in r0 must be saved before the low word
					// is written into r0 - and the pair can arrive exactly swapped.
					std::string low = valueIn(p.value, kScratchA, false, loc);
					std::string high = valueIn(p.highValue, kScratchB, false, loc);
					const std::string r0 = intReg(0);
					const std::string r1 = intReg(1);
					if (high == r0 && low == r1)
					{
						// Swapped: rotate through a scratch holding neither return word.
						_emitter.instr(std::format("mov {}, {}", intReg(kScratchA), r0), comment);
						_emitter.instr(std::format("mov {}, {}", r0, r1), comment);
						_emitter.instr(std::format("mov {}, {}", r1, intReg(kScratchA)), comment);
					}
					else
					{
						if (high == r0)
							_emitter.instr(std::format("mov {}, {}", r1, high), comment);
						if (low != r0)
							_emitter.instr(std::format("mov {}, {}", r0, low), comment);
						if (high != r1 && high != r0)
							_emitter.instr(std::format("mov {}, {}", r1, high), comment);
					}
				}
				else if (p.hasValue)
				{
					std::string returnReg = bankReg(0, p.isFloat);
					std::string source = valueIn(p.value, p.isFloat ? kScratchB : kScratchA, p.isFloat, loc);
					if (source != returnReg)
						_emitter.instr(std::format("mov {}, {}", returnReg, source), comment);
				}

				if (_generatingMain)
				{
					// `main` never returns to a caller - see the header comment on _generatingMain.
					// The return value (if any) is in r0 by now, and it is the program's exit status.
					if (_hasFrame)
						_emitter.instr("leave", comment);

					if (_mainCallsExit)
					{
						// A unit that declares `void exit(int)` is one that links a C library, and
						// falling off `main` is `exit(main())`: the handlers registered with atexit run,
						// the open files are flushed. `exit` does not return, but the halt keeps the
						// machine from running on into whatever follows if a replacement one does.
						if (!p.hasValue)
							_emitter.instr("li r0, 0", comment);
						_emitter.instr(std::format("call {}", mangledName("exit")), comment);
						_emitter.instr("halt", comment);
					}
					else
					{
						// No library to hand the status to, so main stops the machine itself: a word
						// write to the system control device carries the status in bits 15:8
						// (07-IO-Devices-and-Ports.md). A `return;` with no value is status 0, which is
						// what a plain byte write has always meant.
						_emitter.instr(std::format("la {}, 0xFFFF0000", intReg(kScratchA)), comment);
						if (p.hasValue)
						{
							_emitter.instr(std::format("shl {}, r0, 8", intReg(kScratchB)), comment);
							_emitter.instr(std::format("or {}, {}, 1", intReg(kScratchB), intReg(kScratchB)), comment);
							_emitter.instr(std::format("str [{} + 0], {}", intReg(kScratchA), intReg(kScratchB)), comment);
						}
						else
						{
							_emitter.instr(std::format("li {}, 1", intReg(kScratchB)), comment);
							_emitter.instr(std::format("strb [{} + 0], {}", intReg(kScratchA), intReg(kScratchB)), comment);
						}
						_emitter.instr("halt", comment);
					}
				}
				else
				{
					if (_hasFrame)
						_emitter.instr("leave", comment);
					// The callee-saved copies pushed by the prologue come back off in the reverse
					// order: floats first, then the integer mask - before the return, so the caller
					// finds its registers exactly as it left them. An interrupt handler never reaches
					// this branch (its epilogue restores the full allocatable set instead).
					if (!_generatingInterrupt && _calleeSavedFloatMask)
						_emitter.instr(std::format("fpopm 0x{:04X}", _calleeSavedFloatMask), comment);
					if (!_generatingInterrupt && _calleeSavedIntMask)
						_emitter.instr(std::format("popm 0x{:04X}", _calleeSavedIntMask), comment);
					if (_generatingInterrupt)
					{
						// `iret` pops the PC and then the flags the dispatcher pushed, so the registers
						// have to come back off the stack first - and `ret` would pop a return address
						// nobody ever wrote.
						emitInterruptEpilogue(comment);
						_emitter.instr("iret", comment);
					}
					else
					{
						_emitter.instr("ret", comment);
					}
				}
				break;
			}
		}
	}

	// ---- interrupt handlers -----------------------------------------------------------------------

	namespace
	{
		// Does anything in this function touch the float bank? Asked of the IR rather than of the C
		// declaration because the answer has to cover temporaries too - a handler with no float
		// local can still compute one - and because by this point the IR is what codegen will
		// actually walk.
		bool usesFloatBank(const IrFunction& function)
		{
			for (const IrLocalSlot& slot : function.localSlots())
				if (slot.isFloat)
					return true;

			for (const auto& block : function.blocks())
			{
				for (const IrInstr* instr : block->instrs())
				{
					switch (instr->opcode())
					{
						case IrOpcode::Const:   if (instr->as<IrConstPayload>().isFloat) return true; break;
						case IrOpcode::BinOp:   if (instr->as<IrBinOpPayload>().isFloat) return true; break;
						case IrOpcode::UnOp:    if (instr->as<IrUnOpPayload>().isFloat) return true; break;
						case IrOpcode::Cmp:     if (instr->as<IrCmpPayload>().isFloat) return true; break;
						case IrOpcode::Copy:    if (instr->as<IrCopyPayload>().isFloat) return true; break;
						case IrOpcode::Load:    if (instr->as<IrLoadPayload>().isFloat) return true; break;
						case IrOpcode::Store:   if (instr->as<IrStorePayload>().isFloat) return true; break;
						case IrOpcode::Param:   if (instr->as<IrParamPayload>().isFloat) return true; break;
						case IrOpcode::Return:  if (instr->as<IrReturnPayload>().isFloat) return true; break;
						case IrOpcode::CondJump: if (instr->as<IrCondJumpPayload>().isFloat) return true; break;
						case IrOpcode::Builtin:
							if (ast::builtinTouchesFloatBank(instr->as<IrBuiltinPayload>().builtin))
								return true;
							break;
						case IrOpcode::Call:
							// A callee may use the bank whatever this body does, and nothing here can
							// see into it - so any call at all makes the answer yes.
							return true;
						default:
							break;
					}
				}
			}
			return false;
		}
	}

	void CodeGen::emitInterruptPrologue(const IrFunction& function, std::string_view comment)
	{
		// One instruction for the whole integer set. `pushm` is all or nothing: it checks room for
		// the entire mask before storing anything, so a handler can never end up half-saved on a
		// stack it does not own (05-Instruction-Set.md).
		_emitter.instr(std::format("pushm 0x{:04X}", kInterruptSaveMask), comment);

		// The float bank goes back in one `fpushm` as well - worth paying only when the handler can
		// reach the bank at all. The mask is f0-f7 always (caller-saved and scratch), plus whichever
		// callee-saved f8-f15 value placement handed out. The second term is always empty today: a
		// handler has no parameters, and only a parameter can earn a callee-saved register - but
		// OR-ing it in keeps the mask right by construction if that ever changes.
		_interruptSavesFloats = usesFloatBank(function);
		if (_interruptSavesFloats)
		{
			_interruptFloatMask = kInterruptCallerSavedFloatMask | _calleeSavedFloatMask;
			_emitter.instr(std::format("fpushm 0x{:04X}", _interruptFloatMask), comment);
		}
	}

	void CodeGen::emitInterruptEpilogue(std::string_view comment)
	{
		// Exactly the prologue reversed: the floats came off a stack that grows down, so the last
		// one pushed is the first one back.
		if (_interruptSavesFloats)
			_emitter.instr(std::format("fpopm 0x{:04X}", _interruptFloatMask), comment);
		_emitter.instr(std::format("popm 0x{:04X}", kInterruptSaveMask), comment);
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
		_calleeSavedIntMask = placement.calleeSavedIntMask();
		_calleeSavedFloatMask = placement.calleeSavedFloatMask();
		_calleeSavedWords = std::popcount(_calleeSavedIntMask) + std::popcount(_calleeSavedFloatMask);
		bool hasFields = placement.outgoingSlotCount() > 0 || !placement.slots().empty();
		_frameName = hasFields ? std::format("__frame_{}", decl.name()) : std::string{};

		_emitter.blank();
		_emitter.raw(std::format("// {} - {}", decl.name(), sourceComment(decl.location())));

		_slotOffsets.clear();
		u32 frameBytes = 0;
		if (hasFields)
		{
			_emitter.raw(std::format("struct {}", _frameName));
			bool hasWordField = placement.outgoingSlotCount() > 0;
			u32 offset = placement.outgoingSlotCount() * 4;
			for (u32 i = 0; i < placement.outgoingSlotCount(); ++i)
				_emitter.raw(std::format("    outgoing{}: u32", i));
			for (u32 i = 0; i < placement.slots().size(); ++i)
			{
				const FrameSlotInfo& slot = placement.slots()[i];
				hasWordField = hasWordField || slot.sizeInBytes >= 4;
				_emitter.raw(std::format("    {}: {}", slotFieldName(i), fieldTypeName(slot.sizeInBytes, slot.isFloat)));

				// A field sits at a multiple of its own alignment: a byte anywhere, a halfword on an
				// even offset, anything wider (words, floats, arrays of words) on a multiple of 4.
				const u32 alignment = slot.isFloat ? 4 : (slot.sizeInBytes == 1 ? 1 : (slot.sizeInBytes == 2 ? 2 : 4));
				offset = (offset + alignment - 1) / alignment * alignment;
				_slotOffsets.push_back(offset);
				offset += slot.isFloat ? 4 : (slot.sizeInBytes <= 2 ? slot.sizeInBytes : (slot.sizeInBytes + 3) / 4 * 4);
			}
			// CASM rounds a struct's size up to its WIDEST field's alignment, and `enter` reserves exactly
			// that many bytes. A frame of nothing but bytes or halfwords (a lone `char` parameter that spilled)
			// would be 1 or 2 bytes, leaving sp misaligned for every function it calls - whose first word
			// store then faults (AlignmentFault). One word field keeps the frame a multiple of 4.
			if (!hasWordField)
			{
				_emitter.raw("    pad: u32");
				offset = (offset + 3) / 4 * 4 + 4;
			}
			frameBytes = (offset + 3) / 4 * 4;
			_emitter.raw("endstruct");
		}

		bool isEntryPoint = decl.name() == "main"; // must be `global` - 12-Labels-and-Symbols.md's "The main entry point"
		_generatingMain = isEntryPoint;
		_generatingInterrupt = function.isInterruptHandler();
		// `global` is what publishes a symbol to the linker (12-Labels-and-Symbols.md), so it is
		// exactly C's external linkage: everything except a `static` function. `main` gets it
		// regardless - the linker looks that name up to find the entry point.
		bool exported = isEntryPoint || decl.hasExternalLinkage();
		_emitter.label(exported ? std::format("global {}", casmName(decl.name())) : casmName(decl.name()));

		SourceLocation entryLoc = decl.location();
		// Before `enter`, so `leave` puts sp back exactly where the restore expects to find it.
		if (_generatingInterrupt)
			emitInterruptPrologue(function, sourceComment(entryLoc));
		// A normal function that gave a local a callee-saved register has to put the caller's value
		// in it somewhere before it overwrites it, and take it back before it returns. The saved
		// copies go below the frame, ahead of `enter`, so the [fp + N] incoming-argument reads see a
		// shift of exactly _calleeSavedWords (accounted for where those reads are emitted).
		if (!_generatingInterrupt && _calleeSavedIntMask)
			_emitter.instr(std::format("pushm 0x{:04X}", _calleeSavedIntMask), sourceComment(entryLoc));
		if (!_generatingInterrupt && _calleeSavedFloatMask)
			_emitter.instr(std::format("fpushm 0x{:04X}", _calleeSavedFloatMask), sourceComment(entryLoc));
		if (_hasFrame)
		{
			// `enter Frame` carries the frame's size as a 16-bit immediate. A frame past that (a function
			// whose -O0 slots run to tens of thousands) gets a bare `enter`, which sets up fp, and the
			// space is taken off sp by hand; `leave` puts sp back from fp either way.
			constexpr u32 kMaxEnterFrame = 0xFFFF;
			if (hasFields && frameBytes > kMaxEnterFrame)
			{
				_emitter.instr("enter", sourceComment(entryLoc));
				_emitter.instr(std::format("la at, {}", frameBytes), sourceComment(entryLoc));
				_emitter.instr("sub sp, sp, at", sourceComment(entryLoc));
			}
			else
			{
				_emitter.instr(hasFields ? std::format("enter {}", _frameName) : "enter", sourceComment(entryLoc));
			}
		}

		// Settle every parameter into wherever it lives for the rest of the function: nothing at all
		// when it already arrived in the register it was assigned, a move when it was assigned a
		// different one, a store when it lives in a frame field. A parameter that arrived on the
		// stack is read from the CALLER's frame at [fp + 8], [fp + 12], ... (24-Calling-Convention.md).
		std::span<const ArgSlot> arrivals = placement.paramArrival();
		_fixedStackArgWords = 0;
		for (const ArgSlot& arrival : arrivals)
			if (arrival.kind == ArgSlotKind::Stack)
				_fixedStackArgWords += arrival.wide ? 2 : 1;

		for (u32 i = 0; i < function.paramCount(); ++i)
		{
			const ArgSlot& arrival = arrivals[i];
			const IrLocalSlot& slot = function.localSlots()[i];
			Placement home = placement.local(i);
			std::string comment = sourceComment(entryLoc);

			if (home.kind == PlacementKind::None)
				continue; // nothing in the body reads this parameter - it can stay where it landed

			if (arrival.wide)
			{
				// F3.4: a 64-bit parameter arrives as two words and its home is an 8-byte slot (a
				// wide value is never register-placed - it is a pair in memory). The two words come
				// from the two arrival registers, or from [fp + 8 + ...] two words apiece.
				std::string low;
				std::string high;
				if (arrival.kind == ArgSlotKind::Stack)
				{
					u32 base = 8 + 4 * _calleeSavedWords + arrival.index * 4;
					low = bankReg(kScratchA, false);
					high = bankReg(kScratchB, false);
					_emitter.instr(std::format("ldr {}, [fp + {}]", low, base), comment);
					_emitter.instr(std::format("ldr {}, [fp + {}]", high, base + 4), comment);
				}
				else
				{
					low = bankReg(arrival.index, false);
					high = bankReg(arrival.index + 1, false);
				}
				const u32 homeIndex = home.kind == PlacementKind::Slot ? home.index : i;
				// A wide slot is 8 bytes and needs its high word at +4. The symbolic `[sp + Frame.f]`
				// form can only name the field itself, so the address goes into a scratch register
				// once and both words are stored through it.
				_emitter.instr(std::format("la at, {}.{}", _frameName, slotFieldName(homeIndex)), comment);
				_emitter.instr("add at, at, sp", comment);
				_emitter.instr(std::format("str [at], {}", low), comment);
				_emitter.instr(std::format("str [at + 4], {}", high), comment);
				continue;
			}

			if (arrival.kind == ArgSlotKind::Stack)
			{
				std::string scratch = bankReg(kScratchA, slot.isFloat);
				std::string target = home.kind == PlacementKind::Register ? bankReg(home.index, home.isFloat) : scratch;
				_emitter.instr(std::format("ldr {}, [fp + {}]", target, 8 + 4 * _calleeSavedWords + arrival.index * 4), comment);
				if (home.kind == PlacementKind::Slot)
					_emitter.instr(std::format("{} {}, {}", storeMnemonicForSize(slot.sizeInBytes, slot.isFloat),
						slotAddress(home.index), target), comment);
				continue;
			}

			std::string arrived = bankReg(arrival.index, arrival.kind == ArgSlotKind::FloatReg);
			if (home.kind == PlacementKind::Register)
			{
				std::string target = bankReg(home.index, home.isFloat);
				// A sub-word parameter is normalized on the way in, because it came from outside
				// this function. Everything the body itself writes to the local is already in its
				// narrowed representation (ir_instr.h's invariant), but the caller's word is only
				// what the caller chose to put there - and the `strb`/`strh` this parameter used to
				// settle into a frame field truncated it for free. One instruction says the same
				// thing, and it replaces the move rather than following it.
				if (!slot.isFloat && slot.sizeInBytes < 4)
				{
					_emitter.instr(narrowToWidth(target, arrived, slot.sizeInBytes, slot.isSigned), comment);
					continue;
				}
				if (target != arrived)
					_emitter.instr(std::format("mov {}, {}", target, arrived), comment);
				continue;
			}
			_emitter.instr(std::format("{} {}, {}", storeMnemonicForSize(slot.sizeInBytes, slot.isFloat),
				slotAddress(home.index), arrived), comment);
		}

		std::span<const std::unique_ptr<BasicBlock>> blocks = function.blocks();

		// A function with a jump table names its blocks at file scope, so the `.rodata` table can
		// reference them (see blockLabel()). Decided once, before the first label is emitted.
		_globalBlockLabels = false;
		for (const auto& block : blocks)
		{
			for (const IrInstr* instr : block->instrs())
			{
				if (instr->opcode() == IrOpcode::TableJump)
				{
					_globalBlockLabels = true;
					break;
				}
			}
			if (_globalBlockLabels)
				break;
		}
		_blockLabelPrefix = _globalBlockLabels ? std::format("__ccbb_{}_", casmName(decl.name())) : std::string{};

		for (usize b = 0; b < blocks.size(); ++b)
		{
			// `blockLabel()` is the single source of truth for a block's name: it already carries the
			// leading dot for a local label, and `label()` emits `name:` either way, so the definition
			// and every `jp`/`ifXX`/table entry that references it can never drift apart.
			_emitter.label(blockLabel(*blocks[b]));
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
				// The Return a tail call folds into the Call before it is emitted by that Call.
				if (findTailCall(instrs, i) != nullptr)
					_skipInstr[i + 1] = true;
			}

			for (usize i = 0; i < instrs.size(); ++i)
				generateInstr(instrs, i, nextBlockId);
		}

		_skipInstr.clear();
		_suppressedConsts.clear();
		_frameName.clear();
		_globalBlockLabels = false;
		_blockLabelPrefix.clear();
		_slotOffsets.clear();
		_calleeSavedIntMask = 0;
		_calleeSavedFloatMask = 0;
		_calleeSavedWords = 0;
		_generatingMain = false;
		_generatingInterrupt = false;
		_interruptSavesFloats = false;
		_interruptFloatMask = 0;
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
		if (!element || element->isAggregate() || element->isWide() || dimensions.empty())
			return {}; // not this shape - the caller falls back to a flat word array

		std::string name = fieldTypeName(element->sizeInBytes(), element->isFloat());
		for (u32 dimension : dimensions)
			name += std::format("[{}]", dimension);
		return name;
	}

	namespace
	{
		// An expression whose value is the ADDRESS of something with static storage: a string
		// literal, an explicit `&x`, or a name that decays to one (an array, a function). C calls
		// these address constants and allows them as static initializers; this back end cannot
		// place one, which is a limitation worth naming precisely rather than calling the program
		// wrong. See docs/06-Known-Limitations.md.
		bool isAddressConstant(const Expr* expr)
		{
			if (!expr)
				return false;
			if (dynamic_cast<const StringLiteralExpr*>(expr))
				return true;
			if (const auto* cast = dynamic_cast<const ast::CastExpr*>(expr))   // `(char*)buffer` is still that address
				return isAddressConstant(cast->operand());
			if (const auto* unary = dynamic_cast<const ast::UnaryExpr*>(expr))
				return unary->op() == ast::UnaryOp::AddressOf;
			if (const auto* name = dynamic_cast<const ast::NameExpr*>(expr))
				return name->type() && (name->type()->isArray() || name->type()->isFunction());
			return false;
		}
	}

	std::optional<std::string> CodeGen::addressConstantSymbol(const Expr* expr) const
	{
		if (!expr)
			return std::nullopt;

		// A string literal in an initializer has no IR entry, so its label is minted here. The
		// `.lit` prefix stays out of the IR's `.str` numbering (see mangledName()).
		if (const auto* literal = dynamic_cast<const StringLiteralExpr*>(expr))
		{
			auto [it, inserted] = _initializerStringNames.try_emplace(literal, std::string{});
			if (inserted)
			{
				it->second = mangledName(std::format(".lit{}", _nextInitializerStringId++));
				_initializerStringOrder.push_back(literal);
			}
			return it->second;
		}

		// A cast leaves an address as it was: `(char*)table`, `(void*)&x`.
		if (const auto* cast = dynamic_cast<const CastExpr*>(expr))
			return addressConstantSymbol(cast->operand());

		if (const auto* unary = dynamic_cast<const UnaryExpr*>(expr); unary && unary->op() == UnaryOp::AddressOf)
		{
			if (const auto* name = dynamic_cast<const NameExpr*>(unary->operand()))
				return symbolForName(name->name());
			return std::nullopt;
		}

		if (const auto* name = dynamic_cast<const NameExpr*>(expr))
		{
			if (name->type() && (name->type()->isArray() || name->type()->isFunction()))
				return symbolForName(name->name());
		}

		return std::nullopt;
	}

	void CodeGen::emitInitializerStringLiterals()
	{
		for (const StringLiteralExpr* literal : _initializerStringOrder)
		{
			_emitter.raw(casmStringLet(_initializerStringNames.at(literal), literal->value().view(), literal->elementSize()));
		}
	}

	std::string CodeGen::symbolForName(std::string_view name) const
	{
		if (const auto it = _staticLocalSymbols.find(name); it != _staticLocalSymbols.end())
			return it->second;
		return casmName(name);
	}

	std::string CodeGen::casmName(std::string_view name) const
	{
		return casmSymbolName(_asmLabels, name);
	}

	std::optional<std::string> CodeGen::scalarArrayInitText(const Type* type, const Expr* init, const Expr*& outOffender) const
	{
		if (!type || !init)
			return std::nullopt;
		if (type->isArray())
		{
			if (const StringLiteralExpr* filling = stringFillingArray(type, init))
				init = filling;                          // `char s[] = { "abc" }` too
		}

		// A string literal fills a char array as bytes, terminating zero and all - CASM's own
		// `let greeting: u8[16] = "Hello, CeresVM!"` (11-Data-Types-and-Literals.md), which pads the
		// rest with zeros exactly like a short value list does.
		//
		// Only an ARRAY. Initializing a POINTER with one asks for the literal's address, which is a
		// different thing entirely - and used to fall through to the byte list below and write the
		// first character's code into the pointer, so `char* n[2] = {"a", "b"}` became {97, 98}.
		if (const auto* literal = dynamic_cast<const StringLiteralExpr*>(init))
		{
			if (!type->isArray())
				return addressConstantSymbol(init); // a POINTER takes the literal's address
			if (literal->elementSize() != 1)
			{
				// A wide literal fills its array with code units, as a value list; wchar_t is int, so
				// an L literal's units are written signed.
				const bool isSigned = type->arrayElementType() && type->arrayElementType()->isInt();
				std::string text = "[";
				for (usize i = 0; i < literal->length(); ++i)
				{
					const u32 unit = support::codeUnitAt(literal->value().view(), i, literal->elementSize());
					text += i == 0 ? "" : ", ";
					text += isSigned ? std::format("{}", static_cast<i32>(unit)) : std::format("{}", unit);
				}
				return text + "]";
			}
			if (type->arrayElementType() && type->arrayElementType()->sizeInBytes() == 1)
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
				return scalarArrayInitText(type, list->elements().front(), outOffender);
			}
			const Type* element = type->arrayElementType();
			std::string text = "[";
			bool first = true;
			for (const Expr* value : list->elements())
			{
				std::optional<std::string> part = scalarArrayInitText(element, value, outOffender);
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

		// `&x`, an array's name, a function's name - the address of a symbol, not a value.
		if (auto address = addressConstantSymbol(init))
			return address;

		if (type->isFloat())
		{
			std::optional<f32> value = foldGlobalFloat(init);
			return value ? std::optional<std::string>(floatLiteralText(*value)) : std::nullopt;
		}
		std::optional<i64> value = foldGlobalInt(init);
		return value ? std::optional<std::string>(std::format("{}", *value)) : std::nullopt;
	}

	bool CodeGen::buildGlobalImage(const Type* type, const Expr* init, u32 offset, std::vector<u8>& image,
		std::vector<WordReference>& words, const Expr*& outOffender) const
	{
		if (!type)
			return false;
		if (!init)
			return true; // nothing to write - the image is already zero, which is what C promises

		u32 size = type->sizeInBytes();
		if (offset + size > image.size())
			return false; // sema already reported the overflow
		if (type->isArray())
		{
			if (const StringLiteralExpr* filling = stringFillingArray(type, init))
				init = filling;                          // `char s[] = { "abc" }` too
		}

		// A string literal fills a char ARRAY as bytes; it fills a POINTER with its own address, which
		// is a symbol the assembler relocates, not a byte value.
		if (const auto* literal = dynamic_cast<const StringLiteralExpr*>(init))
		{
			if (!type->isArray())
			{
				if (type->sizeInBytes() != 4)
				{
					outOffender = init;
					return false;
				}
				words.push_back(WordReference{ offset / 4, addressConstantSymbol(init).value() });
				return true;
			}
			std::string_view text = literal->value().view();
			for (usize i = 0; i < text.size() && i < size; ++i)
				image[offset + i] = static_cast<u8>(text[i]);
			return true; // the terminating zero and any padding are already zero
		}

		// `&x`, an array's name, a function's name - the address of a symbol, not a value. The word
		// is left zero here and recorded so the caller can spell the symbol's name in its place.
		if (auto address = addressConstantSymbol(init); address.has_value())
		{
			if (type->sizeInBytes() != 4)
			{
				outOffender = init;
				return false;
			}
			words.push_back(WordReference{ offset / 4, std::move(address.value()) });
			return true;
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
					if (!buildGlobalImage(element, value, offset + index * elementSize, image, words, outOffender))
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
					if (!buildGlobalImage(fields[index].type, value,
						offset + sema::fieldOffset(*structDecl, static_cast<u32>(index)), image, words, outOffender))
						return false;
					++index;
				}
				return true;
			}
			// A scalar with braces - `int x = { 5 }`; sema already required exactly one value.
			return list->elements().empty() ||
				buildGlobalImage(type, list->elements().front(), offset, image, words, outOffender);
		}

		// One scalar, written little-endian (02-Memory.md) in its own declared width.
		u64 bits = 0;
		if (type->isDouble())
		{
			std::optional<f64> value = foldGlobalDouble(init);
			if (!value)
			{
				outOffender = init;
				return false;
			}
			bits = std::bit_cast<u64>(*value);
		}
		else if (type->isFloat())
		{
			std::optional<f32> value = foldGlobalFloat(init);
			if (!value)
			{
				outOffender = init;
				return false;
			}
			bits = std::bit_cast<u32>(*value);
		}
		else
		{
			std::optional<i64> value = foldGlobalInt(init);
			if (!value)
			{
				outOffender = init;
				return false;
			}
			bits = static_cast<u64>(*value);
		}
		for (u32 i = 0; i < size && i < 8; ++i)
			image[offset + i] = static_cast<u8>((bits >> (8 * i)) & 0xFF);
		return true;
	}

	void CodeGen::reportUnrepresentableInitializer(const VarDecl& decl, const Expr* offender)
	{
		if (!isAddressConstant(offender))
		{
			_diagnostics.error(DiagId::GlobalInitializerNotConstant, decl.location(),
				"initializer for global '{}' must be a compile-time constant", decl.name());
			return;
		}
		// C calls this an address constant and allows it, so "must be a compile-time constant" is
		// the wrong thing to tell the program: it IS one. A whole object's address - a string
		// literal, `&name`, an array's or function's name - is written as a symbol and relocated, but
		// what reaches here is an address with an OFFSET (`&a[i]`), which nothing on this side can
		// spell as a relocation. It is still rare and still worth naming rather than guessing at.
		_diagnostics.error(DiagId::AddressConstantInStaticInitializer, decl.location(),
			"cannot initialize '{}' with {}: only the address of a whole object can be written into "
			"a static initializer - an address with an offset (like '&a[i]') has to be computed at "
			"run time. Assign it inside a function instead.",
			decl.name(),
			dynamic_cast<const StringLiteralExpr*>(offender) ? "the address of a string literal"
				: "an address constant");
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
			const Expr* offender = nullptr;
			std::optional<std::string> values = scalarArrayInitText(type, decl.initializer(), offender);
			if (!values)
			{
				reportUnrepresentableInitializer(decl, offender);
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
		std::vector<WordReference> wordReferences;
		const Expr* offender = nullptr;
		if (!buildGlobalImage(type, decl.initializer(), 0, image, wordReferences, offender))
		{
			reportUnrepresentableInitializer(decl, offender);
			return;
		}

		// Each word is spelled as hex, except a pointer field's word, which is a symbol's name the
		// assembler relocates into the address.
		std::vector<std::string> wordTexts(words, std::string{});
		for (u32 w = 0; w < words; ++w)
			wordTexts[w] = std::format("0x{:08X}",
				static_cast<u32>(image[w * 4]) | (static_cast<u32>(image[w * 4 + 1]) << 8) |
				(static_cast<u32>(image[w * 4 + 2]) << 16) | (static_cast<u32>(image[w * 4 + 3]) << 24));
		for (const WordReference& reference : wordReferences)
			wordTexts[reference.wordIndex] = reference.symbol;

		std::string values;
		for (u32 w = 0; w < words; ++w)
		{
			if (w != 0)
				values += ", ";
			values += wordTexts[w];
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

		// A 64-bit scalar is two words, little-endian (`u32[2] = [lo, hi]`), which a single CASM
		// integer literal could not spell. A wide ARRAY or struct goes through the flat-word path
		// above, which already writes the bytes correctly.
		if (type->isWide())
		{
			std::string let = exported ? "global let" : "let";
			std::string name{ symbolName };
			std::string comment = std::format("{} ({} bytes)", AstPrinter::typeName(type), type->sizeInBytes());
			if (!decl.initializer())
			{
				_emitter.raw(std::format("{} {}: u32[2]   // {}", let, name, comment));
				return;
			}
			u64 bits = 0;
			if (type->isDouble())
			{
				std::optional<f64> value = foldGlobalDouble(decl.initializer());
				if (!value)
				{
					reportUnrepresentableInitializer(decl, decl.initializer());
					return;
				}
				bits = std::bit_cast<u64>(*value);
			}
			else
			{
				std::optional<i64> value = foldGlobalInt(decl.initializer());
				if (!value)
				{
					reportUnrepresentableInitializer(decl, decl.initializer());
					return;
				}
				bits = static_cast<u64>(*value);
			}
			_emitter.raw(std::format("{} {}: u32[2] = [0x{:08X}, 0x{:08X}]   // {}",
				let, name, static_cast<u32>(bits), static_cast<u32>(bits >> 32), comment));
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
				reportUnrepresentableInitializer(decl, decl.initializer());
				return;
			}
			_emitter.raw(std::format("{} {}: {} = {}", let, name, casmType, floatLiteralText(*value)));
		}
		else
		{
			// A pointer initialized with an address constant - `char* p = "hi"`, `int* q = &g`,
			// `int* r = a` - writes the symbol's name, which the assembler relocates into the address.
			if (auto address = addressConstantSymbol(decl.initializer()))
			{
				_emitter.raw(std::format("{} {}: {} = {}", let, name, casmType, *address));
				return;
			}
			std::optional<i64> value = foldGlobalInt(decl.initializer());
			if (!value)
			{
				reportUnrepresentableInitializer(decl, decl.initializer());
				return;
			}
			_emitter.raw(std::format("{} {}: {} = {}", let, name, casmType, *value));
		}
	}

	void CodeGen::generateStringLiterals(const IrModule& module)
	{
		for (const IrGlobalString& literal : module.stringLiterals())
		{
			_emitter.raw(casmStringLet(mangledName(literal.name), literal.value.view(), literal.elementSize));
		}
	}

	void CodeGen::emitCarriedRoutines()
	{
		// The loop-idiom routines (ir_optimizer.cpp's lowerLoopIdioms) and the memcpy/memset builtins'
		// (ir_builder.cpp). Each is one of the machine's block instructions (CeresASM 6b6372b), which do the
		// whole job a page per step: a leaf with no frame that touches r0-r2 (r7 for the index) and
		// returns nothing, so it is just `ret` after. The counts are signed, so a non-positive `n` does
		// nothing - exactly what the `for (i = 0; i < n; i++)` each replaces did. File-level (no `global`),
		// so each unit carries its own copy and two units that both use one never collide at link time.
		if (_usesMemset)
		{
			_emitter.raw("// emitted because a byte-fill loop or __builtin_memset asked for it (docs/14 O15)");
			_emitter.label("__cc_memset");
			_emitter.instr("ifle r2, 0, .ccm_done");   // a signed count <= 0 fills nothing
			_emitter.instr("mset r0, r1, r2");         // the low byte of r1, r2 times from r0
			_emitter.localLabel("ccm_done");
			_emitter.instr("ret");
		}

		// The byte-copy loop's routine. The C loop it replaces is a FORWARD byte copy, well defined even
		// when the ranges overlap (with `d > s` it repeats bytes) - and that is exactly what `mcpy` is
		// defined to do, so no overlap test is needed.
		if (_usesMemcpy)
		{
			_emitter.raw("// emitted because a byte-copy loop or __builtin_memcpy asked for it (docs/14 O15)");
			_emitter.label("__cc_memcpy");
			_emitter.instr("ifle r2, 0, .ccm2_done");  // a signed count <= 0 copies nothing
			_emitter.instr("mcpy r0, r1, r2");         // forward, lowest byte first
			_emitter.localLabel("ccm2_done");
			_emitter.instr("ret");
		}

		// The strlen scan's routine: the number of bytes before the terminator, found by one `mscan` for
		// a zero byte with no limit of its own.
		if (_usesStrlen)
		{
			_emitter.raw("// emitted because a strlen-style scan was recognized (docs/14 O15)");
			_emitter.label("__cc_strlen");
			_emitter.instr("mov  r1, r0");
			_emitter.instr("li   r2, 0");
			_emitter.instr("la   r3, 0xFFFFFFFF");
			_emitter.instr("mscan r1, r2, r3");        // r1 = the terminator
			_emitter.instr("sub  r0, r1, r0");         // length = end - start
			_emitter.instr("ret");
		}

		// The memchr scan's routine: the index of the first byte equal to `c`, or n. `mscan` leaves the
		// pointer at the byte, or past the block when there is none, so both answers are that pointer
		// less the base.
		if (_usesMemchrIndex)
		{
			_emitter.raw("// emitted because a memchr-style scan was recognized (docs/14 O15)");
			_emitter.label("__cc_memchr_index");
			_emitter.instr("mov  r7, r0");             // the base, for the index at the end
			_emitter.instr("ifle r2, 0, .cmi_none");   // a signed count <= 0 matches nothing
			_emitter.instr("mscan r0, r1, r2");        // the low byte of r1; r0 at it, or past the block
			_emitter.localLabel("cmi_none");
			_emitter.instr("sub  r0, r0, r7");         // index of the match, or n (0 when nothing was scanned)
			_emitter.instr("ret");
		}

		// A 64-bit division or remainder (ir_builder.cpp's lowerWideArithmetic, F3.2). The operands
		// travel as pointers because the 64-bit calling convention is F3.4: `dest` receives the
		// quotient at +0 and the remainder at +8, `aPtr`/`bPtr` are the two operands' addresses and
		// r3 is 1 for a signed operation. The restoring shift-subtract loop runs 64 times; `adc`/
		// `sbc` do the cross-word shift and subtract, which is exactly what the ISA's carry chain is
		// for. Signed operands are turned into magnitudes first and the signs reapplied at the end,
		// C's truncating division (`-7 / 2 == -3`, `-7 % 2 == -1`). A zero divisor (undefined in C)
		// stores zero rather than looping. r8-r11 are callee-saved, so `pushm`/`popm` bracket them.
		if (_usesDiv64)
		{
			_emitter.raw("// emitted because a 64-bit division or remainder was lowered (docs/14 F3.2)");
			_emitter.label("__cc_div64");
			_emitter.instr(std::format("pushm 0x{:04X}", kDiv64SaveMask)); // r8-r11, the callee-saved half
			_emitter.instr("ldr  r8,  [r1]");          // dividend low
			_emitter.instr("ldr  r9,  [r1 + 4]");      // dividend high
			_emitter.instr("ldr  r10, [r2]");          // divisor low
			_emitter.instr("ldr  r11, [r2 + 4]");      // divisor high
			_emitter.instr("or   r12, r10, r11");
			_emitter.instr("ifne r12, 0, .div_nonzero");
			_emitter.instr("li   r12, 0");             // /0 is UB in C: store zero instead of looping
			_emitter.instr("str  [r0], r12");
			_emitter.instr("str  [r0 + 4], r12");
			_emitter.instr("str  [r0 + 8], r12");
			_emitter.instr("str  [r0 + 12], r12");
			_emitter.instr(std::format("popm 0x{:04X}", kDiv64SaveMask));
			_emitter.instr("ret");
			_emitter.localLabel("div_nonzero");
			_emitter.instr("mov  r1, r3");             // r1 = isSigned, captured before r3 is reused
			_emitter.instr("li   r2, 0");              // negRem
			_emitter.instr("li   r3, 0");              // negQuot
			_emitter.instr("ifeq r1, 0, .div_mag");
			_emitter.instr("shr  r2, r9, 31");         // negRem = sign of the dividend
			_emitter.instr("xor  r3, r9, r11");
			_emitter.instr("shr  r3, r3, 31");         // negQuot = sign(a) ^ sign(b)
			_emitter.instr("ifge r9, 0, .div_dsign");  // dividend >= 0: leave it
			_emitter.instr("not  r8, r8");
			_emitter.instr("not  r9, r9");
			_emitter.instr("add  r8, r8, 1");
			_emitter.instr("adc  r9, r9, 0");
			_emitter.localLabel("div_dsign");
			_emitter.instr("ifge r11, 0, .div_mag");   // divisor >= 0: leave it
			_emitter.instr("not  r10, r10");
			_emitter.instr("not  r11, r11");
			_emitter.instr("add  r10, r10, 1");
			_emitter.instr("adc  r11, r11, 0");
			_emitter.localLabel("div_mag");
			_emitter.instr("li   r4, 0");              // remainder = 0
			_emitter.instr("li   r5, 0");
			_emitter.instr("li   r6, 0");              // quotient = 0
			_emitter.instr("li   r7, 0");
			_emitter.instr("li   r12, 64");
			_emitter.localLabel("div_loop");
			_emitter.instr("shr  r1, r9, 31");         // the dividend's top bit
			_emitter.instr("shl  r4, r4, 1");          // remainder <<= 1, carry = its old bit 31
			_emitter.instr("adc  r5, r5, r5");
			_emitter.instr("or   r4, r4, r1");         // bring the dividend's top bit in
			_emitter.instr("shl  r8, r8, 1");          // dividend <<= 1
			_emitter.instr("adc  r9, r9, r9");
			_emitter.instr("shl  r6, r6, 1");          // quotient <<= 1
			_emitter.instr("adc  r7, r7, r7");
			_emitter.instr("ifbl r5, r11, .div_nosub");  // remainder < divisor, unsigned, high first
			_emitter.instr("ifab r5, r11, .div_sub");
			_emitter.instr("ifbl r4, r10, .div_nosub");
			_emitter.localLabel("div_sub");
			_emitter.instr("sub  r4, r4, r10");
			_emitter.instr("sbc  r5, r5, r11");
			_emitter.instr("or   r6, r6, 1");
			_emitter.localLabel("div_nosub");
			_emitter.instr("sub  r12, r12, 1");
			_emitter.instr("ifne r12, 0, .div_loop");
			_emitter.instr("ifeq r3, 0, .div_qpos");   // a quotient is negative only when one sign was negative
			_emitter.instr("not  r6, r6");
			_emitter.instr("not  r7, r7");
			_emitter.instr("add  r6, r6, 1");
			_emitter.instr("adc  r7, r7, 0");
			_emitter.localLabel("div_qpos");
			_emitter.instr("ifeq r2, 0, .div_rpos");   // the remainder takes the dividend's sign
			_emitter.instr("not  r4, r4");
			_emitter.instr("not  r5, r5");
			_emitter.instr("add  r4, r4, 1");
			_emitter.instr("adc  r5, r5, 0");
			_emitter.localLabel("div_rpos");
			_emitter.instr("str  [r0], r6");
			_emitter.instr("str  [r0 + 4], r7");
			_emitter.instr("str  [r0 + 8], r4");
			_emitter.instr("str  [r0 + 12], r5");
			_emitter.instr(std::format("popm 0x{:04X}", kDiv64SaveMask));
			_emitter.instr("ret");
		}
	}

	void CodeGen::emitJumpTables()
	{
		// A second `@rodata` block, after every function's `@text`, so the block labels each entry
		// names are all defined by now. The assembler keeps a per-section offset, so re-entering
		// `.rodata` continues where the first block left off rather than overlapping it.
		for (const JumpTable& table : _jumpTables)
		{
			std::string entries;
			for (usize i = 0; i < table.entries.size(); ++i)
			{
				if (i)
					entries += ", ";
				entries += table.entries[i];
			}
			_emitter.raw(std::format("let {}: u32[{}] = [{}]", table.label, table.entries.size(), entries));
		}
	}

	// ---- entry point ------------------------------------------------------------------------------

	void CodeGen::checkSymbolNames(const TranslationUnit& unit, const IrModule& module)
	{
		// A C name that is a word CeresASM reserves is not an error any more: the symbol is written `__c_` and the
		// name (see casmSymbolName). What can still clash is the prefix itself - a symbol the program calls
		// `__c_at` while another is called `at` would be the same one in the assembly - and an explicit label
		// that is itself a reserved word, which nothing renames.
		auto check = [&](std::string_view name, support::SourceLocation location, std::string_view what)
		{
			if (name.starts_with("__c_") && isReservedCasmWord(name.substr(4)))
			{
				_diagnostics.error(DiagId::ReservedNamePrefix, location,
					"'{}' cannot be used as the name of a {}: '__c_' followed by a reserved word of CeresASM is what '{}' is written as "
					"in the generated assembly (see docs/07-CASM-Interop.md)", name, what, name.substr(4));
			}
		};

		for (Decl* decl : unit.decls())
		{
			if (!decl || decl->name().empty())
				continue;
			const bool isFunction = dynamic_cast<FunctionDecl*>(decl) != nullptr;
			if (isFunction || dynamic_cast<VarDecl*>(decl))
				check(decl->name(), decl->location(), isFunction ? "function" : "global variable");
			if (decl->asmLabel() && isReservedCasmWord(decl->asmLabel().view()))
			{
				_diagnostics.error(DiagId::ReservedCasmWord, decl->location(),
					"'{}' cannot be the asm label of '{}': it is a reserved word in CeresASM", decl->asmLabel().view(), decl->name());
			}
		}
		// A `static` local becomes a file-scope CASM symbol too, under a name that already carries
		// its function's, so it cannot meet a reserved word or the prefix by accident.
		for (const IrStaticLocal& local : module.staticLocals())
			check(local.name, local.decl->location(), "static local variable");
	}

	std::vector<ExternalDeclaration> CodeGen::collectExternalDeclarations(const TranslationUnit& unit) const
	{
		std::vector<ExternalDeclaration> declarations;
		const AsmLabelMap labels = asmLabelsOf(unit);
		for (Decl* decl : unit.decls())
		{
			if (auto* function = dynamic_cast<FunctionDecl*>(decl))
			{
				// Prototypes count as much as definitions: a unit that only declares `int f(int);`
				// still needs every OTHER unit to agree on the name, and whichever one defines it
				// will contribute the same entry. The driver keeps one copy.
				if (!function->hasExternalLinkage())
					continue;
				declarations.push_back(ExternalDeclaration{ casmSymbolName(labels, function->name()), true, function->isDefinition(), false, "@text", {} });
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
			// `extern struct S x;` with S only declared, or `extern int t[];` with no size: the type has no
			// size here, so the shape would come out as `u32[0]` and the assembler rejects a zero-length array. Nothing is stored
			// here (this is a declaration), and the definition - which has a real size - replaces this
			// entry when the driver merges the units, so any non-empty placeholder will do.
			if (typeText.find("[0]") != std::string::npos)
				typeText = "u8[1]";
			declarations.push_back(ExternalDeclaration{ casmSymbolName(labels, variable->name()), false, !variable->isExternDeclaration(),
				variable->initializer() != nullptr, std::move(section), std::move(typeText) });
		}
		return declarations;
	}

	std::string CodeGen::generate(const TranslationUnit& unit, const IrModule& module)
	{
		_asmLabels = asmLabelsOf(unit);
		checkSymbolNames(unit, module);

		// Does this unit see a `void exit(int)`? Then `main` can hand its status to it (see the Return
		// case of generateInstr). Looked for by shape, not just by name: a program's own `exit`
		// with another meaning must not be called with whatever main happened to return.
		_mainCallsExit = false;
		for (Decl* decl : unit.decls())
		{
			auto* function = dynamic_cast<FunctionDecl*>(decl);
			if (function && function->name() == "exit" && function->hasExternalLinkage() &&
				function->params().size() == 1 && function->returnType() && function->returnType()->isVoid())
			{
				_mainCallsExit = true;
				break;
			}
		}

		// String literals that appear only in static initializers get their .rodata labels minted
		// while the globals are emitted below; start each compilation from an empty table.
		_initializerStringNames.clear();
		_initializerStringOrder.clear();
		_nextInitializerStringId = 0;
		_jumpTables.clear();
		_nextJumpTableId = 0;
		_usesMemset = false;
		_usesMemcpy = false;
		_usesStrlen = false;
		_usesMemchrIndex = false;
		_usesDiv64 = false;

		// Before every section. `interrupt N: handler` is a top-level declaration that emits neither
		// code nor data - only a binding the linker resolves and the loader applies before the
		// program's first instruction (CeresASM 26-Interrupt-Vector-Binding.md) - so it is valid
		// wherever `const` is, and putting it first makes the .casm say what it answers before it says
		// what it does.
		bool wroteAnyVector = false;
		for (Decl* decl : unit.decls())
		{
			auto* vector = dynamic_cast<ast::InterruptVectorDecl*>(decl);
			if (!vector)
				continue;
			// The number is written out rather than passed through as the C source spelled it: an enum
			// constant or a macro means nothing to the assembler, and sema has already folded it.
			_emitter.raw(std::format("interrupt {}: {}", vector->resolvedNumber(), casmName(vector->name())));
			wroteAnyVector = true;
		}
		if (wroteAnyVector)
			_emitter.blank();

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
		_staticLocalSymbols.clear();
		for (const IrStaticLocal& local : module.staticLocals())
		{
			staticLocalNames.emplace(local.decl, local.name);
			_staticLocalSymbols.emplace(local.decl->name(), local.name);
		}
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
				std::string symbolName = isStaticLocal ? std::string(it->second) : casmName(g->name());
				// A `static` of either kind has internal linkage and is not published.
				bool exported = !isStaticLocal && g->storageClass() != ast::StorageClass::Static;
				generateGlobal(*g, symbolName, exported);
			}
			_emitter.blank();
		};

		emitBucket("@data", dataGlobals);
		emitBucket("@bss", bssGlobals);
		if (!rodataGlobals.empty() || !module.stringLiterals().empty() || !_initializerStringOrder.empty())
		{
			_emitter.raw("@rodata");
			for (const VarDecl* g : rodataGlobals)
			{
				auto it = staticLocalNames.find(g);
				bool isStaticLocal = it != staticLocalNames.end();
				generateGlobal(*g, isStaticLocal ? std::string(it->second) : casmName(g->name()),
					!isStaticLocal && g->storageClass() != ast::StorageClass::Static);
			}
			generateStringLiterals(module);
			emitInitializerStringLiterals();
			_emitter.blank();
		}

		_emitter.raw("@text");
		for (const auto& function : module.functions())
		{
			const FunctionDecl* decl = findFunctionDecl(unit, function->name());
			if (decl)
				generateFunction(*decl, *function);
			else
				_diagnostics.error(DiagId::MissingDeclarationForFunction, {}, "internal error: no declaration found for generated function '{}'", function->name());
		}

		// The compiler's own runtime routines, if any call site asked for one - still inside `@text`,
		// after every function.
		emitCarriedRoutines();

		// Jump tables go in their own `@rodata` block after all the code: their entries name block
		// labels, which only exist once the function they belong to has been emitted.
		if (!_jumpTables.empty())
		{
			_emitter.blank();
			_emitter.raw("@rodata");
			emitJumpTables();
			_emitter.blank();
		}

		return _emitter.take();
	}
}
