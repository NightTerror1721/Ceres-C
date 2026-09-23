#pragma once

#include <ceresc/ast/decl.h>
#include <ceresc/ast/expr.h>
#include <ceresc/codegen/casm_emitter.h>
#include <ceresc/codegen/frame_layout.h>
#include <ceresc/codegen/value_placement.h>
#include <ceresc/ir/ir_function.h>
#include <ceresc/support/diagnostics.h>
#include <ceresc/support/line_map.h>
#include <ceresc/support/optimization.h>
#include <ceresc/support/source_manager.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// CodeGen - IR -> CASM text, instruction by instruction (§10 of the architecture plan).
//
// The only library that knows the Ceres ABI, the real ISA and .casm syntax - everything upstream
// of it is target-agnostic.
//
// Where values live is decided by ValuePlacement (value_placement.h), not here: this class asks it
// for each value's home and emits accordingly, so the same emission code covers a value sitting in
// a register and one sitting in a frame field. r4-r7/r12 and f4-f5 follow §10's rule - r4/r5 (and
// f4/f5) stay pure scratch for moving a spilled value in and out while one instruction is
// translated, the rest are handed to values whose live range allows it.
//
// Four peepholes run here rather than over the IR, because each one is about the ISA rather than
// about the program (support::OptimizationOptions switches each off individually):
//
//   cmpBranchFusion    A Cmp feeding only a CondJump that tests it against zero is exactly what
//                      lowerCondition() emits for every relational condition (ir_builder.cpp). The
//                      pair becomes one `ifXX` over the comparison's own operands, instead of
//                      materializing a 0/1 value (four instructions, 06-Pseudo-Instructions.md's
//                      own shape for "a comparison used as a value") and then branching on it.
//   immediateOperands  A constant operand goes straight into the instruction - `add r4, r4, 1`,
//                      `ifls r4, 10, .L2` - which the assembler encodes as ADDI/CMPI on its own
//                      (05-Instruction-Set.md: it picks the form from the operand shapes).
//   fallthroughBranches A jump to the block that is about to be emitted next is dropped.
//   addressFolding     An `add` that exists only to compute the address a Load/Store immediately
//                      reads folds into that access's own operand: `[base + index]` (the ISA's
//                      indexed forms, LDRX/STRX at 0xB4-0xBD) or `[base + N]`. This is the one
//                      addressing mode the ISA has and the IR deliberately does not model (§9's
//                      Load/Store take a single address operand), so recognizing it here is exactly
//                      the kind of ISA-shaped rewrite this file owns - and it is what makes walking
//                      an array cost one instruction per element instead of two, which
//                      05-Instruction-Set.md's own indexed-addressing section is written to explain.
//
// With all of them off (-O0), every comparison materializes, every constant is materialized into a
// register first, every address is computed into a register of its own, and every branch is written
// out - the simplified, uniform shape the golden tests still pin on the other side. See
// support/optimization.h for why that path stays alive.
//
// Implemented in Fase 6 (scalar expressions/functions/simple scalar globals) and Fase 7 (arrays,
// pointers, structs) of the phased plan (§13); the optimizations are §13's own Fase 9 list.

namespace ceresc::codegen
{
	// One symbol this translation unit publishes: what another unit has to be TOLD about it before
	// it can refer to it.
	//
	// The assembler picks an opcode from the shape of an operand - `mov r1, counter` encodes
	// differently depending on what `counter` is (25-Separate-Compilation.md) - so an object cannot
	// simply reference a name it has never seen declared. libs/driver collects these from every unit
	// into one declarations file that each generated .casm imports, which is what makes a call or a
	// global read cross a file boundary at all. See docs/07-CASM-Interop.md.
	struct ExternalDeclaration
	{
		std::string name;
		bool isFunction = false;
		bool isDefinition = false;
		bool hasInitializer = false;
		std::string section;   // "@text", "@data", "@bss" or "@rodata" - where the real definition lives
		std::string typeText;  // the CASM type for a variable ("u32", "u8[8]", ...); empty for a function
	};

	class CodeGen
	{
	public:
		CodeGen(const support::SourceManager& sourceManager, support::DiagnosticEngine& diagnostics,
			const support::OptimizationOptions& options) noexcept :
			_sourceManager(sourceManager), _diagnostics(diagnostics), _options(options)
		{}
		CodeGen(const CodeGen&) = delete;
		CodeGen(CodeGen&&) = delete;
		~CodeGen() = default;

		CodeGen& operator=(const CodeGen&) = delete;
		CodeGen& operator=(CodeGen&&) = delete;

	public:
		// Where the lines the locations name really came from, when the text this was compiled from
		// was produced by the preprocessor rather than typed. Without it every trailing comment
		// cites a line of the EXPANDED buffer - which for a file with no `#include` is the same
		// number by construction (a directive leaves its blank line behind), and for one with a
		// header is off by the whole length of that header and names the wrong file besides.
		//
		// Optional because the phases below libs/driver are driven straight from a string: a test
		// that compiles "int f(void) { return 1; }" has no preprocessor and nothing to map.
		void setLineMap(const support::LineMap* lineMap) noexcept { _lineMap = lineMap; }

		// Generates the whole .casm text for `unit`/`module` - `unit` supplies function signatures/
		// parameter names/global VarDecls (which the IR never models on purpose, see IrModule's own
		// header comment); `module` supplies the lowered function bodies. Both must come from the
		// same already-sema-checked TranslationUnit and the IrBuilder::build() call over it.
		std::string generate(const ast::TranslationUnit& unit, const ir::IrModule& module);

		// Every externally visible symbol `unit` defines or declares, in the form another object
		// needs in order to name it. Reads the AST only - no code generation - so it can be called
		// before, after or instead of generate().
		std::vector<ExternalDeclaration> collectExternalDeclarations(const ast::TranslationUnit& unit) const;

	private:
		// Reports every C symbol whose name the assembler cannot read as an identifier. A C symbol
		// keeps its own name in the generated CASM (see mangledName()'s note, codegen.cpp) - that is
		// what makes a routine written in CASM callable from C under one name rather than two - and
		// this is the price: the handful of CeresASM reserved words become names a C program may not
		// give a function, a global or a static local. Reported here, once, before anything is
		// emitted, so the message names the C declaration instead of an assembler syntax error
		// pointing at generated text.
		void checkSymbolNames(const ast::TranslationUnit& unit, const ir::IrModule& module);

		// `symbolName` is what the CASM `let` is called: the C name for an ordinary global, and the
		// function-qualified one for a `static` local (ir_function.h's IrStaticLocal). `exported`
		// adds the `global` keyword that publishes it to the linker - C's external linkage, which a
		// `static` of either kind does not have.
		void generateGlobal(const ast::VarDecl& decl, std::string_view symbolName, bool exported);
		// The `let` declaration for one global of aggregate type - an array or a struct, which has
		// no single machine width to declare and so needs its own CASM spelling:
		//
		//   - an array whose (innermost) element is a scalar keeps its real shape, `u32[4]` /
		//     `u32[2][3]` / `u8[8]` (11-Data-Types-and-Literals.md), so the .casm reads the way the
		//     C declaration did and the assembler gives it the element type's own alignment;
		//   - anything involving a struct becomes one flat `u32[N]` of the right byte count. NOT a
		//     CASM `struct` plus `let g: Point`: 23-Structs.md's struct-as-a-type spelling means
		//     `u8[Point]`, whose alignment is one byte, so a word field of it can land misaligned
		//     and fault at run time. A u32 array is the same bytes with the alignment the object
		//     actually needs, and the field offsets are already baked into the IR by sema
		//     (type_layout.h) rather than looked up from a CASM struct, so nothing is lost but the
		//     field names - which the emitted comment puts back.
		void generateAggregateGlobal(const ast::VarDecl& decl, std::string_view symbolName, bool exported);
		// The CASM type text for an array of scalars - `u32[2][3]` for an `int[2][3]`. Empty when
		// `type` is not that shape (i.e. a struct is involved somewhere), which is the caller's
		// signal to fall back to the flat word array.
		static std::string scalarArrayTypeName(const ast::Type* type);
		// The `[...]` initializer text for an array of scalars, built straight from the AST so the
		// output keeps the shape the C initializer had. Empty when some element is not a
		// compile-time constant - the caller diagnoses that, since only it knows the variable's name.
		// `outOffender` is the sub-expression that could not be written, so the caller - which is
		// the one that knows the variable's name - can say whether the program asked for something
		// impossible (an address) or for something that is simply not constant.
		std::optional<std::string> scalarArrayInitText(const ast::Type* type, const ast::Expr* init,
			const ast::Expr*& outOffender) const;
		// The little-endian byte image of a constant initializer for an object of `type`, written
		// into `image` at `offset`. The only way to get a struct into .data: field offsets come from
		// sema's own layout (type_layout.h), so the image is exactly the memory the running program
		// will address. False when some element is not a compile-time constant.
		//
		// A pointer field is not a byte value: it is the ADDRESS of a symbol, which nothing knows
		// until the link. Such a field is recorded in `words` (which 4-byte word of the image, and
		// which symbol) and left as zero here; the caller writes the symbol's name into that word of
		// the emitted `u32[N]` initializer instead of the hex the bytes would have spelled.
		struct WordReference { u32 wordIndex; std::string symbol; };
		bool buildGlobalImage(const ast::Type* type, const ast::Expr* init, u32 offset, std::vector<u8>& image,
			std::vector<WordReference>& words, const ast::Expr*& outOffender) const;
		// The CASM symbol name an address-constant expression means - a string literal's synthesized
		// label, an explicit `&x` target's name, or an array/function name that decays to one. Empty
		// when `expr` is not an address constant.
		std::optional<std::string> addressConstantSymbol(const ast::Expr* expr) const;
		// The CASM symbol a C name means in a static initializer: the function-qualified symbol for a
		// `static` local, or the mangled name otherwise.
		std::string symbolForName(std::string_view name) const;
		// The name the assembler sees for a C symbol: its `__asm__("label")` when it has one, `__c_` and the name
		// when the name is a word CeresASM reserves, and the name itself otherwise.
		std::string casmName(std::string_view name) const;
		// Emits the .rodata `let` for every string literal that appeared only in a static
		// initializer, in first-sight order. The IR's own string literals are emitted separately.
		void emitInitializerStringLiterals();
		// The one diagnostic both of the above feed: what `outOffender` was, said as precisely as
		// this back end can say it.
		void reportUnrepresentableInitializer(const ast::VarDecl& decl, const ast::Expr* offender);
		void generateFunction(const ast::FunctionDecl& decl, const ir::IrFunction& function);
		void generateStringLiterals(const ir::IrModule& module);
		// Emits the `.rodata` jump tables a lowered `switch` (ir::IrOpcode::TableJump) asked for, one
		// `let __ccjt_<func>_<n>: u32[N] = [...]` per table. Emitted as a second `@rodata` block after
		// all the `@text`, once every block label the entries name has been defined.
		void emitJumpTables();
		// The label naming a basic block. `.L<id>` (a CASM local label, scoped to the function's own
		// global label) for an ordinary function; a file-scope `__ccbb_<func>_<id>` for one that has
		// a jump table, because a `.rodata` table's initializer cannot name a local `.L` label at all
		// (the assembler rejects a leading `.` in a constant expression) and the table is emitted
		// outside the function's local-label scope besides.
		std::string blockLabel(const ir::BasicBlock& block) const;

		// One IR instruction at `instrs[index]`. Takes the whole block (rather than just the one
		// instruction) because Call needs to look backward at its own Param instructions to assign
		// argument registers/stack slots, and CondJump needs to look backward at the Cmp it may be
		// able to fuse with. `nextBlockId` is what the fallthrough peephole compares a branch target
		// against; instructions an earlier fusion consumes are marked in `_skipInstr` up front.
		void generateInstr(std::span<ir::IrInstr* const> instrs, usize index, u32 nextBlockId);

		// "file.c:12" from an IR instruction's/AST node's own SourceLocation - see CasmEmitter's own
		// header comment on why it doesn't do this itself.
		std::string sourceComment(support::SourceLocation location) const;

		// A frame field's name inside the current function's frame struct. Generic (`slot3`) rather
		// than the original C name: a field can be shared by several values whose live ranges do not
		// overlap (value_placement.h), so no single source name describes it.
		static std::string slotFieldName(u32 slotIndex);
		// The CASM type for a frame field of this size/bank. Multi-word slots use an aligned u32
		// array so the assembler reserves the whole object rather than only its first word.
		static std::string fieldTypeName(u32 sizeInBytes, bool isFloat);

		// The `[sp + Frame.slotN]` operand text for a frame field.
		// The operand that names frame slot `slotIndex`. Usually `[sp + Frame.field]`; for a slot past
		// what a 16-bit displacement reaches it first emits the two instructions that put the slot's
		// address in `at` (r13, the assembler temporary, which nothing here allocates) and names `[at + 0]`.
		// So it must be called while building the instruction that uses it, and not twice for one use.
		std::string slotAddress(u32 slotIndex);

		// ---- operand access, placement-aware ----------------------------------------------------
		//
		// `valueIn` answers "which register holds this value right now", loading it into `scratch`
		// first when it lives in a frame field. `defineInto` answers the mirror question for a
		// result: the register to compute into, with storeResult() writing it back afterwards when
		// that register was only scratch.
		std::string valueIn(ir::IrValue value, u32 scratch, bool isFloat, support::SourceLocation loc);
		std::string defineInto(ir::IrValue value, u32 scratch, bool isFloat);
		void storeResult(ir::IrValue value, std::string_view reg, support::SourceLocation loc);

		// The register a local lives in, or empty when it lives in a frame field.
		std::optional<std::string> localRegister(u32 localIndex) const;

		// `li`/`la reg, value` - whichever fits (06-Pseudo-Instructions.md's "la with a literal"):
		// `li` only reaches an unsigned 16-bit immediate, `la` (lui+ori) reaches any 32-bit pattern,
		// negative values included.
		void emitLoadImmediate(std::string_view reg, i64 value, support::SourceLocation loc);

		// The constant an integer temporary was defined by, when the immediateOperands peephole may
		// use it directly as an instruction operand: it has to be a Const, defined exactly once, and
		// fit the 16-bit immediate field the encoding provides (04-Instruction-Format.md).
		std::optional<i64> immediateFor(ir::IrValue value) const;

		// Fills _suppressedConsts: the Const instructions whose every reader will take them as an
		// immediate, so materializing them into a register would leave a dead `li` behind. Called
		// once per function, before any instruction is emitted, because the decision needs to see
		// all of a constant's uses - not just the one currently being translated.
		void collectSuppressedConstants(const ir::IrFunction& function);

		// One IrCmpPredicate's ifXX mnemonic (06-Pseudo-Instructions.md's table): the signed
		// (ifeq/ifne/ifgr/ifge/ifls/ifle) or unsigned (ifab/ifae/ifbl/ifbe) spelling, chosen by
		// `isUnsigned` - Eq/Ne have only one spelling either way.
		static std::string_view ifMnemonic(ir::IrCmpPredicate predicate, bool isUnsigned);
		// The same predicate with its sense inverted - what the fused branch needs when it can only
		// jump to the false target directly.
		static ir::IrCmpPredicate invertPredicate(ir::IrCmpPredicate predicate);

		// Materializes a Cmp's predicate/operands as a real 0/1 value in `resultReg` (an int
		// register) - the four-instruction `ifXX`/`li`/`jp`/`li` shape 06-Pseudo-Instructions.md
		// itself describes for "a comparison used as a value".
		void materializeCmp(const ir::IrCmpPayload& payload, std::string_view resultReg, support::SourceLocation loc);

		// Emits one `ifXX a, b, .Ltarget` for a comparison, using an immediate second operand when
		// the peephole allows it.
		void emitConditionalBranch(ir::IrCmpPredicate predicate, bool isUnsigned, bool isFloat,
			ir::IrValue lhs, ir::IrValue rhs, std::string_view target, support::SourceLocation loc);

		// True when `instrs[index]` is a CondJump that tests, against a constant zero, the result of
		// the Cmp immediately before it - the shape cmpBranchFusion collapses. `cmpOut` receives
		// that Cmp's payload.
		bool findFusableCmp(std::span<ir::IrInstr* const> instrs, usize index, const ir::IrCmpPayload*& cmpOut) const;

		// True when `instrs[index]` is a Call that is immediately followed by a Return of exactly
		// its result - the shape a tail call collapses (`return f(args)`). The returned payload is
		// that Return. The condition is deliberately conservative:
		//   - a direct call to a named function (an indirect target or inline asm is left alone);
		//   - every argument fits in an argument register, so no outgoing stack word has to be
		//     written into the caller's frame after `leave` would have destroyed it;
		//   - the call's result is read only by the Return, and the two agree on the bank;
		//   - not `main` (its Return is the shutdown sequence, not a `ret`) and not an interrupt
		//     handler (its Return is an `iret`).
		// The caller emits the argument moves, then the ordinary epilogue with a `jp` in place of
		// `ret`, and marks the Return consumed. The `jp` needs no return address of its own: the
		// caller's own return address is already where the callee's `ret` will pop it.
		const ir::IrReturnPayload* findTailCall(std::span<ir::IrInstr* const> instrs, usize index) const;

		// ---- address folding (Fase 7) -----------------------------------------------------------

		// One absorbed address computation: the base register's value plus either another register's
		// value (the ISA's indexed forms) or a constant displacement - never both, because `rt` and
		// `imm16` are the same encoding bits (04-Instruction-Format.md).
		struct FoldedAddress
		{
			ir::IrValue base;
			ir::IrValue index;                // valid when the offset is a register
			std::optional<i64> displacement;  // set when the offset is a constant instead
		};

		// True when `instrs[index]` is a Load/Store whose address is produced by the `add`
		// IMMEDIATELY before it and read by nothing else, and folding that `add` into the access is
		// possible at all.
		//
		// Adjacency is what makes this safe without touching liveness, and it is worth being
		// explicit about why: with the `add` gone, its two operands have to still be in the
		// registers ValuePlacement put them in AT THE ACCESS, while ValuePlacement's own liveness
		// says their last read was the `add`. Nothing is emitted in between, so nothing can have
		// taken those registers - and the only thing allocated AT the access is a load's own
		// destination, which may safely reuse one, because every load form computes its address
		// before it writes rd (05-Instruction-Set.md's LDR/LDRX and the VM's own handlers). Teaching
		// liveness about non-adjacent folds instead was tried and is a pessimization: extending the
		// base's live range costs a register, and the value being stored then spills to the very
		// frame field the fold was saving an instruction on.
		//
		// IrBuilder cooperates by lowering a plain assignment's VALUE before its target address
		// (ir_builder.cpp), which is what puts a store's address computation adjacent to it.
		//
		// The remaining condition is the register budget (§10: r4/r5 are the only scratch). An
		// indexed STORE reads three registers at once - base, index and value - so the one refused
		// case is the one where all three would have to be fetched from a frame field; it keeps the
		// plain `add`.
		//
		// Called from two places that must agree exactly: the pre-pass that marks the `add` as
		// consumed, and the Load/Store case that emits the folded form - same contract as
		// findFusableCmp() above.
		bool findFoldableAddress(std::span<ir::IrInstr* const> instrs, usize index, FoldedAddress& out) const;
		// The `[...]` operand text for a folded address, or for a plain one-register address.
		std::string addressOperand(const FoldedAddress& folded, u32 baseScratch, u32 indexScratch, support::SourceLocation loc);
		// True when this value has to be shuttled through a scratch register to be read at all -
		// i.e. it lives in a frame field rather than a register.
		bool livesInSlot(ir::IrValue value) const;

	private:
		const support::SourceManager& _sourceManager;
		const support::LineMap* _lineMap = nullptr; // nullable - see setLineMap()
		support::DiagnosticEngine& _diagnostics;
		support::OptimizationOptions _options;
		CasmEmitter _emitter;

		// String literals that appear only in a static initializer, never in a function body. The
		// IR has no entry for them (IrBuilder only lowers function bodies), so codegen mints the
		// .rodata labels itself on first sight and emits the bytes alongside the IR's own literals.
		// Mutable: the map is a cache populated while a const pass reads an initializer.
		mutable std::unordered_map<const ast::StringLiteralExpr*, std::string> _initializerStringNames;
		mutable std::vector<const ast::StringLiteralExpr*> _initializerStringOrder;
		mutable u32 _nextInitializerStringId = 0;

		// A `static` local's C name maps to its function-qualified CASM symbol ("count" ->
		// "main.count"). Populated in generate(), read by addressConstantSymbol() so that
		// `static int* p = &x;` names the real symbol rather than the bare C name.
		std::unordered_map<std::string_view, std::string> _staticLocalSymbols;

		// C name -> the label an `__asm__("...")` gave it. Populated in generate(), read by casmName().
		std::unordered_map<std::string_view, std::string> _asmLabels;

		// Per-function state, valid only while generateFunction() is on the stack.
		const ir::IrFunction* _function = nullptr;
		std::optional<ValuePlacement> _placement;
		std::string _frameName;       // "__frame_<function>", empty when this function has no frame fields
		bool _hasFrame = false;       // whether enter/leave bracket the body at all
		// How many stack words this function's own FIXED parameters arrived in. Only a variadic
		// function reads it (IrOpcode::VaStart): its argument tail starts at the first incoming stack
		// word the fixed parameters did not already take.
		u32 _fixedStackArgWords = 0;
		// Which callee-saved registers this function handed to a local (value_placement.h), and how
		// many words that is: what the prologue pushes and the epilogue pops, and how far the saved
		// copies shift the incoming stack-argument area - a stack parameter is read at
		// [fp + 8 + 4*_calleeSavedWords + ...], not [fp + 8 + ...].
		u32 _calleeSavedIntMask = 0;
		u32 _calleeSavedFloatMask = 0;
		u32 _calleeSavedWords = 0;
		u32 _nextComparisonLabel = 0; // uniquely names each materialized comparison's .cmpN_true/.cmpN_end pair
		// How often each temporary is read and written across the whole function - what the
		// peepholes consult before consuming an instruction (a Cmp may only be folded into the
		// branch that reads it if nothing ELSE reads it) and what immediateFor() uses to be sure a
		// constant really is one (IrBuilder reuses a temporary id for two definitions in
		// materializeBoolean()/visit(TernaryExpr&), and such a value is not a constant at all).
		std::unordered_map<u32, u32> _useCount;
		std::unordered_map<u32, u32> _defCount;
		// A lowered `switch` that became a jump table (ir::IrOpcode::TableJump). While generating a
		// function that has one, every block label is file-scope (`_blockLabelPrefix` + id) so the
		// `.rodata` table can name it - a table's initializer cannot use a `.L` local label. Each
		// table is recorded here as its final label text and emitted after all the `@text`.
		struct JumpTable
		{
			std::string label;
			std::vector<std::string> entries;
		};
		bool _globalBlockLabels = false;
		std::string _blockLabelPrefix;
		std::vector<JumpTable> _jumpTables;
		u32 _nextJumpTableId = 0;
		// Per-block: instructions an earlier peephole already consumed, so generateInstr() emits
		// nothing for them when the loop reaches them.
		std::vector<bool> _skipInstr;
		// Temporaries defined by a Const that every reader will fold into itself as an immediate -
		// see collectSuppressedConstants(). Emitting the `li` for one of these would produce a
		// register nothing ever reads.
		std::vector<bool> _suppressedConsts;
		// True while generating `main` specifically - see generateInstr()'s Return case. Nothing
		// ever reaches `main` through a real `call` (the VM sets the program counter straight to
		// its address at load time, 09-CRES-Binary-Format.md/12-Labels-and-Symbols.md), so there is
		// no return address on the stack for an ordinary `leave`/`ret` epilogue to find - `main`
		// must stop the machine itself instead (SystemControlDevice + `halt`,
		// 07-IO-Devices-and-Ports.md), exactly like every hand-written CeresASM program does. What
		// it returns is the exit status: bits 15:8 of the word it writes there.
		bool _generatingMain = false;
		// Where each frame slot sits inside the frame struct, computed the way the assembler lays the
		// struct out (each field at its own alignment, after the outgoing-argument words). Only
		// slotAddress() reads it, to notice a slot the displacement cannot reach.
		std::vector<u32> _slotOffsets;
		// The unit declares `void exit(int)`, so `main` ends by calling it with its own status.
		bool _mainCallsExit = false;

		// Set while generating a call to a loop-idiom routine (ir_optimizer.cpp's lowerFillIdioms),
		// so generate() emits that routine's body once at the end of `@text`. The routine is not in
		// the library and not in the AST - the compiler carries its own copy, and only when a call
		// site asked for it.
		bool _usesMemset = false;
		// Emits every loop-idiom routine a call site asked for, at the end of `@text`.
		void emitLoopIdiomRoutines();

		// True while generating an `__interrupt` handler. Two things change, and both follow from the
		// same fact: a handler is not called, it preempts. It ends in `iret`, which pops the flags and
		// PC the dispatcher pushed, rather than `ret`, which would pop a return address nobody wrote;
		// and it must hand every register back exactly as it found it, because the code it interrupted
		// never agreed to anything.
		//
		// The caller/callee split of 24-Calling-Convention.md does not help here. r0-r7, r12 and
		// f0-f7 are caller-saved because a CALLER saved them; a handler has no caller, so it saves
		// them itself. r8-r11 and f8-f15 are the callee-saved half, which the handler has to save
		// too once value_placement hands them out (Fase 1) - the interrupt save mask below is the
		// whole allocatable set for exactly that reason.
		bool _generatingInterrupt = false;

		// True when the handler being generated touches the float bank at all. The integer set goes
		// back in one `pushm`/`popm` pair whatever happens, so it is not worth deciding about; the
		// floats go back in one `fpushm`/`fpopm` pair now, but still worth not paying for a handler
		// that never looks at one.
		bool _interruptSavesFloats = false;

		// The float mask the interrupt prologue actually pushed - f0-f7 always, plus whatever f8-f15
		// value placement handed out - remembered so the epilogue pops exactly what the prologue
		// saved rather than recomputing it.
		u32 _interruptFloatMask = 0;

		// The whole integer set a generated body can write: r0-r7 (bits 0-7), r8-r11 (bits 8-11,
		// callee-saved) and r12 (bit 12). `pushm` stores from the highest set bit down and `popm`
		// reads back from r0, so the pair round-trips by construction (05-Instruction-Set.md).
		static constexpr u32 kInterruptSaveMask = 0x1FFF;
		// f0-f7 (caller-saved and scratch) are the whole float set a body can write except for the
		// callee-saved f8-f15, which only become reachable when value placement hands one out -
		// calleeSavedFloatMask(), OR-ed in by emitInterruptPrologue().
		static constexpr u32 kInterruptCallerSavedFloatMask = 0x00FF;

		// The two halves of that, emitted around the frame the ordinary prologue/epilogue open.
		void emitInterruptPrologue(const ir::IrFunction& function, std::string_view comment);
		void emitInterruptEpilogue(std::string_view comment);
	};
}
