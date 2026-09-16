#pragma once

#include <ceresc/support/types.h>
#include <ceresc/support/source_location.h>
#include <string_view>
#include <type_traits>
#include <variant>

// IrInstr - the three-address-code opcode set: Const, BinOp, UnOp, Cmp, Copy, FrameAddr,
// GlobalAddr, Load, Store, Param, Call, VaStart, Jump, CondJump, Return.
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
		Const, BinOp, UnOp, Cmp, Copy, FrameAddr, GlobalAddr, Load, Store, Param, Call, VaStart, Jump, CondJump, Return
	};

	// Binary arithmetic/bitwise ops. Shr and Sar are two distinct opcodes - not one "Shr" opcode
	// plus a signedness flag - because CASM itself has two distinct mnemonics for them (sar/shr,
	// §10): IrBuilder, which already knows an operand's signedness from sema's own resolved Type,
	// picks the right one at lowering time instead of leaving that decision for codegen to
	// rediscover.
	enum class IrBinOp : u8 { Add, Sub, Mul, Div, Mod, And, Or, Xor, Shl, Shr, Sar };

	// IntToFloat/FloatToInt exist because casting across the int/float line is a real runtime
	// operation (itof/iitof/ftoi/ftoii, §10): there is no bit-pattern trick that reinterprets an i32
	// as an f32. IrBuilder emits one of these two whenever a conversion's source and target types
	// disagree on isFloat(); codegen is what picks the exact mnemonic from IrUnOpPayload::isUnsigned.
	//
	// Narrow is an int-to-int WIDTH change - `(char)x`, or storing an int into a `short`. This used
	// to be assumed free, on the theory that the memSize of a later Load/Store would realize it. It
	// is not: a value of narrow type can be produced, used and consumed without ever touching
	// memory (the optimizer's whole job is to arrange exactly that), and `(int)(char)0x101` has to
	// be 1 whether or not a store happened in between. IrUnOpPayload::narrowSize says how wide the
	// result is and isUnsigned whether the spare bits are filled with zeros or with the sign.
	//
	// ToBool is the int-to-bool conversion, which C defines as "zero stays zero, anything else
	// becomes one" rather than as a truncation - `(bool)256` is `true`, not `false`.
	//
	// Together these three give the IR one invariant the back end and the optimizer both rely on: a
	// value whose C type is narrow is ALWAYS already in its narrowed representation. That is what
	// makes forwarding a stored value straight to a load sound - the store's value is exactly what
	// the load would have read back.
	enum class IrUnOp : u8 { Neg, Not, LogicalNot, IntToFloat, FloatToInt, Narrow, ToBool };

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
		bool isFloat = false;    // selects the F-prefixed opcode (FADD/FSUB/FMUL/FDIV, §10) - only
		                         // ever true for Add/Sub/Mul/Div: sema requires integer operands for
		                         // Mod/And/Or/Xor/Shl/Shr (sema.cpp's isIntegerType() checks), so a
		                         // float operand can never reach this opcode with isFloat and one of
		                         // those combined
		IrValue lhs, rhs;
	};

	struct IrUnOpPayload
	{
		IrValue result;
		IrUnOp op = IrUnOp::Neg;
		bool isFloat = false;    // Neg only: selects FNEG over `neg` (imul rd, rs, -1, §10) - Not/
		                         // LogicalNot never see a float operand (sema requires an integer/
		                         // scalar one)
		bool isUnsigned = false; // IntToFloat/FloatToInt: the INTEGER side's signedness - which real
		                         // opcode to pick (itof/iitof, ftoi/ftoii, §10). Narrow: whether the
		                         // bits above the result's width are filled with zeros (`and`) or
		                         // with the sign (`sxtb`/`sxth`)
		IrMemSize narrowSize = IrMemSize::Word; // Narrow only: Byte or Half. Word would be a no-op,
		                                        // and IrBuilder never emits one
		IrValue operand;
	};

	struct IrCmpPayload
	{
		IrValue result;
		IrCmpPredicate predicate = IrCmpPredicate::Eq;
		bool isUnsigned = false; // only meaningful for Lt/Le/Gt/Ge - Eq/Ne are identical either way
		bool isFloat = false;    // selects FCMP - see §10's note that FCMP already leaves the result
		                         // readable through the UNSIGNED branch family (Carry from `fs < ft`),
		                         // so isFloat and isUnsigned are independent bits, not one implying
		                         // the other
		IrValue lhs, rhs;
	};

	struct IrCopyPayload
	{
		IrValue result;
		bool isFloat = false; // selects `mov fd, fs` over `mov rd, rs` - see visit(TernaryExpr&),
		                      // the only place this opcode is emitted (ir_builder.cpp)
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
		bool isFloat = false;  // selects `ldr fd, [...]` (always Word-sized, §10's FLDR) over the
		                       // integer load family chosen by `size`
		bool isSigned = false; // Byte/Half only: selects `ldrsb`/`ldrsh` over `ldrb`/`ldrh`. A
		                       // `signed char` read back from memory has to come out negative, and
		                       // the zero-extending forms are the reason it used not to. Word loads
		                       // already fill the register, and a float load has no such choice

		// The object being read is `volatile`, so this load is observable and no pass may remove
		// it, reorder it, or answer it from a value some earlier store is known to have left in
		// memory. IrLocalSlot::isVolatile says the same thing about a whole local; this says it
		// about ONE access, which is the only form the fact can take when the object is reached
		// through a pointer (`volatile int* p` qualifies the pointee, not the pointer, so there is
		// no local slot to hang it on).
		bool isVolatile = false;
		IrValue address;
	};

	struct IrStorePayload
	{
		IrMemSize size = IrMemSize::Word;
		bool isFloat = false; // selects `str [...], fs` (FSTR) over the integer store family
		bool isVolatile = false; // see IrLoadPayload::isVolatile - a volatile store is observable
		                         // even when nothing ever reads the object back
		IrValue address;
		IrValue value;
	};

	struct IrParamPayload
	{
		IrValue value;   // queues one outgoing argument before the next Call - see §9
		bool isFloat = false; // the argument expression's OWN type - codegen routes it through
		                       // f0-f3/the outgoing float slots instead of r0-r3 when set. Reflects
		                       // the caller's argument, not the callee's declared parameter type:
		                       // IrBuilder does not resolve a callee's signature (it looks up a Call's
		                       // target by name only, see the header comment on IrBuilder's contract),
		                       // so - unlike a plain assignment or initializer - a call passing a
		                       // literal of the "wrong" arithmetic family to a scalar parameter is not
		                       // converted here; write the matching literal/variable type at the call site.

		// This argument sits in the callee's variadic tail (past its last declared parameter), so
		// it is passed on the stack no matter which bank `isFloat` names and no matter how many
		// argument registers are still free. That is the whole of the variadic convention on the
		// caller's side, and it is what lets the callee find these arguments at all: it knows how
		// many stack words its own FIXED parameters consumed, so the next word is where the
		// variadic ones begin - which a register-passed argument could never be.
		// See docs/09-Variadic-Convention.md.
		bool isVariadicArg = false;
	};

	struct IrCallPayload
	{
		IrValue result;   // only meaningful when hasResult is true (the callee's return type is not void)
		bool hasResult = false;
		bool isFloat = false; // meaningful only when hasResult: the result comes back in f0/ret0 (§10)
		std::string_view callee;
		u32 argCount = 0; // number of Params queued since the previous Call - see §9
	};

	// The address of the first argument in THIS function's variadic tail - the one thing a variadic
	// body cannot compute for itself, because it lives in the caller's frame rather than in any
	// local slot. Codegen resolves it from the function's own signature (how many stack words its
	// fixed parameters consumed), so the instruction carries no operand at all.
	//
	// Everything else va_list does is ordinary pointer work on the value this produces: va_arg is a
	// Load plus an Add, va_copy is a Copy, va_end is nothing. Only this one step needs the back end.
	struct IrVaStartPayload
	{
		IrValue result;
	};

	struct IrJumpPayload
	{
		BasicBlock* target = nullptr;
	};

	struct IrCondJumpPayload
	{
		IrCmpPredicate predicate = IrCmpPredicate::Eq;
		bool isUnsigned = false;
		bool isFloat = false;
		IrValue lhs, rhs;
		BasicBlock* trueTarget = nullptr;
		BasicBlock* falseTarget = nullptr;
	};

	struct IrReturnPayload
	{
		bool hasValue = false;
		bool isFloat = false; // meaningful only when hasValue: `ret0`/f0 (§10) - the function's own
		                      // declared return type, not necessarily the return expression's raw
		                      // type (IrBuilder converts a mismatched one first, see visit(ReturnStmt&))
		IrValue value;
	};

	using IrInstrPayload = std::variant<
		IrConstPayload, IrBinOpPayload, IrUnOpPayload, IrCmpPayload, IrCopyPayload,
		IrFrameAddrPayload, IrGlobalAddrPayload, IrLoadPayload, IrStorePayload, IrParamPayload,
		IrCallPayload, IrVaStartPayload, IrJumpPayload, IrCondJumpPayload, IrReturnPayload>;
	// Declaration order here must match IrOpcode's own order exactly - opcode() below derives the
	// opcode from the variant's index() instead of storing a second, redundant tag. The size check
	// alone only pins the *count*: swapping two payload types (e.g. Load/Store), or adding an
	// IrOpcode enumerator without a matching payload, would keep the count at 14 while silently
	// remapping opcode() and every switch in ir_printer.cpp/ir_function.cpp to the wrong payload -
	// so each alternative's *position* is pinned individually too, not just the total.
	static_assert(std::variant_size_v<IrInstrPayload> == 15, "IrInstrPayload must have exactly one alternative per IrOpcode");
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
	static_assert(OpcodeMapsToPayload<IrOpcode::VaStart, IrVaStartPayload>);
	static_assert(OpcodeMapsToPayload<IrOpcode::Jump, IrJumpPayload>);
	static_assert(OpcodeMapsToPayload<IrOpcode::CondJump, IrCondJumpPayload>);
	static_assert(OpcodeMapsToPayload<IrOpcode::Return, IrReturnPayload>);

	// Declared before IrInstr so resultOf()/forEachOperand() below can be defined right after it -
	// see their own comment for why they live here rather than in each consumer.
	class IrInstr;

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

	// ---- reading an instruction without switching on its opcode ------------------------------------
	//
	// Which payload field is a RESULT and which is an OPERAND is a fact about this opcode table and
	// nothing else, so it is answered once, here, rather than separately inside every consumer that
	// needs it: ir_optimizer.cpp (use counting for dead-code elimination, remapping for inlining)
	// and libs/codegen's value_placement.cpp (liveness) would otherwise each carry their own copy
	// of the same fourteen-case switch, and a payload gaining a field would have to be remembered in
	// every one of them.

	// The temporary `instr` defines, or an invalid IrValue for an opcode that defines none
	// (Store/Param/Jump/CondJump/Return, and a Call whose callee returns void).
	inline IrValue resultOf(const IrInstr& instr) noexcept
	{
		switch (instr.opcode())
		{
			case IrOpcode::Const:      return instr.as<IrConstPayload>().result;
			case IrOpcode::BinOp:      return instr.as<IrBinOpPayload>().result;
			case IrOpcode::UnOp:       return instr.as<IrUnOpPayload>().result;
			case IrOpcode::Cmp:        return instr.as<IrCmpPayload>().result;
			case IrOpcode::Copy:       return instr.as<IrCopyPayload>().result;
			case IrOpcode::FrameAddr:  return instr.as<IrFrameAddrPayload>().result;
			case IrOpcode::GlobalAddr: return instr.as<IrGlobalAddrPayload>().result;
			case IrOpcode::Load:       return instr.as<IrLoadPayload>().result;
			case IrOpcode::VaStart:    return instr.as<IrVaStartPayload>().result;
			case IrOpcode::Call:
			{
				const IrCallPayload& payload = instr.as<IrCallPayload>();
				return payload.hasResult ? payload.result : IrValue{};
			}
			default: return IrValue{};
		}
	}

	// Calls `fn(IrValue)` once per temporary `instr` READS, in operand order.
	template <typename F>
	void forEachOperand(const IrInstr& instr, F&& fn)
	{
		switch (instr.opcode())
		{
			case IrOpcode::BinOp:
			{
				const IrBinOpPayload& p = instr.as<IrBinOpPayload>();
				fn(p.lhs); fn(p.rhs);
				break;
			}
			case IrOpcode::UnOp: fn(instr.as<IrUnOpPayload>().operand); break;
			case IrOpcode::Cmp:
			{
				const IrCmpPayload& p = instr.as<IrCmpPayload>();
				fn(p.lhs); fn(p.rhs);
				break;
			}
			case IrOpcode::Copy: fn(instr.as<IrCopyPayload>().source); break;
			case IrOpcode::Load: fn(instr.as<IrLoadPayload>().address); break;
			case IrOpcode::Store:
			{
				const IrStorePayload& p = instr.as<IrStorePayload>();
				fn(p.address); fn(p.value);
				break;
			}
			case IrOpcode::Param: fn(instr.as<IrParamPayload>().value); break;
			case IrOpcode::CondJump:
			{
				const IrCondJumpPayload& p = instr.as<IrCondJumpPayload>();
				fn(p.lhs); fn(p.rhs);
				break;
			}
			case IrOpcode::Return:
			{
				const IrReturnPayload& p = instr.as<IrReturnPayload>();
				if (p.hasValue)
					fn(p.value);
				break;
			}
			default: break; // Const/FrameAddr/GlobalAddr/Call/Jump read no temporary
		}
	}
}
