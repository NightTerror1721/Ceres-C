#pragma once

#include <ceresc/ast/decl.h>
#include <ceresc/codegen/casm_emitter.h>
#include <ceresc/codegen/frame_layout.h>
#include <ceresc/codegen/value_placement.h>
#include <ceresc/ir/ir_function.h>
#include <ceresc/support/diagnostics.h>
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
// Three peepholes run here rather than over the IR, because each one is about the ISA rather than
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
//
// With all of them off (-O0), every comparison materializes, every constant is materialized into a
// register first, and every branch is written out - the simplified, uniform shape the golden tests
// still pin on the other side. See support/optimization.h for why that path stays alive.
//
// Implemented in Fase 6 (scalar expressions/functions/simple scalar globals) and Fase 7 (arrays,
// pointers, structs) of the phased plan (§13); the optimizations are §13's own Fase 9 list.

namespace ceresc::codegen
{
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
		// Generates the whole .casm text for `unit`/`module` - `unit` supplies function signatures/
		// parameter names/global VarDecls (which the IR never models on purpose, see IrModule's own
		// header comment); `module` supplies the lowered function bodies. Both must come from the
		// same already-sema-checked TranslationUnit and the IrBuilder::build() call over it.
		std::string generate(const ast::TranslationUnit& unit, const ir::IrModule& module);

	private:
		void generateGlobal(const ast::VarDecl& decl);
		void generateFunction(const ast::FunctionDecl& decl, const ir::IrFunction& function);
		void generateStringLiterals(const ir::IrModule& module);

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
		std::string slotAddress(u32 slotIndex) const;

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

	private:
		const support::SourceManager& _sourceManager;
		support::DiagnosticEngine& _diagnostics;
		support::OptimizationOptions _options;
		CasmEmitter _emitter;

		// Per-function state, valid only while generateFunction() is on the stack.
		const ir::IrFunction* _function = nullptr;
		std::optional<ValuePlacement> _placement;
		std::string _frameName;       // "__frame_<function>", empty when this function has no frame fields
		bool _hasFrame = false;       // whether enter/leave bracket the body at all
		u32 _nextComparisonLabel = 0; // uniquely names each materialized comparison's .cmpN_true/.cmpN_end pair
		// How often each temporary is read and written across the whole function - what the
		// peepholes consult before consuming an instruction (a Cmp may only be folded into the
		// branch that reads it if nothing ELSE reads it) and what immediateFor() uses to be sure a
		// constant really is one (IrBuilder reuses a temporary id for two definitions in
		// materializeBoolean()/visit(TernaryExpr&), and such a value is not a constant at all).
		std::unordered_map<u32, u32> _useCount;
		std::unordered_map<u32, u32> _defCount;
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
		// 07-IO-Devices-and-Ports.md), exactly like every hand-written CeresASM program does.
		bool _generatingMain = false;
	};
}
