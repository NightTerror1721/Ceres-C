#pragma once

#include <ceresc/support/types.h>
#include <ceresc/support/source_location.h>
#include <string_view>
#include <type_traits>
#include <variant>

// IrInstr - the three-address-code opcode set: Const, BinOp, UnOp, Cmp, Copy, FrameAddr,
// GlobalAddr, Load, Store, Param, Call, Jump, CondJump, Return.
//
// Deliberately not SSA and with no dominator tree - a BasicBlock (ir_function.h) is a flat, linear
// list of these, terminated by a jump or a return. There is no setcc-equivalent in the CASM ISA: a
// comparison used as a value gets synthesized by codegen (libs/codegen) as a short branch, not
// lowered here. See the architecture plan, §9.
//
// Every operand and result is an IrValue - an unlimited virtual temporary (%t0, %t1, ...), never a
// physical register (libs/codegen, not this library, is the only place that thinks about r4 or
// at). A literal is never embedded directly into another instruction's operand: it is always
// materialized first by its own Const instruction, and every other opcode's operands are always
// IrValue references to an already-defined temp - "does this temp trace back to a Const" is
// exactly the kind of peephole codegen (§10) does on its own (e.g. to fold a constant BinOp operand
// into addi/subi), not something this library needs to special-case.
//
// Each opcode's operands are modeled as their own small payload struct, one per opcode, joined by
// a std::variant - the same closed-payload idiom Type::PayloadType (libs/ast/type.h) and
// TokenValue (libs/lexer/token.h) already use, so e.g. a Load's fields and a Call's fields can
// never be confused with each other by construction, instead of one flat struct with a dozen
// "only meaningful for some opcodes" fields.
//
// Implemented in Fase 5 of the phased plan (§13). IrPrinter (--emit-ir) ships alongside it.

namespace ceresc::ir
{
	class BasicBlock;

	// A virtual temporary/register - see the header comment above. Ids are handed out by
	// IrFunction::newTemp(), scoped to a single function (temp numbering restarts at 0 for every
	// function - §9 frames "unlimited virtual temporaries" per function, not per translation unit),
	// so real ids start at 0 - a default-constructed IrValue{} is deliberately NOT id 0 (a
	// genuinely valid first temp) but InvalidId, so the "no value" sentinel this file's payloads
	// use throughout (a no-result Call, a valueless Return, IrBuilder's own _lastValue before
	// anything has set it) can never be mistaken for one. isValid() names the check explicitly
	// rather than leaving every consumer to compare against InvalidId by hand.
	struct IrValue
	{
		static constexpr u32 InvalidId = ~0u;

		u32 id = InvalidId;

		constexpr bool operator==(const IrValue&) const noexcept = default;
		constexpr bool isValid() const noexcept { return id != InvalidId; }
	};

	enum class IrOpcode : u8
	{
		Const, BinOp, UnOp, Cmp, Copy, FrameAddr, GlobalAddr, Load, Store, Param, Call, Jump, CondJump, Return
	};

	// Binary arithmetic/bitwise ops. Shr and Sar are two distinct opcodes - not one "Shr" opcode
	// plus a signedness flag - because CASM itself has two distinct mnemonics for them (sar/shr,
	// §10): IrBuilder, which already knows an operand's signedness from sema's own resolved Type,
	// picks the right one at lowering time instead of leaving that decision for codegen to
	// rediscover.
	enum class IrBinOp : u8 { Add, Sub, Mul, Div, Mod, And, Or, Xor, Shl, Shr, Sar };

	enum class IrUnOp : u8 { Neg, Not, LogicalNot };

	// Shared by Cmp and CondJump - "exactamente uno de los seis predicados con/sin signo" (§9).
	enum class IrCmpPredicate : u8 { Eq, Ne, Lt, Le, Gt, Ge };

	enum class IrMemSize : u8 { Byte, Half, Word };

	// The narrowest IrMemSize that can hold `sizeInBytes` bytes (Type::sizeInBytes(), libs/ast) -
	// 1 -> Byte, 2 -> Half, anything else (4, the ABI's native register width, or an unexpected/
	// composite size - see ir_builder.cpp's own note on whole-struct loads and stores, which this
	// phase does not model precisely) -> Word.
	constexpr IrMemSize irMemSizeForBytes(u32 sizeInBytes) noexcept
	{
		switch (sizeInBytes)
		{
			case 1: return IrMemSize::Byte;
			case 2: return IrMemSize::Half;
			default: return IrMemSize::Word;
		}
	}

	// ---- opcode payloads ---------------------------------------------------------------------------
	// Every payload that defines a temp carries its own `result` field (rather than hoisting one
	// top-level "result" onto IrInstr) so an opcode with no result (Store/Param/Jump/CondJump/
	// Return) simply has no such field to misuse - see the header comment above.

	struct IrConstPayload
	{
		IrValue result;
		i64 intValue = 0;     // Const covers int/char/bool literals, widened to i64
		f32 floatValue = 0.0f;
		bool isFloat = false; // selects intValue vs floatValue
	};

	struct IrBinOpPayload
	{
		IrValue result;
		IrBinOp op = IrBinOp::Add;
		bool isUnsigned = false; // only meaningful for Mul/Div/Mod - see §10's IR->CASM mapping table
		IrValue lhs, rhs;
	};

	struct IrUnOpPayload
	{
		IrValue result;
		IrUnOp op = IrUnOp::Neg;
		IrValue operand;
	};

	struct IrCmpPayload
	{
		IrValue result;
		IrCmpPredicate predicate = IrCmpPredicate::Eq;
		bool isUnsigned = false; // only meaningful for Lt/Le/Gt/Ge - Eq/Ne are identical either way
		IrValue lhs, rhs;
	};

	struct IrCopyPayload
	{
		IrValue result;
		IrValue source;
	};

	struct IrFrameAddrPayload
	{
		IrValue result;
		u32 localIndex = 0; // see IrFunction's own note on local slot numbering (ir_function.h)
	};

	struct IrGlobalAddrPayload
	{
		IrValue result;
		std::string_view name; // a global variable's own name, or a synthesized string-literal label (IrModule::stringLiterals())
	};

	struct IrLoadPayload
	{
		IrValue result;
		IrMemSize size = IrMemSize::Word;
		IrValue address;
	};

	struct IrStorePayload
	{
		IrMemSize size = IrMemSize::Word;
		IrValue address;
		IrValue value;
	};

	struct IrParamPayload
	{
		IrValue value; // queues one outgoing argument before the next Call - see §9
	};

	struct IrCallPayload
	{
		IrValue result;   // only meaningful when hasResult is true (the callee's return type is not void)
		bool hasResult = false;
		std::string_view callee;
		u32 argCount = 0; // number of Params queued since the previous Call - see §9
	};

	struct IrJumpPayload
	{
		BasicBlock* target = nullptr;
	};

	struct IrCondJumpPayload
	{
		IrCmpPredicate predicate = IrCmpPredicate::Eq;
		bool isUnsigned = false;
		IrValue lhs, rhs;
		BasicBlock* trueTarget = nullptr;
		BasicBlock* falseTarget = nullptr;
	};

	struct IrReturnPayload
	{
		bool hasValue = false;
		IrValue value;
	};

	using IrInstrPayload = std::variant<
		IrConstPayload, IrBinOpPayload, IrUnOpPayload, IrCmpPayload, IrCopyPayload,
		IrFrameAddrPayload, IrGlobalAddrPayload, IrLoadPayload, IrStorePayload, IrParamPayload,
		IrCallPayload, IrJumpPayload, IrCondJumpPayload, IrReturnPayload>;
	// Declaration order here must match IrOpcode's own order exactly - opcode() below derives the
	// opcode from the variant's index() instead of storing a second, redundant tag. The size check
	// alone only pins the *count*: swapping two payload types (e.g. Load/Store), or adding an
	// IrOpcode enumerator without a matching payload, would keep the count at 14 while silently
	// remapping opcode() and every switch in ir_printer.cpp/ir_function.cpp to the wrong payload -
	// so each alternative's *position* is pinned individually too, not just the total.
	static_assert(std::variant_size_v<IrInstrPayload> == 14, "IrInstrPayload must have exactly one alternative per IrOpcode");
	template <IrOpcode Op, typename Payload>
	concept OpcodeMapsToPayload = std::is_same_v<std::variant_alternative_t<static_cast<usize>(Op), IrInstrPayload>, Payload>;
	static_assert(OpcodeMapsToPayload<IrOpcode::Const, IrConstPayload>);
	static_assert(OpcodeMapsToPayload<IrOpcode::BinOp, IrBinOpPayload>);
	static_assert(OpcodeMapsToPayload<IrOpcode::UnOp, IrUnOpPayload>);
	static_assert(OpcodeMapsToPayload<IrOpcode::Cmp, IrCmpPayload>);
	static_assert(OpcodeMapsToPayload<IrOpcode::Copy, IrCopyPayload>);
	static_assert(OpcodeMapsToPayload<IrOpcode::FrameAddr, IrFrameAddrPayload>);
	static_assert(OpcodeMapsToPayload<IrOpcode::GlobalAddr, IrGlobalAddrPayload>);
	static_assert(OpcodeMapsToPayload<IrOpcode::Load, IrLoadPayload>);
	static_assert(OpcodeMapsToPayload<IrOpcode::Store, IrStorePayload>);
	static_assert(OpcodeMapsToPayload<IrOpcode::Param, IrParamPayload>);
	static_assert(OpcodeMapsToPayload<IrOpcode::Call, IrCallPayload>);
	static_assert(OpcodeMapsToPayload<IrOpcode::Jump, IrJumpPayload>);
	static_assert(OpcodeMapsToPayload<IrOpcode::CondJump, IrCondJumpPayload>);
	static_assert(OpcodeMapsToPayload<IrOpcode::Return, IrReturnPayload>);

	class IrInstr
	{
	private:
		support::SourceLocation _location;
		IrInstrPayload _payload;

	public:
		IrInstr(support::SourceLocation location, IrInstrPayload payload) noexcept :
			_location(location), _payload(std::move(payload))
		{}

	public:
		support::SourceLocation location() const noexcept { return _location; }
		IrOpcode opcode() const noexcept { return static_cast<IrOpcode>(_payload.index()); }

		// `T` must be the payload type matching opcode() - see the table above (e.g. IrBinOpPayload
		// for IrOpcode::BinOp). Mismatching them is a programming error in this library itself, not
		// something a well-formed IrModule can trigger, so this deliberately does not soft-fail.
		template <typename T>
		const T& as() const noexcept { return std::get<T>(_payload); }
	};
	// Every IrInstr is Arena-allocated (Arena::create<T>, arena.h), which requires this - see §4's
	// "todos los nodos del AST y las instrucciones IR se reservan aquí" and the identical
	// static_assert every arena-allocated AST node type carries (expr.h/stmt.h/decl.h/type.h). Kept
	// true only because every IrInstrPayload alternative is itself a plain struct of IrValue/enum/
	// bool/u32/string_view/pointer fields - a payload gaining an owning member like std::string or
	// std::vector (the exact trap FieldDecl/Param avoid by storing a {pointer, count} view instead
	// of owning storage, see expr.h's header comment) would silently break this.
	static_assert(TriviallyDestructible<IrInstr>, "IrInstr must be trivially destructible because it is allocated in an arena and never deleted individually");
}
