#pragma once

#include <ceresc/support/source_location.h>
#include <ceresc/support/string_pool.h>
#include "type.h"
#include <span>
#include <string_view>
#include <variant>

// Expr hierarchy: IntLiteralExpr, FloatLiteralExpr, CharLiteralExpr, BoolLiteralExpr,
// StringLiteralExpr, NameExpr, CallExpr, UnaryExpr, BinaryExpr, AssignExpr, IndexExpr, MemberExpr,
// CastExpr, SizeofExpr, AlignofExpr, VaExpr, TernaryExpr, InitListExpr.
//
// InitListExpr is the odd one out: `{ 1, 2, 3 }` is an *initializer*, not an expression the
// grammar accepts anywhere an expression goes (§3: `initializer ::= assignment-expr | "{"
// initializer-list "}"`), and it has no type of its own until something is initialized with it.
// It lives in this hierarchy anyway rather than as a fourth top-level node kind, because that is
// what lets VarDecl keep its single `Expr* initializer()` member and lets sema/IrBuilder reach it
// through the same accept()/visit() dispatch as every other initializer - the alternative would be
// a parallel Initializer hierarchy whose only member is this one class. What keeps it honest is
// that nothing but an initializer position ever produces one: libs/parser only ever builds it from
// parseInitializer(), never from parseAssignment() (see parser.h), so a stray `{` inside an
// ordinary expression is still the syntax error it always was.
//
// TernaryExpr (`cond ? then : else`) sits between assignment and the binary table in the
// precedence chain, right-associative like assignment: libs/parser's parseAssignment() calls
// parseTernary() for its left-hand side instead of jumping straight to the binary levels, and
// parseTernary() itself parses `then` as a full assignment-expression and recurses into itself for
// `else` (so `a ? b : c ? d : e` groups as `a ? b : (c ? d : e)`). It is its own node kind, not
// desugared into anything else, for the same reason AssignExpr is its own node and not a BinaryExpr
// with a fake operator: sema and codegen need to see the three-way branch directly.
//
// MemberExpr covers both `.` and `->` as two genuinely distinct forms with real C semantics (the
// parser must not desugar `a->b` into `(*a).b`) - see token.h's Dot/Arrow. It is one class with an
// isArrow() flag, not two node types: the two forms differ only in whether the object is
// dereferenced first, which is exactly the kind of thing sema checks against the operand type,
// not something that needs its own node kind.
//
// Every Expr carries a const Type* (type.h), resolved by sema (libs/sema) and initially null out of
// the parser - CastExpr and SizeofExpr's *type argument* are the two exceptions: a cast's result
// type IS its target type by definition, no inference needed, so the parser fixes it immediately
// (see CastExpr below). SizeofExpr does NOT do the same for its own result type (what C calls
// size_t) - that is a codegen/ABI policy call this library does not own, so it is left for the
// parser to set via the ordinary setType() like any other node.
//
// UnaryOp/BinaryOp/AssignOp are this library's own enums, not ceresc::lexer::TokenKind - libs/ast
// does not depend on libs/lexer (see the architecture plan's §1 dependency diagram: lexer -> parser
// and ast -> parser, never ast -> lexer). Translating a TokenKind into one of these is the parser's
// job, once it exists.
//
// None of these classes declares a destructor, and Expr's own destructor is not virtual - see the
// static_assert after each class. Arena::create<T> requires T to be trivially destructible, and a
// virtual destructor alone would break that; this is safe because no AST node is ever deleted
// through a base pointer, only ever torn down all at once with the whole Arena (see arena.h). Any
// node with a variable-length child list (CallExpr and InitListExpr) stores a non-owning
// {pointer, count} view over arena-allocated storage instead of a std::vector, for the same reason
// a std::vector member would break the trivially-destructible requirement.
//
// sizeof is V1 scope (the lexer already has KwSizeof, see token.h): SizeofExpr just hasn't had its
// phase yet, ordinary phased work like everything else in this file. `alignof` is different and
// stays reserved for a version after v1 (KwAlignof exists in the lexer's reserved group); once the
// parser exists, it must reject `alignof` with a "not implemented in this version" diagnostic
// instead of guessing at AlignofExpr's shape early.
//
// Implemented in Phase 2 of the phased plan (§13).

namespace ceresc::ast
{
	class AstVisitor;

	class Expr
	{
	protected:
		support::SourceLocation _location;
		const Type* _type = nullptr;

	public:
		explicit Expr(support::SourceLocation location) noexcept : _location(location) {}

	public:
		support::SourceLocation location() const noexcept { return _location; }
		const Type* type() const noexcept { return _type; }
		void setType(const Type* type) noexcept { _type = type; }

		virtual void accept(AstVisitor& visitor) = 0;
	};
	static_assert(TriviallyDestructible<Expr>, "Expr must be trivially destructible because it is allocated in an arena and never deleted individually");

	class IntLiteralExpr final : public Expr
	{
	private:
		u64 _value;

	public:
		IntLiteralExpr(support::SourceLocation location, u64 value) noexcept : Expr(location), _value(value) {}

	public:
		u64 value() const noexcept { return _value; }
		void accept(AstVisitor& visitor) override;
	};
	static_assert(TriviallyDestructible<IntLiteralExpr>, "IntLiteralExpr must be trivially destructible (Arena-allocated)");

	class FloatLiteralExpr final : public Expr
	{
	private:
		f64 _value; // stored as f64 until sema truncates to f32 - see token.h's TokenValue

	public:
		FloatLiteralExpr(support::SourceLocation location, f64 value) noexcept : Expr(location), _value(value) {}

	public:
		f64 value() const noexcept { return _value; }
		void accept(AstVisitor& visitor) override;
	};
	static_assert(TriviallyDestructible<FloatLiteralExpr>, "FloatLiteralExpr must be trivially destructible (Arena-allocated)");

	class CharLiteralExpr final : public Expr
	{
	private:
		char _value;

	public:
		CharLiteralExpr(support::SourceLocation location, char value) noexcept : Expr(location), _value(value) {}

	public:
		char value() const noexcept { return _value; }
		void accept(AstVisitor& visitor) override;
	};
	static_assert(TriviallyDestructible<CharLiteralExpr>, "CharLiteralExpr must be trivially destructible (Arena-allocated)");

	class BoolLiteralExpr final : public Expr
	{
	private:
		bool _value;

	public:
		BoolLiteralExpr(support::SourceLocation location, bool value) noexcept : Expr(location), _value(value) {}

	public:
		bool value() const noexcept { return _value; }
		void accept(AstVisitor& visitor) override;
	};
	static_assert(TriviallyDestructible<BoolLiteralExpr>, "BoolLiteralExpr must be trivially destructible (Arena-allocated)");

	class StringLiteralExpr final : public Expr
	{
	private:
		support::PooledString _value; // interned - escapes change the literal's length, so this can't be a raw view into the source buffer (see token.h's TokenValue)

	public:
		StringLiteralExpr(support::SourceLocation location, support::PooledString value) noexcept : Expr(location), _value(value) {}

	public:
		support::PooledString value() const noexcept { return _value; }
		void accept(AstVisitor& visitor) override;
	};
	static_assert(TriviallyDestructible<StringLiteralExpr>, "StringLiteralExpr must be trivially destructible (Arena-allocated)");

	class NameExpr final : public Expr
	{
	private:
		std::string_view _name; // a raw view into the source buffer, same as an identifier Token's lexeme - no escapes to decode, so no interning needed

	public:
		NameExpr(support::SourceLocation location, std::string_view name) noexcept : Expr(location), _name(name) {}

	public:
		std::string_view name() const noexcept { return _name; }
		void accept(AstVisitor& visitor) override;
	};
	static_assert(TriviallyDestructible<NameExpr>, "NameExpr must be trivially destructible (Arena-allocated)");

	class CallExpr final : public Expr
	{
	private:
		Expr* _callee; // usually a NameExpr, but the grammar allows any postfix-expr here - sema checks it, not the parser
		Expr* const* _args; // non-owning view over an arena-allocated array; the caller must already have
							 // copied the collected argument list into arena storage before constructing
							 // this node - CallExpr never owns or allocates it itself
		u32 _argCount;

	public:
		CallExpr(support::SourceLocation location, Expr* callee, std::span<Expr* const> args) noexcept :
			Expr(location), _callee(callee), _args(args.data()), _argCount(static_cast<u32>(args.size()))
		{}

	public:
		Expr* callee() const noexcept { return _callee; }
		std::span<Expr* const> args() const noexcept { return { _args, _argCount }; }
		u32 argCount() const noexcept { return _argCount; }
		void accept(AstVisitor& visitor) override;
	};
	static_assert(TriviallyDestructible<CallExpr>, "CallExpr must be trivially destructible (Arena-allocated)");

	enum class UnaryOp : u8
	{
		AddressOf,		// &x
		Deref,			// *x
		Negate,			// -x
		LogicalNot,		// !x
		BitwiseNot,		// ~x
		PreIncrement,	// ++x
		PreDecrement,	// --x
		PostIncrement,	// x++
		PostDecrement,	// x--
	};

	class UnaryExpr final : public Expr
	{
	private:
		Expr* _operand;
		UnaryOp _op;

	public:
		UnaryExpr(support::SourceLocation location, UnaryOp op, Expr* operand) noexcept : Expr(location), _operand(operand), _op(op) {}

	public:
		UnaryOp op() const noexcept { return _op; }
		Expr* operand() const noexcept { return _operand; }
		void accept(AstVisitor& visitor) override;
	};
	static_assert(TriviallyDestructible<UnaryExpr>, "UnaryExpr must be trivially destructible (Arena-allocated)");

	enum class BinaryOp : u8
	{
		LogicalOr, LogicalAnd,				// levels 2-3
		BitOr, BitXor, BitAnd,				// levels 4-6
		Eq, Ne,								// level 7
		Lt, Le, Gt, Ge,						// level 8
		Shl, Shr,							// level 9
		Add, Sub,							// level 10
		Mul, Div, Mod,						// level 11
	};

	class BinaryExpr final : public Expr
	{
	private:
		Expr* _lhs;
		Expr* _rhs;
		BinaryOp _op;

	public:
		BinaryExpr(support::SourceLocation location, BinaryOp op, Expr* lhs, Expr* rhs) noexcept : Expr(location), _lhs(lhs), _rhs(rhs), _op(op) {}

	public:
		BinaryOp op() const noexcept { return _op; }
		Expr* lhs() const noexcept { return _lhs; }
		Expr* rhs() const noexcept { return _rhs; }
		void accept(AstVisitor& visitor) override;
	};
	static_assert(TriviallyDestructible<BinaryExpr>, "BinaryExpr must be trivially destructible (Arena-allocated)");

	enum class AssignOp : u8
	{
		Assign,
		AddAssign, SubAssign, MulAssign, DivAssign, ModAssign,
		AndAssign, OrAssign, XorAssign, ShlAssign, ShrAssign,
	};

	class AssignExpr final : public Expr
	{
	private:
		Expr* _target;
		Expr* _value;
		AssignOp _op;

	public:
		AssignExpr(support::SourceLocation location, AssignOp op, Expr* target, Expr* value) noexcept : Expr(location), _target(target), _value(value), _op(op) {}

	public:
		AssignOp op() const noexcept { return _op; }
		Expr* target() const noexcept { return _target; }
		Expr* value() const noexcept { return _value; }
		void accept(AstVisitor& visitor) override;
	};
	static_assert(TriviallyDestructible<AssignExpr>, "AssignExpr must be trivially destructible (Arena-allocated)");

	class IndexExpr final : public Expr
	{
	private:
		Expr* _array;
		Expr* _index;

	public:
		IndexExpr(support::SourceLocation location, Expr* array, Expr* index) noexcept : Expr(location), _array(array), _index(index) {}

	public:
		Expr* array() const noexcept { return _array; }
		Expr* index() const noexcept { return _index; }
		void accept(AstVisitor& visitor) override;
	};
	static_assert(TriviallyDestructible<IndexExpr>, "IndexExpr must be trivially destructible (Arena-allocated)");

	class MemberExpr final : public Expr
	{
	private:
		Expr* _object;
		std::string_view _memberName;
		bool _isArrow; // true for `->`, false for `.` - see token.h's Dot/Arrow

	public:
		MemberExpr(support::SourceLocation location, Expr* object, std::string_view memberName, bool isArrow) noexcept :
			Expr(location), _object(object), _memberName(memberName), _isArrow(isArrow)
		{}

	public:
		Expr* object() const noexcept { return _object; }
		std::string_view memberName() const noexcept { return _memberName; }
		bool isArrow() const noexcept { return _isArrow; }
		void accept(AstVisitor& visitor) override;
	};
	static_assert(TriviallyDestructible<MemberExpr>, "MemberExpr must be trivially destructible (Arena-allocated)");

	class CastExpr final : public Expr
	{
	private:
		Expr* _operand;

	public:
		CastExpr(support::SourceLocation location, const Type* targetType, Expr* operand) noexcept : Expr(location), _operand(operand)
		{
			setType(targetType); // a cast's result type IS the target type - see the header comment above
		}

	public:
		const Type* targetType() const noexcept { return type(); }
		Expr* operand() const noexcept { return _operand; }
		void accept(AstVisitor& visitor) override;
	};
	static_assert(TriviallyDestructible<CastExpr>, "CastExpr must be trivially destructible (Arena-allocated)");

	class SizeofExpr final : public Expr
	{
	private:
		std::variant<const Type*, Expr*> _argument; // sizeof(type-name) or sizeof unary-expr - mutually exclusive per the grammar

	public:
		SizeofExpr(support::SourceLocation location, const Type* argumentType) noexcept : Expr(location), _argument(argumentType) {}
		SizeofExpr(support::SourceLocation location, Expr* argumentExpr) noexcept : Expr(location), _argument(argumentExpr) {}

	public:
		bool isTypeArgument() const noexcept { return std::holds_alternative<const Type*>(_argument); }
		const Type* argumentType() const noexcept { return std::holds_alternative<const Type*>(_argument) ? std::get<const Type*>(_argument) : nullptr; }
		Expr* argumentExpr() const noexcept { return std::holds_alternative<Expr*>(_argument) ? std::get<Expr*>(_argument) : nullptr; }
		void accept(AstVisitor& visitor) override;
	};
	static_assert(TriviallyDestructible<SizeofExpr>, "SizeofExpr must be trivially destructible (Arena-allocated)");

	class AlignofExpr final : public Expr
	{
	private:
		const Type* _argumentType;

	public:
		AlignofExpr(support::SourceLocation location, const Type* argumentType) noexcept : Expr(location), _argumentType(argumentType) {}

	public:
		const Type* argumentType() const noexcept { return _argumentType; }
		void accept(AstVisitor& visitor) override;
	};
	static_assert(TriviallyDestructible<AlignofExpr>, "AlignofExpr must be trivially destructible (Arena-allocated)");

	// Which of the four <stdarg.h> operations a VaExpr is. They are one node kind rather than four
	// because they differ only in which of the same two operands they use, and every consumer
	// (sema, IrBuilder, the printer) has to switch over them together anyway.
	enum class VaOp : u8
	{
		Start, // va_start(ap, last)  - `last` names the final fixed parameter; the result is void
		Arg,   // va_arg(ap, T)       - reads the next argument and advances ap; the result is T
		End,   // va_end(ap)          - the result is void, and there is nothing to undo
		Copy   // va_copy(dst, src)   - the result is void
	};

	constexpr std::string_view vaOpName(VaOp op) noexcept
	{
		switch (op)
		{
			case VaOp::Start: return "va_start";
			case VaOp::Arg:   return "va_arg";
			case VaOp::End:   return "va_end";
			case VaOp::Copy:  return "va_copy";
		}
		return "";
	}

	// One of the four variadic-access builtins. They cannot be ordinary functions - va_arg takes a
	// TYPE as its second operand, and all four have to modify the va_list the caller named rather
	// than a copy of it - so the parser recognizes their names directly and builds this instead of a
	// CallExpr, the same way it treats sizeof/alignof as syntax rather than as calls.
	//
	// `list` is the va_list operand, always an lvalue (C requires it, and va_start/va_arg/va_copy
	// all write through it). `second` is the other operand where there is one: the last fixed
	// parameter's NameExpr for Start, the source va_list for Copy, null for Arg and End - Arg carries
	// a type in `argumentType` instead. See docs/09-Variadic-Convention.md.
	class VaExpr final : public Expr
	{
	private:
		VaOp _op;
		Expr* _list;
		Expr* _second;            // nullable - see the header comment above
		const Type* _argumentType; // VaOp::Arg only

	public:
		VaExpr(support::SourceLocation location, VaOp op, Expr* list, Expr* second = nullptr,
			const Type* argumentType = nullptr) noexcept :
			Expr(location), _op(op), _list(list), _second(second), _argumentType(argumentType)
		{}

	public:
		VaOp op() const noexcept { return _op; }
		Expr* list() const noexcept { return _list; }
		Expr* second() const noexcept { return _second; }
		const Type* argumentType() const noexcept { return _argumentType; }
		void accept(AstVisitor& visitor) override;
	};
	static_assert(TriviallyDestructible<VaExpr>, "VaExpr must be trivially destructible (Arena-allocated)");

	// One machine instruction this language has no other way to reach. All three govern interrupt
	// delivery, which is the one part of the machine a program cannot express through ordinary C:
	// there is no memory address to store into and no arithmetic that has the effect.
	enum class MachineOp : u8
	{
		Sti,  // set the Interrupt flag - user interrupts 16-63 are masked until it is set
		Cli,  // clear it again, for a critical section a handler must not preempt
		Halt  // stop fetching until an interrupt arrives; the dispatcher clears the Halting flag
	};

	constexpr std::string_view machineOpName(MachineOp op) noexcept
	{
		switch (op)
		{
			case MachineOp::Sti:  return "__builtin_sti";
			case MachineOp::Cli:  return "__builtin_cli";
			case MachineOp::Halt: return "__builtin_halt";
		}
		return "";
	}

	// The CASM mnemonic each one lowers to - one instruction, no operands, no result.
	constexpr std::string_view machineOpMnemonic(MachineOp op) noexcept
	{
		switch (op)
		{
			case MachineOp::Sti:  return "sti";
			case MachineOp::Cli:  return "cli";
			case MachineOp::Halt: return "halt";
		}
		return "";
	}

	// `__builtin_sti()` and friends: an instruction spelled as a call, recognized by the parser the
	// same way the va_* builtins are, because there is no header to declare them in and nothing for
	// a real function to contain but the one instruction.
	//
	// Written with the `__builtin_` prefix rather than as bare keywords on purpose: the prefix is
	// reserved to the implementation in C, so no existing program can be using the name, and it
	// reads as what it is - an escape hatch to the machine rather than part of the language.
	// docs/06 keeps the list; an interrupt handler is what they exist for (docs/10).
	class MachineOpExpr final : public Expr
	{
	private:
		MachineOp _op;

	public:
		MachineOpExpr(support::SourceLocation location, MachineOp op) noexcept : Expr(location), _op(op) {}

	public:
		MachineOp op() const noexcept { return _op; }
		void accept(AstVisitor& visitor) override;
	};
	static_assert(TriviallyDestructible<MachineOpExpr>, "MachineOpExpr must be trivially destructible (Arena-allocated)");

	class TernaryExpr final : public Expr
	{
	private:
		Expr* _cond;
		Expr* _then;
		Expr* _else;

	public:
		TernaryExpr(support::SourceLocation location, Expr* cond, Expr* thenExpr, Expr* elseExpr) noexcept :
			Expr(location), _cond(cond), _then(thenExpr), _else(elseExpr)
		{}

	public:
		Expr* cond() const noexcept { return _cond; }
		Expr* thenExpr() const noexcept { return _then; }
		Expr* elseExpr() const noexcept { return _else; }
		void accept(AstVisitor& visitor) override;
	};
	static_assert(TriviallyDestructible<TernaryExpr>, "TernaryExpr must be trivially destructible (Arena-allocated)");

	// `{ 1, 2, 3 }` / `{ { 1, 2 }, { 3, 4 } }` - a brace initializer for an array or a struct (§3's
	// `initializer-list`). Elements are whatever `initializer` itself can be, so an element is
	// either an ordinary expression or another InitListExpr for a nested aggregate.
	//
	// Nesting is NOT elided in this version: a sub-array or a struct field that is itself an
	// aggregate needs its own braces, and sema reports the flat form explicitly rather than
	// guessing (see libs/sema). The node carries no element-to-field mapping of its own - position
	// is the whole contract, resolved against the declared type by sema, exactly like CASM's own
	// positional struct initializers (CeresASM docs/23-Structs.md).
	//
	// type() is set by sema to the type being initialized, so codegen/IrBuilder can ask an
	// InitListExpr what shape it was checked against instead of re-deriving it from the VarDecl.
	class InitListExpr final : public Expr
	{
	private:
		Expr* const* _elements; // non-owning view over arena-allocated storage - see CallExpr above
		u32 _elementCount;

	public:
		InitListExpr(support::SourceLocation location, std::span<Expr* const> elements) noexcept :
			Expr(location), _elements(elements.data()), _elementCount(static_cast<u32>(elements.size()))
		{}

	public:
		std::span<Expr* const> elements() const noexcept { return { _elements, _elementCount }; }
		u32 elementCount() const noexcept { return _elementCount; }
		void accept(AstVisitor& visitor) override;
	};
	static_assert(TriviallyDestructible<InitListExpr>, "InitListExpr must be trivially destructible (Arena-allocated)");
}
