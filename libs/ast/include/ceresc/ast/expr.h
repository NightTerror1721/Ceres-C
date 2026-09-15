#pragma once

#include <ceresc/support/source_location.h>
#include <ceresc/support/string_pool.h>
#include "type.h"
#include <span>
#include <string_view>
#include <variant>

// Expr hierarchy: IntLiteralExpr, FloatLiteralExpr, CharLiteralExpr, BoolLiteralExpr,
// StringLiteralExpr, NameExpr, CallExpr, UnaryExpr, BinaryExpr, AssignExpr, IndexExpr, MemberExpr,
// CastExpr, SizeofExpr.
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
// node with a variable-length child list (only CallExpr right now) stores a non-owning
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
}
