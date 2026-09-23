#pragma once

#include <ceresc/ast/ast_visitor.h>
#include <ceresc/ir/ir_function.h>
#include <ceresc/support/arena.h>
#include <ceresc/support/diagnostics.h>
#include <ceresc/support/optimization.h>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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
// ---- how a struct-typed expression is represented ---------------------------------------------
//
// The IR has no whole-struct value: §9's opcode table has no Load/Store that moves more than one
// machine word, and adding one would mean teaching every consumer (the printer, the optimizer's
// alias analysis, codegen's liveness) about a value that does not fit a register. So instead,
// **lowering a struct-typed expression yields the ADDRESS of its storage**, never a loaded value -
// exactly the convention an array-typed expression already follows (see lowerRValue()). A struct is
// only ever *moved* by emitMemoryCopy(), which lowers to as many ordinary Load/Store pairs as the
// type's own size and alignment call for; there is no memcpy to call (Ceres's stdlib/ has none yet,
// see §14), and the sizes are compile-time constants anyway, so the copy is always unrolled.
//
// The struct calling convention follows from that, and is derived from the TYPE alone on both ends
// of a call - IrBuilder never resolves a callee's declaration (see above), so caller and callee
// cannot disagree about it:
//
//   - A struct of exactly 1, 2 or 4 bytes travels in a register, as one byte/half/word: returned in
//     ret0, passed in an ordinary argument register. One load at one end, one store at the other.
//   - Every other struct (3 bytes, or more than 4) travels through memory:
//       * returning one adds a HIDDEN FIRST ARGUMENT holding the destination address (§10's
//         "puntero oculto en arg0, argumentos visibles corridos uno"). The callee copies its result
//         there and returns that same pointer in ret0, so nothing about the visible signature has to
//         change for a caller that ignores the result.
//       * passing one passes the address of a COPY the caller made in its own frame - by-value
//         semantics without a by-value register class. The callee's parameter slot holds that
//         pointer, and reading the parameter's address means loading it (see LocalSymbol::isIndirect).
//
// A struct-returning call always lands in a compiler temp slot first, and the caller then copies it
// where it belongs - so `struct P q = make();` copies twice. Eliding the second copy needs the
// destination to be threaded down into the call, which is real plumbing for a case this version has
// no measurements about: it stays a Fase 9 item alongside the rest of §13's optimization list.
//
// Implemented in Fase 5 of the phased plan (§13); struct/array/pointer memory in Fase 7. IrPrinter
// (--emit-ir) ships alongside it.

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
		void visit(ast::AlignofExpr& node) override;
		void visit(ast::VaExpr& node) override;
		void visit(ast::GenericSelectionExpr& node) override;
		void visit(ast::MachineOpExpr& node) override;
		void visit(ast::BuiltinExpr& node) override;
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
		void visit(ast::DesignatedInitExpr& node) override;
		void visit(ast::AsmStmt& node) override;
		void visit(ast::CompoundLiteralExpr& node) override;
		void visit(ast::StaticAssertDecl& node) override;
		void visit(ast::TranslationUnit& node) override;

	private:
		// Marks the functions whose address a global's or a static local's initializer holds, so that
		// unused-function elimination keeps them. Run once, after everything is lowered.
		void markFunctionsNamedByData(ast::TranslationUnit& unit);

		enum class LocalSymbolKind : u8 { Local, Global, EnumConstant };

		// A name IrBuilder knows how to lower a reference to - see the header comment above for why
		// this is not sema::Symbol.
		struct LocalSymbol
		{
			LocalSymbolKind kind = LocalSymbolKind::Local;
			u32 localSlot = 0; // Local (a frame slot index - see IrFunction::newLocalSlot())
			i64 enumValue = 0; // EnumConstant (already folded - see foldConstant())
			// Local only: the slot holds a POINTER to the object rather than the object itself -
			// a by-value struct parameter that arrived as the address of the caller's copy (see the
			// struct-convention note in the header comment above). Taking such a parameter's address
			// means loading that pointer, not computing a FrameAddr.
			bool isIndirect = false;
			// Global only: the file-scope symbol this name really refers to, when that is not the name
			// itself. Set for a `static` local, whose CASM symbol carries its function's name so two
			// functions can each have their own `count` (see IrStaticLocal, ir_function.h). Empty means
			// "the C name, verbatim", which is every ordinary global.
			std::string_view globalName;
		};

	private:
		support::Arena& _arena;
		support::DiagnosticEngine& _diagnostics;
		support::OptimizationOptions _options;

		IrModule _module;
		IrFunction* _currentFunction = nullptr;
		BasicBlock* _currentBlock = nullptr;
		IrValue _lastValue{}; // set by every visit(SomeExpr&), read back by lowerExpr() - same idiom as Sema::_lastExprType

		// F3.1b: 64-bit integers are lowered as an 8-byte addressed value (see the wide-integer
		// helpers below), but a few operations still have no representation (mul/div/mod and shifts,
		// float conversions, the 64-bit calling convention). The first of those to reach lowering is
		// reported once, as E5002, instead of being miscompiled. Cleared by build(); see
		// rejectWideFeature().
		bool _reportedWideInteger = false;

		std::vector<std::unordered_map<std::string_view, LocalSymbol>> _scopes; // function-local block scopes; empty at file scope
		std::unordered_map<std::string_view, LocalSymbol> _globalSymbols;      // file-scope variables + file-scope enum constants

		// Every function the translation unit declares OR defines, by name - filled up front in
		// build() so a call can be lowered before the callee's own declaration has been visited.
		// Used for one thing: converting each argument to its parameter's declared type, which is
		// what keeps a narrow parameter's value narrowed when it never reaches memory (see
		// convertForStore()).
		std::unordered_map<std::string_view, const ast::FunctionDecl*> _functionDecls;

		// Every file-scope symbol a `static` local has already been given, so a second one wanting the
		// same `function__name` gets a suffix instead of silently sharing storage with it.
		std::set<std::string, std::less<>> _staticLocalNames;

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

		// The frame slot holding the hidden destination pointer, for a function that returns a
		// struct through memory - empty for every other function. Set by visit(FunctionDecl&),
		// read by visit(ReturnStmt&). See the struct-convention note in the header comment.
		std::optional<u32> _hiddenReturnSlot;

	private:
		// Reserves a frame slot for one local VarDecl: a slot a closed sibling scope left behind
		// when options().localSlotReuse allows it, a brand new one otherwise. Either way the slot
		// is recorded against the innermost open scope, so it returns to the pool when that scope
		// closes - see _scopeSlots/_freeLocalSlots.
		u32 newLocalSlotFor(u32 sizeInBytes, bool isFloat, bool isVolatile = false, bool preferRegister = false, bool isRestrict = false);

		void pushScope();
		void popScope();
		void declareSymbol(std::string_view name, const LocalSymbol& symbol);
		LocalSymbol* lookupSymbol(std::string_view name) noexcept;

		// Reports E5002 the first time a 64-bit operation this phase does not implement would be
		// lowered (F3.1b). A no-op for a supported one; see _reportedWideInteger.
		void rejectWideFeature(support::SourceLocation loc, std::string_view what);

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
		// True when `node` is one of the side-effect-free select idioms and lowers it to the matching
		// one-instruction builtin instead of the branch diamond: `a < b ? a : b` (any relational
		// operator, either arm order) becomes imin/umin or imax/umax by the result's own signedness,
		// and `x < 0 ? -x : x` becomes abs. Only matched when each arm is structurally the
		// comparison's own operand, so nothing is evaluated that was not already.
		bool tryLowerSelectIdiom(const ast::TernaryExpr& node);
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
		//
		// It also realizes the two integer-to-integer conversions C does define as real operations:
		// narrowing to a `char`/`short` (truncate, then re-extend by the target's own signedness)
		// and converting to `bool` (zero stays zero, anything else becomes one). Those used to be
		// left to whatever a later store happened to truncate, which is wrong the moment the value
		// never reaches memory - see IrUnOp::Narrow's own note (ir_instr.h). Every path that moves a
		// value into storage of a declared type goes through here: initialization, assignment,
		// compound assignment, `return`, an argument moving into a parameter, and an explicit cast.
		IrValue convertForStore(support::SourceLocation loc, IrValue value, const ast::Type* fromType, const ast::Type* toType);

		// ---- composite memory (Fase 7) --------------------------------------------------------
		//
		// True when a struct of this type travels through memory rather than in a register - see the
		// struct-convention note in the header comment above. False for every non-struct type.
		static bool isIndirectStruct(const ast::Type* type) noexcept;
		// True when `type` is a struct at all, i.e. when lowering an expression of it yields an
		// address rather than a value.
		static bool isStructType(const ast::Type* type) noexcept;

		// Copies `sizeInBytes` bytes from [sourceAddr] to [destAddr], unrolled into ordinary
		// Load/Store pairs of the widest piece `alignment` permits (word if 4-aligned, half if
		// 2-aligned, byte otherwise) - the only way a struct ever moves, see the header comment.
		// `alignment` is the TYPE's own alignment, so a struct of chars is copied byte by byte even
		// though its frame slot happens to be word-aligned: the same copy has to stay correct for a
		// pointer aimed anywhere, not just at a frame slot.
		void emitMemoryCopy(support::SourceLocation loc, IrValue destAddr, IrValue sourceAddr, u32 sizeInBytes, u32 alignment);
		// `base + offset` as an IrValue, or `base` itself when the offset is zero - the one bit of
		// address arithmetic every composite access below shares.
		IrValue offsetAddress(support::SourceLocation loc, IrValue base, u32 offset);
		// Writes `sizeInBytes` zero bytes at [destAddr + startOffset], same piece-size rule as
		// emitMemoryCopy() - what an initializer list shorter than its aggregate leaves behind (C
		// zero-fills the rest, and there is no memset to call either).
		void emitZeroFill(support::SourceLocation loc, IrValue destAddr, u32 startOffset, u32 sizeInBytes, u32 alignment);

		// Stores `init` into the object of type `type` living at [baseAddr + offset] - the lowering
		// counterpart to Sema::checkInitializer(), handling the same three initializer forms (a
		// brace list, a string literal for a char array, and an ordinary expression) and recursing
		// for a nested aggregate. Everything the list does not reach is zero-filled, so the object
		// is fully defined afterwards exactly as C promises.
		//
		// Every one of those stores is written out: there is no memset to call (§14 - Ceres's
		// stdlib/ has no mem.casm yet) and no loop is synthesized, so `int a[100] = { 1 };` really
		// does cost a hundred stores. That is the honest price of the simplification, and it is
		// visible in the emitted .casm rather than hidden behind a runtime call - the moment
		// stdlib/ grows a memset, this is the one place that has to change.
		void lowerInitializerInto(support::SourceLocation loc, IrValue baseAddr, u32 offset,
			const ast::Type* type, ast::Expr* init);

		// Reserves a fresh, unnamed frame slot to hold one struct temporary - a struct-returning
		// call's destination, or the caller's copy of a by-value struct argument. Deliberately not
		// newLocalSlotFor(): a compiler temp belongs to no lexical scope, so it must not be handed
		// to the scope-based slot-reuse pool that a declared local's slot goes through.
		u32 newStructTempSlot(u32 sizeInBytes);

		// ---- 64-bit integers (F3.1b) ----------------------------------------------------------
		//
		// A wide value lowers to the ADDRESS of its 8-byte storage, exactly like a struct or an
		// array (see the memory-valued convention in the header comment): the low word at +0 and the
		// high word at +4, little-endian, so the whole back end already understands it with no new
		// IR opcode. `long long` and `unsigned long long` differ only in how a scalar is extended
		// into the high word and how a comparison orders the two words.
		//
		// A wide operation materializes its operands (a scalar operand is extended into a fresh
		// temp) and writes the two result words into another temp whose address is the result.

		// The address of an 8-byte storage holding `value` as a 64-bit integer: `value` itself when
		// `fromType` is already wide, otherwise a fresh temp filled with the extended word.
		IrValue materializeWide(support::SourceLocation loc, IrValue value, const ast::Type* fromType);
		// Writes `value` (converted to a 64-bit integer) into the wide object at `destAddr`.
		void emitWideStore(support::SourceLocation loc, IrValue destAddr, IrValue value, const ast::Type* fromType, bool isVolatile);
		// One word of a wide value: the low word (`high` false) or the high word (`high` true).
		IrValue loadWideWord(support::SourceLocation loc, IrValue wideAddr, bool high, bool isVolatile);
		// A fresh 8-byte temp whose low/high words are the two given values.
		IrValue makeWideValue(support::SourceLocation loc, IrValue low, IrValue high);
		// add/sub/and/or/xor on two wide operands; a wide mul/div/mod/shift is refused (F3.2/F3.3).
		IrValue lowerWideArithmetic(support::SourceLocation loc, ast::BinaryOp op, const ast::Type* resultType,
			const ast::Type* lhsType, const ast::Type* rhsType, IrValue lhsVal, IrValue rhsVal);
		// A 0/1 word for `lhsAddr op rhsAddr`, comparing the high words first (signed or unsigned as
		// `isUnsigned` says) and the low words unsigned - the low half never carries the sign.
		IrValue lowerWideCompare(support::SourceLocation loc, ast::BinaryOp op, IrValue lhsAddr, IrValue rhsAddr, bool isUnsigned);
		// The low word of a wide value for a context that needs a scalar (an array index, a pointer
		// offset, a shift amount) - C converts each of those to `int`, so this is that truncation.
		// A scalar passes through unchanged.
		IrValue toWord(support::SourceLocation loc, IrValue value, const ast::Type* type);

		void collectLabelBlocks(ast::Stmt* stmt);
		void collectSwitchCases(ast::Stmt* stmt, std::vector<std::pair<i64, BasicBlock*>>& cases, BasicBlock*& defaultBlock);
		// The `switch` dispatch, when options().jumpTables lets it replace the plain comparison
		// chain: a dense case set becomes one IrTableJumpPayload (a jump table in `.rodata`), a
		// sparse-but-large one becomes a balanced tree of CondJumps, and a small one returns false so
		// visit(SwitchStmt&) emits the chain it always did. `isUnsigned` selects the ordering the
		// tree's `<` tests use (the table's own bounds check is unsigned either way).
		bool emitSwitchDispatch(support::SourceLocation loc, IrValue condValue, bool isUnsigned,
			const std::vector<std::pair<i64, BasicBlock*>>& cases, BasicBlock* defaultBlock, BasicBlock& exitBlock);
		// Recursive half of the binary-search dispatch: emits the tests for cases[begin, end) into
		// the current block, splitting at the median. `cases` is sorted by value.
		void emitSwitchTree(support::SourceLocation loc, IrValue condValue, bool isUnsigned,
			const std::vector<std::pair<i64, BasicBlock*>>& cases, usize begin, usize end, BasicBlock* fallback);
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
		// `isSigned` picks `ldrsb`/`ldrsh` over `ldrb`/`ldrh` for a Byte/Half load - it comes from the
		// loaded TYPE's own signedness, never from the address's. Use loadOfType() below wherever
		// there is a Type to ask; this raw form exists for the few loads that describe a machine word
		// rather than a C object (a one-register struct argument, a whole-object copy's pieces).
		IrValue emitLoad(support::SourceLocation loc, IrValue address, IrMemSize size, bool isFloat = false, bool isSigned = false,
			bool isVolatile = false);

		// emitLoad() for a value of a known C type: picks the width, the bank and the signedness
		// from `type` in one place, so a caller cannot get two of the three right and forget the
		// third. That is exactly how narrow signed loads came to be zero-extending.
		IrValue loadOfType(support::SourceLocation loc, IrValue address, const ast::Type* type);
		void emitStore(support::SourceLocation loc, IrValue address, IrMemSize size, IrValue value, bool isFloat = false,
			bool isVolatile = false);
		IrValue emitFrameAddr(support::SourceLocation loc, u32 localIndex);
		IrValue emitGlobalAddr(support::SourceLocation loc, std::string_view name);
		IrValue emitBinOp(support::SourceLocation loc, IrBinOp op, IrValue lhs, IrValue rhs, bool isUnsigned, bool isFloat = false);
		IrValue emitCmp(support::SourceLocation loc, IrCmpPredicate predicate, IrValue lhs, IrValue rhs, bool isUnsigned, bool isFloat = false);
		// One machine builtin as an instruction of its own - used by the overflow builtins, which
		// expand to arithmetic plus a multiply-high.
		IrValue emitBuiltin(support::SourceLocation loc, ast::Builtin builtin, IrValue a, IrValue b = IrValue{});
		IrValue emitUnOp(support::SourceLocation loc, IrUnOp op, IrValue operand, bool isFloat = false, bool isUnsigned = false);
		IrValue emitNarrow(support::SourceLocation loc, IrValue operand, IrMemSize size, bool isUnsigned);

		// Copies `text` into `_arena` and returns a stable string_view over that copy - used to
		// synthesize a string literal's label (".str0", ...), which needs a lifetime outliving the
		// single visit(StringLiteralExpr&) call that creates it.
		std::string_view internLabel(std::string_view text);

		static IrMemSize memSizeOf(const ast::Type* type) noexcept;
	};
}
