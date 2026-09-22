#pragma once

#include "expr.h"
#include <span>

// Stmt hierarchy: EmptyStmt, ExprStmt, DeclStmt, CompoundStmt, IfStmt, WhileStmt, DoWhileStmt,
// ForStmt, ReturnStmt, BreakStmt, ContinueStmt, SwitchStmt, CaseStmt, DefaultStmt, GotoStmt,
// LabelStmt.
//
// Same trivially-destructible / non-owning-arena-span discipline as expr.h, for the same reason:
// every node here is Arena-allocated via Arena::create<T> and never deleted individually, so no
// class in this file declares a destructor and Stmt's own destructor is not virtual (see the
// static_assert after each class). CompoundStmt's statement list is the one variable-length child
// here (mirrors CallExpr::_args in expr.h) - a non-owning {pointer, count} view over arena storage,
// not a std::vector.
//
// DeclStmt only forward-declares Decl (this file does not include decl.h): it stores an array of
// Decl*, which needs no more than a forward declaration, and decl.h's own FunctionDecl needs a CompoundStmt*
// right back - an actual mutual #include would be a real cycle. AstVisitor (ast_visitor.h) is the
// one place that includes both stmt.h and decl.h together and therefore the one place that can
// define every accept() body, same reasoning as expr.h's own note about AstVisitor.
//
// CaseStmt/DefaultStmt/SwitchStmt follow real C's grammar, not a simplified one: `case`/`default`
// are ordinary labeled-statements (`labeled-statement := 'case' constant-expression ':' statement`),
// each one wrapping exactly the single statement that immediately follows its ':'. They are not
// containers for "everything until the next case" - that shape (and C's fallthrough behavior) falls
// out naturally once a switch's body is the usual CompoundStmt: sibling statements after a
// `case N: stmt` simply sit next to it in that same CompoundStmt, same as any other block. This
// project has no constant-expression evaluator yet (see CastExpr/SizeofExpr in expr.h for the same
// idea applied elsewhere), so CaseStmt's value is a general Expr* - checking that it is actually a
// compile-time constant is sema's job. LabelStmt (`identifier ':' statement`) exists only because
// GotoStmt needs something to name as a target; this parser does not check that a goto's label
// actually exists anywhere in the function, or that case/default only appear inside a switch - both
// are whole-function/whole-switch checks that need a symbol table this project doesn't have until
// libs/sema.
//
// Implemented in Fase 3 of the phased plan (§13). See the architecture plan, §6 (AST structure) and
// §7 (grammar).

namespace ceresc::ast
{
	class Decl; // forward declaration only - see the header comment above

	class Stmt
	{
	protected:
		support::SourceLocation _location;

	public:
		explicit Stmt(support::SourceLocation location) noexcept : _location(location) {}

	public:
		support::SourceLocation location() const noexcept { return _location; }

		virtual void accept(AstVisitor& visitor) = 0;
	};
	static_assert(TriviallyDestructible<Stmt>, "Stmt must be trivially destructible because it is allocated in an arena and never deleted individually");

	class EmptyStmt final : public Stmt
	{
	public:
		explicit EmptyStmt(support::SourceLocation location) noexcept : Stmt(location) {}

	public:
		void accept(AstVisitor& visitor) override;
	};
	static_assert(TriviallyDestructible<EmptyStmt>, "EmptyStmt must be trivially destructible (Arena-allocated)");

	class ExprStmt final : public Stmt
	{
	private:
		Expr* _expr;

	public:
		ExprStmt(support::SourceLocation location, Expr* expr) noexcept : Stmt(location), _expr(expr) {}

	public:
		Expr* expr() const noexcept { return _expr; }
		void accept(AstVisitor& visitor) override;
	};
	static_assert(TriviallyDestructible<ExprStmt>, "ExprStmt must be trivially destructible (Arena-allocated)");

	// `__asm__("sti\n\thalt");` - text for the assembler, put into the function as it is. To the code generator it is a call:
	// values a call would clobber are kept out of the registers it may change, and nothing is assumed about memory
	// across it. `volatile` is accepted and changes nothing, because nothing here moves or drops an asm statement.
	class AsmStmt final : public Stmt
	{
	private:
		support::PooledString _text;
		bool _isVolatile;

	public:
		AsmStmt(support::SourceLocation location, support::PooledString text, bool isVolatile) noexcept :
			Stmt(location), _text(text), _isVolatile(isVolatile)
		{}

	public:
		support::PooledString text() const noexcept { return _text; }
		bool isVolatile() const noexcept { return _isVolatile; }
		void accept(AstVisitor& visitor) override;
	};
	static_assert(TriviallyDestructible<AsmStmt>, "AsmStmt must be trivially destructible (Arena-allocated)");

	class DeclStmt final : public Stmt
	{
	private:
		// One entry per declarator in `int a, b, c;` - a non-owning view over an arena-allocated
		// array, same pattern as CompoundStmt::_stmts. A bare pointer, so the forward declaration
		// of Decl above is enough.
		Decl* const* _decls;
		u32 _count;

	public:
		DeclStmt(support::SourceLocation location, std::span<Decl* const> decls) noexcept :
			Stmt(location), _decls(decls.data()), _count(static_cast<u32>(decls.size()))
		{}

	public:
		std::span<Decl* const> decls() const noexcept { return { _decls, _count }; }
		u32 count() const noexcept { return _count; }
		void accept(AstVisitor& visitor) override;
	};
	static_assert(TriviallyDestructible<DeclStmt>, "DeclStmt must be trivially destructible (Arena-allocated)");

	class CompoundStmt final : public Stmt
	{
	private:
		Stmt* const* _stmts; // non-owning view over an arena-allocated array, same pattern as CallExpr::_args
		u32 _count;

	public:
		CompoundStmt(support::SourceLocation location, std::span<Stmt* const> stmts) noexcept :
			Stmt(location), _stmts(stmts.data()), _count(static_cast<u32>(stmts.size()))
		{}

	public:
		std::span<Stmt* const> stmts() const noexcept { return { _stmts, _count }; }
		u32 count() const noexcept { return _count; }
		void accept(AstVisitor& visitor) override;
	};
	static_assert(TriviallyDestructible<CompoundStmt>, "CompoundStmt must be trivially destructible (Arena-allocated)");

	class IfStmt final : public Stmt
	{
	private:
		Expr* _cond;
		Stmt* _then;
		Stmt* _else; // nullable - no `else` clause

	public:
		IfStmt(support::SourceLocation location, Expr* cond, Stmt* thenStmt, Stmt* elseStmt = nullptr) noexcept :
			Stmt(location), _cond(cond), _then(thenStmt), _else(elseStmt)
		{}

	public:
		Expr* cond() const noexcept { return _cond; }
		Stmt* thenStmt() const noexcept { return _then; }
		Stmt* elseStmt() const noexcept { return _else; }
		void accept(AstVisitor& visitor) override;
	};
	static_assert(TriviallyDestructible<IfStmt>, "IfStmt must be trivially destructible (Arena-allocated)");

	class WhileStmt final : public Stmt
	{
	private:
		Expr* _cond;
		Stmt* _body;

	public:
		WhileStmt(support::SourceLocation location, Expr* cond, Stmt* body) noexcept : Stmt(location), _cond(cond), _body(body) {}

	public:
		Expr* cond() const noexcept { return _cond; }
		Stmt* body() const noexcept { return _body; }
		void accept(AstVisitor& visitor) override;
	};
	static_assert(TriviallyDestructible<WhileStmt>, "WhileStmt must be trivially destructible (Arena-allocated)");

	class DoWhileStmt final : public Stmt
	{
	private:
		Stmt* _body;
		Expr* _cond;

	public:
		DoWhileStmt(support::SourceLocation location, Stmt* body, Expr* cond) noexcept : Stmt(location), _body(body), _cond(cond) {}

	public:
		Stmt* body() const noexcept { return _body; }
		Expr* cond() const noexcept { return _cond; }
		void accept(AstVisitor& visitor) override;
	};
	static_assert(TriviallyDestructible<DoWhileStmt>, "DoWhileStmt must be trivially destructible (Arena-allocated)");

	class ForStmt final : public Stmt
	{
	private:
		Stmt* _init;	  // nullable: `for (;;)` - either an ExprStmt, a DeclStmt, or absent
		Expr* _cond;	  // nullable: an absent condition means "always true", same as real C
		Expr* _increment; // nullable
		Stmt* _body;

	public:
		ForStmt(support::SourceLocation location, Stmt* init, Expr* cond, Expr* increment, Stmt* body) noexcept :
			Stmt(location), _init(init), _cond(cond), _increment(increment), _body(body)
		{}

	public:
		Stmt* init() const noexcept { return _init; }
		Expr* cond() const noexcept { return _cond; }
		Expr* increment() const noexcept { return _increment; }
		Stmt* body() const noexcept { return _body; }
		void accept(AstVisitor& visitor) override;
	};
	static_assert(TriviallyDestructible<ForStmt>, "ForStmt must be trivially destructible (Arena-allocated)");

	class ReturnStmt final : public Stmt
	{
	private:
		Expr* _value; // nullable: `return;` with no value

	public:
		ReturnStmt(support::SourceLocation location, Expr* value = nullptr) noexcept : Stmt(location), _value(value) {}

	public:
		Expr* value() const noexcept { return _value; }
		void accept(AstVisitor& visitor) override;
	};
	static_assert(TriviallyDestructible<ReturnStmt>, "ReturnStmt must be trivially destructible (Arena-allocated)");

	class BreakStmt final : public Stmt
	{
	public:
		explicit BreakStmt(support::SourceLocation location) noexcept : Stmt(location) {}

	public:
		void accept(AstVisitor& visitor) override;
	};
	static_assert(TriviallyDestructible<BreakStmt>, "BreakStmt must be trivially destructible (Arena-allocated)");

	class ContinueStmt final : public Stmt
	{
	public:
		explicit ContinueStmt(support::SourceLocation location) noexcept : Stmt(location) {}

	public:
		void accept(AstVisitor& visitor) override;
	};
	static_assert(TriviallyDestructible<ContinueStmt>, "ContinueStmt must be trivially destructible (Arena-allocated)");

	class SwitchStmt final : public Stmt
	{
	private:
		Expr* _cond;
		Stmt* _body; // typically a CompoundStmt full of CaseStmt/DefaultStmt labels - see the header comment above

	public:
		SwitchStmt(support::SourceLocation location, Expr* cond, Stmt* body) noexcept : Stmt(location), _cond(cond), _body(body) {}

	public:
		Expr* cond() const noexcept { return _cond; }
		Stmt* body() const noexcept { return _body; }
		void accept(AstVisitor& visitor) override;
	};
	static_assert(TriviallyDestructible<SwitchStmt>, "SwitchStmt must be trivially destructible (Arena-allocated)");

	class CaseStmt final : public Stmt
	{
	private:
		Expr* _value; // a constant-expression in real C - sema checks constancy, not the parser (see the header comment above)
		Expr* _upper; // GNU `case low ... high:`: the high bound, or null for a single value
		Stmt* _body;  // exactly the one statement following ':' - see the header comment above

	public:
		CaseStmt(support::SourceLocation location, Expr* value, Stmt* body, Expr* upper = nullptr) noexcept :
			Stmt(location), _value(value), _upper(upper), _body(body) {}

	public:
		Expr* value() const noexcept { return _value; }
		Expr* upper() const noexcept { return _upper; } // null unless this is a `...` range
		Stmt* body() const noexcept { return _body; }
		void accept(AstVisitor& visitor) override;
	};
	static_assert(TriviallyDestructible<CaseStmt>, "CaseStmt must be trivially destructible (Arena-allocated)");

	class DefaultStmt final : public Stmt
	{
	private:
		Stmt* _body; // exactly the one statement following ':' - see the header comment above

	public:
		DefaultStmt(support::SourceLocation location, Stmt* body) noexcept : Stmt(location), _body(body) {}

	public:
		Stmt* body() const noexcept { return _body; }
		void accept(AstVisitor& visitor) override;
	};
	static_assert(TriviallyDestructible<DefaultStmt>, "DefaultStmt must be trivially destructible (Arena-allocated)");

	class GotoStmt final : public Stmt
	{
	private:
		std::string_view _label; // a raw view into the source buffer, same as NameExpr::_name in expr.h

	public:
		GotoStmt(support::SourceLocation location, std::string_view label) noexcept : Stmt(location), _label(label) {}

	public:
		std::string_view label() const noexcept { return _label; }
		void accept(AstVisitor& visitor) override;
	};
	static_assert(TriviallyDestructible<GotoStmt>, "GotoStmt must be trivially destructible (Arena-allocated)");

	class LabelStmt final : public Stmt
	{
	private:
		std::string_view _label;
		Stmt* _body; // exactly the one statement following ':' - see the header comment above

	public:
		LabelStmt(support::SourceLocation location, std::string_view label, Stmt* body) noexcept : Stmt(location), _label(label), _body(body) {}

	public:
		std::string_view label() const noexcept { return _label; }
		Stmt* body() const noexcept { return _body; }
		void accept(AstVisitor& visitor) override;
	};
	static_assert(TriviallyDestructible<LabelStmt>, "LabelStmt must be trivially destructible (Arena-allocated)");
}
