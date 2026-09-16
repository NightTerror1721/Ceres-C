#pragma once

#include <ceresc/ast/ast_visitor.h>
#include <ceresc/ir/ir_function.h>
#include <ceresc/support/arena.h>
#include <ceresc/support/diagnostics.h>
#include <ceresc/support/optimization.h>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

// IrBuilder - an AstVisitor (libs/ast) that lowers the annotated AST from sema into IR.
//
// Every visited Expr returns an IrValue (a temporary or a constant); every visited Stmt appends
// instructions to the current basic block and, for control flow (if/while/for), creates the child
// blocks and updates what "the current block" is for what follows. && and || lower to control
// flow directly, never to a plain BinOp, so the right-hand side is never evaluated once the left
// side already decides the result. See the architecture plan, §9.
//
// Assumes `unit` already passed Sema::check() (libs/sema) with zero errors: it does not
// re-diagnose anything sema already would have (an undeclared name, a bad assignment target, a
// non-constant case label...), and can produce nonsense IR - or crash - on a program sema would
// have rejected. This is exactly the same contract libs/codegen (§10) will have on the IR this
// class produces.
//
// Sema's own Scope/Symbol chain (libs/sema/symbol_table.h) does not outlive Sema::check() (see its
// own header comment) - by the time IrBuilder runs, in a later, separate pass, every Scope sema
// pushed is long gone. So IrBuilder resolves names a second time, with its own scope chain and its
// own small per-function symbol table (see LocalSymbol below) - deliberately not the same Symbol
// shape sema.h uses, since IrBuilder needs different information for lowering (which frame slot,
// or a global's own name, or an already-folded enum constant value) than sema needs for
// diagnostics. A *function's* own name is never looked up this way at all: CallExpr's callee is
// always a NameExpr in this subset (sema.cpp's own note), so IrBuilder just uses its lexeme
// directly as the Call instruction's callee name - libs/codegen is what resolves that against a
// real CASM symbol, not this library.
//
// Implemented in Fase 5 of the phased plan (§13). IrPrinter (--emit-ir) ships alongside it.

namespace ceresc::ir
{
	class IrBuilder final : public ast::AstVisitor
	{
	public:
		// `options` controls the one optimization IrBuilder itself performs: reusing a frame slot
		// between locals whose lexical scopes are disjoint (see newLocalSlotFor()). Everything else
		// optimization-related happens later, over the finished IR (ir_optimizer.h) or during code
		// generation - IrBuilder's job is to lower faithfully first. Pass
		// support::OptimizationOptions::none() for the plain, one-slot-per-declaration lowering that
		// every IR golden test pins.
		IrBuilder(support::Arena& arena, support::DiagnosticEngine& diagnostics,
			const support::OptimizationOptions& options) noexcept;
		IrBuilder(const IrBuilder&) = delete;
		IrBuilder(IrBuilder&&) = delete;
		~IrBuilder() override = default;

		IrBuilder& operator=(const IrBuilder&) = delete;
		IrBuilder& operator=(IrBuilder&&) = delete;

	public:
		// Lowers `unit` into one IrFunction per FunctionDecl definition - see the header comment
		// above for the "already sema-checked" assumption this relies on.
		IrModule build(ast::TranslationUnit& unit);

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

	private:
		enum class LocalSymbolKind : u8 { Local, Global, EnumConstant };

		// A name IrBuilder knows how to lower a reference to - see the header comment above for why
		// this is not sema::Symbol.
		struct LocalSymbol
		{
			LocalSymbolKind kind = LocalSymbolKind::Local;
			u32 localSlot = 0; // Local (a frame slot index - see IrFunction::newLocalSlot())
			i64 enumValue = 0; // EnumConstant (already folded - see foldConstant())
		};

	private:
		support::Arena& _arena;
		support::DiagnosticEngine& _diagnostics;
		support::OptimizationOptions _options;

		IrModule _module;
		IrFunction* _currentFunction = nullptr;
		BasicBlock* _currentBlock = nullptr;
		IrValue _lastValue{}; // set by every visit(SomeExpr&), read back by lowerExpr() - same idiom as Sema::_lastExprType

		std::vector<std::unordered_map<std::string_view, LocalSymbol>> _scopes; // function-local block scopes; empty at file scope
		std::unordered_map<std::string_view, LocalSymbol> _globalSymbols;      // file-scope variables + file-scope enum constants

		// Frame-slot reuse across disjoint lexical scopes (options().localSlotReuse). `_scopeSlots`
		// records which slots each open scope introduced; popping a scope returns them to
		// `_freeLocalSlots`, where the next declaration in a SIBLING scope can claim one instead of
		// growing the frame. Lexical scoping is what makes this safe without any analysis: a local
		// is unreachable by name once its scope closes, and a pointer still aimed at it is already
		// dangling by C's own rules.
		std::vector<std::vector<u32>> _scopeSlots;
		std::vector<u32> _freeLocalSlots;

		std::vector<BasicBlock*> _breakTargets;
		std::vector<BasicBlock*> _continueTargets;
		std::unordered_map<std::string_view, BasicBlock*> _labelBlocks; // goto targets, rebuilt once per function (see collectLabelBlocks())
		std::unordered_map<const ast::Stmt*, BasicBlock*> _caseBlocks;  // switch case/default entry blocks, keyed by node identity (see collectSwitchCases())

		u32 _nextStringLiteralId = 0;

	private:
		// Reserves a frame slot for one local VarDecl: a slot a closed sibling scope left behind
		// when options().localSlotReuse allows it, a brand new one otherwise. Either way the slot
		// is recorded against the innermost open scope, so it returns to the pool when that scope
		// closes - see _scopeSlots/_freeLocalSlots.
		u32 newLocalSlotFor(u32 sizeInBytes, bool isFloat);

		void pushScope();
		void popScope();
		void declareSymbol(std::string_view name, const LocalSymbol& symbol);
		LocalSymbol* lookupSymbol(std::string_view name) noexcept;

		IrValue lowerExpr(ast::Expr* expr);
		void lowerStmt(ast::Stmt* stmt);
		// The address of an lvalue - see the ".cpp" for why this is its own recursive dynamic_cast
		// dispatch (mirroring sema.cpp's own isLValue()/evalConstantExpr()) rather than a second
		// AstVisitor.
		IrValue lowerAddress(ast::Expr* expr);
		// The rvalue of `expr`, decaying an array-typed result to the address of its own storage
		// instead of loading through it - real C's array-to-pointer decay (mirrors sema::Sema's own
		// decayArray(), sema.cpp), needed because unlike every other rvalue an array never sits
		// behind a separately-stored address the way a pointer variable does: its frame/global slot
		// already holds the elements directly, so "the array's value" and "the array's address" are
		// the same IrValue. Shared by every Expr kind whose annotated type can be an array as well as
		// an ordinary scalar (NameExpr, IndexExpr, MemberExpr, `*p`) - see the ".cpp".
		IrValue lowerRValue(ast::Expr* expr);
		// Lowers `cond` as a branch directly to trueBlock/falseBlock, short-circuiting && and ||
		// (and De Morgan-swapping ! ) instead of first materializing a 0/1 value and then branching
		// on it.
		void lowerCondition(ast::Expr* cond, BasicBlock* trueBlock, BasicBlock* falseBlock);
		// The 0/1 value of `cond` used as a plain expression (not as an if/while/for condition) -
		// built on lowerCondition() plus the Ltrue/Lfalse/Lend materialization pattern §9 shows for
		// && / ||.
		IrValue materializeBoolean(ast::Expr* cond);
		// Shared by BinaryExpr's own arithmetic operators and AssignExpr's compound-assignment
		// operators (`+=` and friends desugar to this same lowering) - handles pointer-arithmetic
		// scaling and the Shr/Sar and isUnsigned choice.
		IrValue lowerArithmetic(support::SourceLocation loc, ast::BinaryOp op, const ast::Type* resultType,
			const ast::Type* lhsType, const ast::Type* rhsType, IrValue lhsVal, IrValue rhsVal);

		// Sema unifies mixed int/float operands onto one common `resultType` (commonArithmeticType(),
		// sema.cpp) without inserting an implicit-cast node anywhere in the AST - the operands simply
		// keep their own original types, and the promotion is only ever recorded as the *node's*
		// type. So wherever an operation needs both sides in the SAME register bank (arithmetic,
		// comparison), this converts `value` (whose real type is `type`) up to float first when
		// `type` says it is not one already - a no-op (returns `value` unchanged) when it already is.
		// Mirrors real C's usual arithmetic conversions, just realized here instead of in sema.
		IrValue toFloatIfNeeded(support::SourceLocation loc, IrValue value, const ast::Type* type);
		// The general form of toFloatIfNeeded() above, for a value moving into storage of a
		// DIFFERENT declared type than the one it was computed as - `float x; x = 5;` (int value,
		// float storage) or `int x; x += 1.5f;` (compound assignment's promoted float result,
		// narrowing back to `x`'s real int storage) both need this, unlike a plain BinOp/Cmp operand
		// pair, which sema has already unified onto one common type by the time IrBuilder sees it.
		// A no-op when `fromType`/`toType` agree on isFloat().
		IrValue convertForStore(support::SourceLocation loc, IrValue value, const ast::Type* fromType, const ast::Type* toType);

		// Reports "using a struct by value here is not supported in this version" when `type` is a
		// struct - called right before every place this file materializes an ordinary rvalue via a
		// single scalar Load or Call result (NameExpr, MemberExpr, IndexExpr, `*p`, a struct-
		// returning call, and lowerAddress()'s own fallback for a non-lvalue struct base like
		// `make().field`): none of those can honestly represent a struct wider than one word, and
		// staying silent about it would mean a struct-by-value assignment/argument/expression
		// (Sema's isAssignable allows `target == source` for two identical struct types - sema.cpp -
		// even though this subset's documented scope is pointer-only struct passing, see expr.h/
		// decl.h) silently truncates to its first 4 bytes instead of failing loudly, the opposite of
		// what §0 of the architecture plan calls for ("los errores enseñan"). Does nothing for every
		// other type, including a null one (already some other pass's error to report). Deliberately
		// void, not bool: every call site still falls through to its own best-effort scalar Load/
		// Store afterward regardless (this project's usual "report and keep going" recovery style,
		// see sema.h's own header comment) rather than branching on a result - a bool return here
		// would just be dead weight nobody reads.
		void requireScalarValue(const ast::Type* type, support::SourceLocation loc);

		void collectLabelBlocks(ast::Stmt* stmt);
		void collectSwitchCases(ast::Stmt* stmt, std::vector<std::pair<i64, BasicBlock*>>& cases, BasicBlock*& defaultBlock);
		// A small compile-time integer constant evaluator - deliberately duplicates
		// Sema::evalConstantExpr's shape (sema.cpp) rather than reusing it: that method needs a live
		// Sema instance with an in-progress Scope stack, which does not exist once check() has
		// returned (see the header comment above), and this evaluator reads IrBuilder's own
		// LocalSymbol table instead.
		std::optional<i64> foldConstant(ast::Expr* expr);

		// Jumps into `block` from the current block first, if it is not already terminated (the
		// fallthrough case: a switch case/default label, or a goto label, reached by falling off the
		// end of the previous statement) - then makes `block` the current block.
		void switchToBlock(BasicBlock& block, support::SourceLocation loc);

		void emitVoid(support::SourceLocation loc, IrInstrPayload payload);
		IrValue emitConstInt(support::SourceLocation loc, i64 value);
		IrValue emitConstFloat(support::SourceLocation loc, f32 value);
		void emitConstInto(support::SourceLocation loc, IrValue result, i64 value); // reuses a temp id already allocated - see materializeBoolean()
		void emitCopyInto(support::SourceLocation loc, IrValue result, IrValue source, bool isFloat = false); // ditto - see visit(TernaryExpr&)
		IrValue emitLoad(support::SourceLocation loc, IrValue address, IrMemSize size, bool isFloat = false);
		void emitStore(support::SourceLocation loc, IrValue address, IrMemSize size, IrValue value, bool isFloat = false);
		IrValue emitFrameAddr(support::SourceLocation loc, u32 localIndex);
		IrValue emitGlobalAddr(support::SourceLocation loc, std::string_view name);
		IrValue emitBinOp(support::SourceLocation loc, IrBinOp op, IrValue lhs, IrValue rhs, bool isUnsigned, bool isFloat = false);
		IrValue emitUnOp(support::SourceLocation loc, IrUnOp op, IrValue operand, bool isFloat = false, bool isUnsigned = false);

		// Copies `text` into `_arena` and returns a stable string_view over that copy - used to
		// synthesize a string literal's label (".str0", ...), which needs a lifetime outliving the
		// single visit(StringLiteralExpr&) call that creates it.
		std::string_view internLabel(std::string_view text);

		static IrMemSize memSizeOf(const ast::Type* type) noexcept;
	};
}
