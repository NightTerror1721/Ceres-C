#pragma once

#include <ceresc/ast/ast_visitor.h>
#include <ceresc/sema/symbol_table.h>
#include <ceresc/support/arena.h>
#include <ceresc/support/diagnostics.h>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

// Sema - walks the parser's AST (which does not yet know whether `x` exists, or what type
// anything has) and annotates it in place: every Expr gets its resolved Type* via the
// Expr::setType() every node already carries (see expr.h), every NameExpr resolves to a Symbol
// (symbol_table.h) through a real chain of lexical scopes the parser never had.
//
// Never stops at the first type error - it assumes the best type it can infer (usually `int`,
// see errorRecoveryType()) and keeps walking, so the rest of the file still gets checked. All
// diagnostics accumulate in the same DiagnosticEngine (libs/support) every earlier phase already
// uses.
//
// What this phase deliberately does NOT do: assign VarDecl a precise byte-level stack frame
// offset. Storage classification (Symbol::isGlobal) is as far as Fase 4 goes - the actual frame
// layout (incoming args, spill slots, locals, in that specific calling-convention order) is
// libs/codegen/frame_layout.h's job, explicitly starting Fase 6, and duplicating that math here
// before codegen exists to consume it would just be dead weight to keep in sync.
//
// struct/enum/typedef name resolution is NOT redone here either: libs/parser's own flat
// _structTable/_enumTable/_typedefTable (parser.h) already baked every tag/typedef reference into
// the AST itself (a Type's StructDecl*/EnumDecl* payload, see type.h) before sema ever runs. This
// class's own Scope chain (symbol_table.h) exists only for *value*-level names - variables,
// parameters, functions, enum constants - which the parser never scoped at all.
//
// See the architecture plan, §8.
//
// Implemented in Fase 4 of the phased plan (§13).

namespace ceresc::ast
{
	class TranslationUnit;
	class NameExpr;
}

namespace ceresc::sema
{
	class Sema final : public ast::AstVisitor
	{
	public:
		Sema(support::Arena& arena, support::DiagnosticEngine& diagnostics) noexcept;
		Sema(const Sema&) = delete;
		Sema(Sema&&) = delete;
		~Sema() override = default;

		Sema& operator=(const Sema&) = delete;
		Sema& operator=(Sema&&) = delete;

	public:
		// Type-checks `unit` in place. Returns true iff no error (warnings are fine) was reported
		// while checking it.
		bool check(ast::TranslationUnit& unit);

	public:
		void visit(ast::IntLiteralExpr& node) override;
		void visit(ast::FloatLiteralExpr& node) override;
		void visit(ast::CharLiteralExpr& node) override;
		void visit(ast::BoolLiteralExpr& node) override;
		void visit(ast::StringLiteralExpr& node) override;
		void visit(ast::NameExpr& node) override;
		void visit(ast::CallExpr& node) override;
		void visit(ast::UnaryExpr& node) override;
		void visit(ast::BinaryExpr& node) override;
		void visit(ast::AssignExpr& node) override;
		void visit(ast::IndexExpr& node) override;
		void visit(ast::MemberExpr& node) override;
		void visit(ast::CastExpr& node) override;
		void visit(ast::SizeofExpr& node) override;
		void visit(ast::TernaryExpr& node) override;

		void visit(ast::EmptyStmt& node) override;
		void visit(ast::ExprStmt& node) override;
		void visit(ast::DeclStmt& node) override;
		void visit(ast::CompoundStmt& node) override;
		void visit(ast::IfStmt& node) override;
		void visit(ast::WhileStmt& node) override;
		void visit(ast::DoWhileStmt& node) override;
		void visit(ast::ForStmt& node) override;
		void visit(ast::ReturnStmt& node) override;
		void visit(ast::BreakStmt& node) override;
		void visit(ast::ContinueStmt& node) override;
		void visit(ast::SwitchStmt& node) override;
		void visit(ast::CaseStmt& node) override;
		void visit(ast::DefaultStmt& node) override;
		void visit(ast::GotoStmt& node) override;
		void visit(ast::LabelStmt& node) override;

		void visit(ast::VarDecl& node) override;
		void visit(ast::FunctionDecl& node) override;
		void visit(ast::StructDecl& node) override;
		void visit(ast::EnumDecl& node) override;
		void visit(ast::TypedefDecl& node) override;
		void visit(ast::TranslationUnit& node) override;

	public:
		// A small compile-time integer constant evaluator - exactly what this project needs
		// constant-expressions for: enum values (`enum { A, B = 1 << A }`) and switch/case labels
		// (`labeled-statement`'s constant-expression production - "sema checks constancy, not the
		// parser", see stmt.h/expr.h). Deliberately not a general-purpose one: no floating-point
		// constants, no reference to a plain variable (even a const one) or a function call - just
		// int/char/bool literals, unary/binary/ternary operators over other constants, sizeof (via
		// the operand's already-annotated Type, so this must run after checkExpr() has visited the
		// same subtree at least once), a cast (no truncation modeled, just passes the value
		// through), and a NameExpr referring to a previously-declared enum constant.
		std::optional<i64> evalConstantExpr(ast::Expr* expr);

	private:
		support::Arena& _arena;
		support::DiagnosticEngine& _diagnostics;

		std::vector<std::unique_ptr<Scope>> _scopes; // stack; back() is the innermost, front() the global scope
		Scope* _globalScope = nullptr;

		const ast::Type* _currentFunctionReturnType = nullptr;
		std::vector<std::string_view> _currentFunctionLabels; // collected once per function, see collectLabels()
		u32 _loopDepth = 0;

		// Per-switch state, pushed in visit(SwitchStmt&) and popped on the way out - a stack, not
		// a single instance, because a switch can nest inside another switch's case body.
		// `_switchStack.empty()` doubles as "not currently inside a switch" (what a bare
		// `_switchDepth` counter used to answer for break/case/default validity); the seen-values/
		// defaultSeen fields exist so case/default can also catch a duplicate within the innermost
		// switch.
		struct SwitchContext
		{
			std::vector<i64> seenCaseValues;
			bool defaultSeen = false;
		};
		std::vector<SwitchContext> _switchStack;

		// AstVisitor::visit() returns void, so the "type of the Expr just visited" is threaded
		// through this member instead of a return value - every visit(SomeExpr&) sets it right
		// before returning, and checkExpr() (the one place anything reads it) does so immediately
		// after the accept() call that set it. Safe because traversal is single-threaded, strictly
		// sequential recursive descent - the same reasoning AstPrinter's own _output member relies on.
		const ast::Type* _lastExprType = nullptr;

	private:
		Scope& pushScope();
		void popScope();
		Scope& currentScope() noexcept;

		const ast::Type* checkExpr(ast::Expr* expr);
		void checkStmt(ast::Stmt* stmt);

		bool declareSymbol(const Symbol& symbol);
		void collectLabels(ast::Stmt* stmt, std::vector<std::string_view>& out);

		static const ast::Type* errorRecoveryType() noexcept;
		static bool isArithmeticType(const ast::Type* type) noexcept;
		static bool isIntegerType(const ast::Type* type) noexcept;
		static bool isScalarType(const ast::Type* type) noexcept;
		// Not static: an lvalue NameExpr must actually be an object (a variable/parameter, not an
		// enum constant or a bare function name), which needs a symbol lookup against the current
		// scope - see sema.cpp.
		bool isLValue(const ast::Expr* expr);
		static bool isAssignable(const ast::Type* target, const ast::Type* source) noexcept;
		static const ast::Type* integerPromote(const ast::Type* type) noexcept;
		static const ast::Type* commonArithmeticType(const ast::Type* lhs, const ast::Type* rhs) noexcept;
	};
}
