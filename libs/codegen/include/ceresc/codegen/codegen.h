#pragma once

#include <ceresc/ast/decl.h>
#include <ceresc/codegen/casm_emitter.h>
#include <ceresc/codegen/frame_layout.h>
#include <ceresc/ir/ir_function.h>
#include <ceresc/support/diagnostics.h>
#include <ceresc/support/source_manager.h>
#include <span>
#include <string>
#include <string_view>

// CodeGen - IR -> CASM text, instruction by instruction (§10 of the architecture plan).
//
// The only library that knows the Ceres ABI, the real ISA and .casm syntax - everything upstream
// of it is target-agnostic.
//
// Register allocation rule, in full - simpler than §10's own prose describes, and deliberately so
// (see frame_layout.h's header comment for why): every local, parameter AND temporary lives in its
// own permanent frame field for the whole function, never in a register between IR instructions.
// r4-r7 (int) and f4-f5 (float) are used only as scratch registers while translating a single IR
// instruction - loaded from a slot's memory immediately before use, stored back immediately after
// being produced - so a `call` (which clobbers r0-r7, r12, f0-f7, at and the flags, per the ABI)
// can never lose a value nothing was resident in a register to hold onto. This costs more
// load/store traffic than the tighter version of the rule §10 describes, in exchange for needing
// no liveness analysis to get right - see frame_layout.h.
//
// Deliberately does NOT fuse a Cmp immediately followed by a CondJump comparing its result against
// zero into one `ifXX` (the shape lowerCondition()'s generic fallback always produces for a plain
// relational condition, see ir_builder.cpp) - every comparison materializes a real 0/1 value first
// (the four-instruction `ifXX`/`li`/`jp`/`li` shape 06-Pseudo-Instructions.md itself describes for
// "a comparison used as a value"), and every CondJump then branches on that value against zero.
// More verbose than hand-written CASM would be, never incorrect - a natural peephole for Fase 9
// once real programs are running against the simpler version.
//
// Implemented in Fase 6 (scalar expressions/functions/simple scalar globals) and Fase 7 (arrays,
// pointers, structs) of the phased plan (§13).

namespace ceresc::codegen
{
	class CodeGen
	{
	public:
		CodeGen(const support::SourceManager& sourceManager, support::DiagnosticEngine& diagnostics) noexcept :
			_sourceManager(sourceManager), _diagnostics(diagnostics)
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

		// One IR instruction at `instrs[index]`, appended to the function currently being generated
		// - see the .cpp for the full IrOpcode -> CASM mapping table (§10). Takes the whole block
		// (rather than just the one instruction) because Call needs to look backward at its own
		// Param instructions to assign argument registers/stack slots (see frame_layout.h's
		// assignArgSlots()) - a Param on its own emits nothing, all of its work happens once its
		// Call is reached.
		void generateInstr(std::span<ir::IrInstr* const> instrs, usize index);

		// "file.c:12" from an IR instruction's/AST node's own SourceLocation - see CasmEmitter's own
		// header comment on why it doesn't do this itself.
		std::string sourceComment(support::SourceLocation location) const;

		// A local's/temporary's field name inside the current function's frame struct - see
		// generateFunction()'s own note on why these are generic (`local3`/`t7`) rather than the
		// original C name.
		static std::string localFieldName(u32 slotIndex);
		static std::string tempFieldName(u32 tempId);
		// The CASM type for a frame field of this size/bank. Multi-word slots use an aligned u32
		// array so the assembler reserves the whole object rather than only its first word.
		static std::string fieldTypeName(u32 sizeInBytes, bool isFloat);

		// Loads temp `value`'s slot into scratch register `reg` (e.g. "r4"/"f4") - `reg`'s own bank
		// must match `isFloat`. A no-op for an invalid IrValue, which happens only for a Return's
		// own unused `value` field (a `return;` with no expression).
		void loadTemp(ir::IrValue value, std::string_view reg, bool isFloat, support::SourceLocation loc);
		// Stores scratch register `reg` into temp `value`'s slot.
		void storeTemp(ir::IrValue value, std::string_view reg, bool isFloat, support::SourceLocation loc);
		// The `[sp + Frame.field]` operand text for temp `value`'s slot.
		std::string tempAddress(ir::IrValue value) const;
		// The `[sp + Frame.field]` operand text for local/parameter slot `slotIndex`.
		std::string localAddress(u32 slotIndex) const;

		// `li`/`la reg, value` - whichever fits (06-Pseudo-Instructions.md's "la with a literal"):
		// `li` only reaches an unsigned 16-bit immediate, `la` (lui+ori) reaches any 32-bit pattern,
		// negative values included.
		void emitLoadImmediate(std::string_view reg, i64 value, support::SourceLocation loc);

		// One IrCmpPredicate's ifXX mnemonic (06-Pseudo-Instructions.md's table): the signed
		// (ifeq/ifne/ifgr/ifge/ifls/ifle) or unsigned (ifab/ifae/ifbl/ifbe) spelling, chosen by
		// `isUnsigned` - Eq/Ne have only one spelling either way.
		static std::string_view ifMnemonic(ir::IrCmpPredicate predicate, bool isUnsigned);

		// Materializes a Cmp's predicate/operands as a real 0/1 value in `resultReg` (an int
		// register) - the four-instruction `ifXX`/`li`/`jp`/`li` shape 06-Pseudo-Instructions.md
		// itself describes for "a comparison used as a value" (see the header comment on why this
		// class does not fuse a Cmp into a following CondJump instead).
		void materializeCmp(const ir::IrCmpPayload& payload, std::string_view resultReg, support::SourceLocation loc);

	private:
		const support::SourceManager& _sourceManager;
		support::DiagnosticEngine& _diagnostics;
		CasmEmitter _emitter;

		// Per-function state, valid only while generateFunction() is on the stack.
		std::string _frameName;      // "__frame_<function>", or empty when this function opened no fields at all (bare `enter`)
		u32 _outgoingSlotCount = 0;
		u32 _nextComparisonLabel = 0; // uniquely names each materialized comparison's .cmpN_true/.cmpN_end pair
		// True while generating `main` specifically - see generateInstr()'s Return case. Nothing
		// ever reaches `main` through a real `call` (the VM sets the program counter straight to
		// its address at load time, 09-CRES-Binary-Format.md/12-Labels-and-Symbols.md), so there is
		// no return address on the stack for an ordinary `leave`/`ret` epilogue to find - `main`
		// must stop the machine itself instead (SystemControlDevice + `halt`,
		// 07-IO-Devices-and-Ports.md), exactly like every hand-written CeresASM program does.
		bool _generatingMain = false;
	};
}
