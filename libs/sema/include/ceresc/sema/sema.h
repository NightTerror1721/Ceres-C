#pragma once

#include <ceresc/ast/ast_visitor.h>
#include <ceresc/sema/symbol_table.h>
#include <ceresc/support/arena.h>
#include <ceresc/support/diagnostics.h>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
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
		void visit(ast::AlignofExpr& node) override;
		void visit(ast::VaExpr& node) override;
		void visit(ast::MachineOpExpr& node) override;
		void visit(ast::TernaryExpr& node) override;
		void visit(ast::InitListExpr& node) override;

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
		void visit(ast::InterruptVectorDecl& node) override;
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

		// Every interrupt number bound in this unit, so a second binding to one can name the first.
		// Only this unit: whole-program uniqueness belongs to `ceres link`, which is the only thing
		// that sees every object (CeresASM 26-Interrupt-Vector-Binding.md).
		std::unordered_map<i64, ast::InterruptVectorDecl*> _interruptVectors;

	private:
		support::Arena& _arena;
		support::DiagnosticEngine& _diagnostics;

		std::vector<std::unique_ptr<Scope>> _scopes; // stack; back() is the innermost, front() the global scope
		Scope* _globalScope = nullptr;

		const ast::Type* _currentFunctionReturnType = nullptr;
		// The function whose body is being checked, or null at file scope. Only va_start needs it:
		// it has to name that function's last fixed parameter, and only a variadic function has a
		// tail for it to start on.
		const ast::FunctionDecl* _currentFunction = nullptr;
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

		// Checks one `initializer` (§3) against the type being initialized - the declarator-position
		// counterpart to checkExpr(), and the only path that understands an InitListExpr (expr.h).
		// Separate from checkExpr() because an initializer is checked TOP-DOWN: a brace list has no
		// type of its own to infer and report back, it only has a shape that either fits the
		// declared type or does not, and each element is then checked against whatever type that
		// position implies. Recurses for a nested aggregate.
		//
		// Three forms are accepted, matching real C:
		//   - a brace list for an array or a struct, positionally, with FEWER elements than the
		//     aggregate has slots allowed (the rest zero-fills, as in C - see libs/ir for where
		//     that actually happens) and more being an error;
		//   - a string literal for a char array (`char s[8] = "hola"`, including one row of a
		//     `char[2][8]`), which must fit with its terminating zero;
		//   - any ordinary assignment-expression for a scalar/pointer/whole-struct target, checked
		//     exactly as an assignment to that type would be.
		//
		// Brace elision is deliberately NOT accepted: `int m[2][2] = { 1, 2, 3, 4 }` is reported
		// rather than guessed at, because the flat and nested forms would otherwise mean different
		// things depending on a rule nobody reading the code can see ("comprensible antes que
		// completo", §0). The diagnostic names the missing braces.
		void checkInitializer(const ast::Type* type, ast::Expr* init);
		// The brace-list half of checkInitializer(), split out only so the recursion reads as
		// "list against aggregate" instead of one function with two unrelated halves.
		void checkInitList(const ast::Type* type, ast::InitListExpr& list);
		// True for a type a string literal may initialize an array OF - `char`/`signed char`/
		// `unsigned char`, i.e. exactly the one-byte integer types.
		static bool isCharType(const ast::Type* type) noexcept;

		bool declareSymbol(const Symbol& symbol);

		// The rules a storage-class specifier carries that are not about types: where each one is
		// allowed to appear, and what it demands of an initializer. `static` is the interesting one -
		// its initializer becomes bytes in the loaded image, so it has to be computable now.
		void checkStorageClass(ast::VarDecl& node);
		// Everything `__interrupt` promises about a function's contract with the world - see the
		// definition. A no-op for every ordinary function.
		void checkInterruptHandler(ast::FunctionDecl& node);

		// True for an expression whose value the compiler can work out without running anything -
		// what a variable with static storage (a global, or a `static` local) needs its initializer
		// to be. Deliberately syntactic: it recognizes the shapes that are constant rather than
		// trying to evaluate them, because the evaluation itself belongs to libs/codegen, which is
		// where the target's own arithmetic lives.
		bool isConstantInitializer(const ast::Expr* expr) const;
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
		// Array-to-pointer decay (real C's rule, see §8/§14 of the architecture plan): wherever an
		// array's VALUE is used - a function argument, an initializer, the right side of an
		// assignment, an operand of pointer arithmetic, a return value - it is really the address of
		// its first element, not its own Array type. `sizeof` and unary `&` are the two contexts a
		// real array does NOT decay in; both already read the operand's Type directly instead of
		// going through isAssignable()/a BinaryExpr's operand types, so neither calls this. Not
		// static (unlike isAssignable() itself): building the decayed Pointer type needs the same
		// Arena every other compound Type in this file is allocated from.
		// Type-checks one argument sitting in a call's variadic tail. There is no declared
		// parameter to check it against, so what is checked instead is that the type can travel
		// through the variadic half of the calling convention at all - see
		// docs/09-Variadic-Convention.md, and Sema's own note on the default argument promotions.
		void checkVariadicArgument(ast::Expr* arg, bool calleeIsVariadic);

		const ast::Type* decayArray(const ast::Type* type) noexcept;
		static const ast::Type* integerPromote(const ast::Type* type) noexcept;
		static const ast::Type* commonArithmeticType(const ast::Type* lhs, const ast::Type* rhs) noexcept;
	};
}
