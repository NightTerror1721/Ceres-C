#include <ceresc/codegen/codegen.h>
#include <ceresc/ir/ir_builder.h>
#include <ceresc/parser/parser.h>
#include <ceresc/sema/sema.h>
#include <ceresc/lexer/lexer.h>
#include <ceresc/support/arena.h>
#include <ceresc/support/diagnostics.h>
#include <ceresc/support/source_manager.h>
#include <ceresc/support/string_pool.h>

#include "framework.h"

using namespace ceresc;

namespace
{
	// Parses, type-checks, lowers to IR and generates .casm text for `source` - the whole pipeline
	// short of --run, exercised end to end the same way the driver will (libs/driver, Fase 6).
	std::string generateCasm(std::string_view source)
	{
		support::SourceManager sourceManager;
		support::SourceId sourceId = sourceManager.registerBuffer("test.c", std::string(source));
		support::Arena arena;
		support::DiagnosticEngine diagnostics;
		support::StringPool pool;
		lexer::Lexer lexer(source, sourceId, diagnostics, pool);
		parser::Parser parser(lexer, arena, diagnostics);

		ast::TranslationUnit* unit = parser.parseTranslationUnit();
		CHECK(unit != nullptr);
		if (!unit)
			return "<parse-failed>";

		sema::Sema sema(arena, diagnostics);
		bool ok = sema.check(*unit);
		CHECK(ok);

		ir::IrBuilder builder(arena, diagnostics);
		ir::IrModule module = builder.build(*unit);

		codegen::CodeGen codeGen(sourceManager, diagnostics);
		return codeGen.generate(*unit, module);
	}
}

// ---- golden tests (§12 of the architecture plan) --------------------------------------------
//
// Every program below was also verified for real: assembled with the actual `ceres asm` and
// executed with the actual `ceres run` from the sibling CeresASM checkout (never linked against -
// see codegen.h's own header comment on why libs/driver only ever spawns it as a subprocess),
// poking CeresASM's TerminalDevice MMIO register directly (this project has no printf yet) to make
// the real computed VALUE observable, since `ceres run`'s own process exit code never carries one
// (verified against CeresASM's Ceres/libs/driver/src/machine_runner.cpp - there is no register-to-
// exit-code channel at all, which is also why `main` halts the machine instead of an ordinary
// `leave`/`ret` - see codegen.h). That live check is deliberately NOT re-run by these tests (it
// needs a sibling checkout, an external process and no fixed timeout guarantee - not what a unit
// suite should depend on); it is exactly what tests/e2e (§12) automates instead. What is captured
// here is that the resulting .casm text, byte for byte, is the one that was actually verified.
//
// This project does not implement §10's "no frame at all" leaf optimization (see frame_layout.h's
// header comment on why), so there is no "function without a frame" golden case to distinguish
// from "function with a frame" - every function gets one, uniformly.

TEST(codegen, returning_a_constant)
{
	CHECK_EQ(generateCasm("int main() { return 42; }"),
		"@text\n"
		"\n"
		"// main - test.c:1\n"
		"struct __frame_main\n"
		"    t0: u32\n"
		"endstruct\n"
		"global main:\n"
		"    enter __frame_main    // test.c:1\n"
		".L0:\n"
		"    li r4, 42             // test.c:1\n"
		"    str [sp + __frame_main.t0], r4 // test.c:1\n"
		"    ldr r0, [sp + __frame_main.t0] // test.c:1\n"
		"    leave                 // test.c:1\n"
		"    la r4, 0xFFFF0000     // test.c:1\n"
		"    li r5, 1              // test.c:1\n"
		"    strb [r4 + 0], r5     // test.c:1\n"
		"    halt                  // test.c:1\n");
}

TEST(codegen, arithmetic_reloads_every_operand_from_its_own_frame_slot)
{
	// Every local (including the first two parameters, which arrive in r0/r1) is copied into its
	// own frame field in the prologue - see codegen.h's own note on why nothing is ever kept
	// resident in a register between IR instructions, even within one function.
	CHECK_EQ(generateCasm("int add(int a, int b) { return a + b; }"),
		"@text\n"
		"\n"
		"// add - test.c:1\n"
		"struct __frame_add\n"
		"    local0: u32\n"
		"    local1: u32\n"
		"    t0: u32\n"
		"    t1: u32\n"
		"    t2: u32\n"
		"    t3: u32\n"
		"    t4: u32\n"
		"endstruct\n"
		"cc_add:\n"
		"    enter __frame_add     // test.c:1\n"
		"    str [sp + __frame_add.local0], r0 // test.c:1\n"
		"    str [sp + __frame_add.local1], r1 // test.c:1\n"
		".L0:\n"
		"    la r4, [sp + __frame_add.local0] // test.c:1\n"
		"    str [sp + __frame_add.t0], r4 // test.c:1\n"
		"    ldr r4, [sp + __frame_add.t0] // test.c:1\n"
		"    ldr r5, [r4]          // test.c:1\n"
		"    str [sp + __frame_add.t1], r5 // test.c:1\n"
		"    la r4, [sp + __frame_add.local1] // test.c:1\n"
		"    str [sp + __frame_add.t2], r4 // test.c:1\n"
		"    ldr r4, [sp + __frame_add.t2] // test.c:1\n"
		"    ldr r5, [r4]          // test.c:1\n"
		"    str [sp + __frame_add.t3], r5 // test.c:1\n"
		"    ldr r4, [sp + __frame_add.t1] // test.c:1\n"
		"    ldr r5, [sp + __frame_add.t3] // test.c:1\n"
		"    add r4, r4, r5        // test.c:1\n"
		"    str [sp + __frame_add.t4], r4 // test.c:1\n"
		"    ldr r0, [sp + __frame_add.t4] // test.c:1\n"
		"    leave                 // test.c:1\n"
		"    ret                   // test.c:1\n");
}

TEST(codegen, comparison_used_as_a_value_materializes_with_ifxx_li_jp_li)
{
	CHECK_EQ(generateCasm("int lessThan(int a, int b) { return a < b; }"),
		"@text\n"
		"\n"
		"// lessThan - test.c:1\n"
		"struct __frame_lessThan\n"
		"    local0: u32\n"
		"    local1: u32\n"
		"    t0: u32\n"
		"    t1: u32\n"
		"    t2: u32\n"
		"    t3: u32\n"
		"    t4: u32\n"
		"endstruct\n"
		"cc_lessThan:\n"
		"    enter __frame_lessThan // test.c:1\n"
		"    str [sp + __frame_lessThan.local0], r0 // test.c:1\n"
		"    str [sp + __frame_lessThan.local1], r1 // test.c:1\n"
		".L0:\n"
		"    la r4, [sp + __frame_lessThan.local0] // test.c:1\n"
		"    str [sp + __frame_lessThan.t0], r4 // test.c:1\n"
		"    ldr r4, [sp + __frame_lessThan.t0] // test.c:1\n"
		"    ldr r5, [r4]          // test.c:1\n"
		"    str [sp + __frame_lessThan.t1], r5 // test.c:1\n"
		"    la r4, [sp + __frame_lessThan.local1] // test.c:1\n"
		"    str [sp + __frame_lessThan.t2], r4 // test.c:1\n"
		"    ldr r4, [sp + __frame_lessThan.t2] // test.c:1\n"
		"    ldr r5, [r4]          // test.c:1\n"
		"    str [sp + __frame_lessThan.t3], r5 // test.c:1\n"
		"    ldr r4, [sp + __frame_lessThan.t1] // test.c:1\n"
		"    ldr r5, [sp + __frame_lessThan.t3] // test.c:1\n"
		"    ifls r4, r5, .cmp0_true // test.c:1\n"
		"    li r4, 0              // test.c:1\n"
		"    jp .cmp0_end          // test.c:1\n"
		".cmp0_true:\n"
		"    li r4, 1              // test.c:1\n"
		".cmp0_end:\n"
		"    str [sp + __frame_lessThan.t4], r4 // test.c:1\n"
		"    ldr r0, [sp + __frame_lessThan.t4] // test.c:1\n"
		"    leave                 // test.c:1\n"
		"    ret                   // test.c:1\n");
}

TEST(codegen, recursive_call_needs_no_extra_save_because_every_local_already_lives_in_memory)
{
	// `n` (frame slot local0) survives the recursive call for free: it was never in a register to
	// begin with, unlike the hand-written CeresASM version of this same function
	// (24-Calling-Convention.md's own worked example), which has to save `r8` explicitly for
	// exactly this reason. That's the trade this project's simpler rule makes - see codegen.h.
	CHECK_EQ(generateCasm("int factorial(int n) { if (n <= 1) return 1; return n * factorial(n - 1); }"),
		"@text\n"
		"\n"
		"// factorial - test.c:1\n"
		"struct __frame_factorial\n"
		"    local0: u32\n"
		"    t0: u32\n"
		"    t1: u32\n"
		"    t2: u32\n"
		"    t3: u32\n"
		"    t4: u32\n"
		"    t5: u32\n"
		"    t6: u32\n"
		"    t7: u32\n"
		"    t8: u32\n"
		"    t9: u32\n"
		"    t10: u32\n"
		"    t11: u32\n"
		"    t12: u32\n"
		"    t13: u32\n"
		"endstruct\n"
		"cc_factorial:\n"
		"    enter __frame_factorial // test.c:1\n"
		"    str [sp + __frame_factorial.local0], r0 // test.c:1\n"
		".L0:\n"
		"    la r4, [sp + __frame_factorial.local0] // test.c:1\n"
		"    str [sp + __frame_factorial.t0], r4 // test.c:1\n"
		"    ldr r4, [sp + __frame_factorial.t0] // test.c:1\n"
		"    ldr r5, [r4]          // test.c:1\n"
		"    str [sp + __frame_factorial.t1], r5 // test.c:1\n"
		"    li r4, 1              // test.c:1\n"
		"    str [sp + __frame_factorial.t2], r4 // test.c:1\n"
		"    ldr r4, [sp + __frame_factorial.t1] // test.c:1\n"
		"    ldr r5, [sp + __frame_factorial.t2] // test.c:1\n"
		"    ifle r4, r5, .cmp0_true // test.c:1\n"
		"    li r4, 0              // test.c:1\n"
		"    jp .cmp0_end          // test.c:1\n"
		".cmp0_true:\n"
		"    li r4, 1              // test.c:1\n"
		".cmp0_end:\n"
		"    str [sp + __frame_factorial.t3], r4 // test.c:1\n"
		"    li r4, 0              // test.c:1\n"
		"    str [sp + __frame_factorial.t4], r4 // test.c:1\n"
		"    ldr r4, [sp + __frame_factorial.t3] // test.c:1\n"
		"    ldr r5, [sp + __frame_factorial.t4] // test.c:1\n"
		"    ifne r4, r5, .L1      // test.c:1\n"
		"    jp .L2                // test.c:1\n"
		".L1:\n"
		"    li r4, 1              // test.c:1\n"
		"    str [sp + __frame_factorial.t5], r4 // test.c:1\n"
		"    ldr r0, [sp + __frame_factorial.t5] // test.c:1\n"
		"    leave                 // test.c:1\n"
		"    ret                   // test.c:1\n"
		".L2:\n"
		"    la r4, [sp + __frame_factorial.local0] // test.c:1\n"
		"    str [sp + __frame_factorial.t6], r4 // test.c:1\n"
		"    ldr r4, [sp + __frame_factorial.t6] // test.c:1\n"
		"    ldr r5, [r4]          // test.c:1\n"
		"    str [sp + __frame_factorial.t7], r5 // test.c:1\n"
		"    la r4, [sp + __frame_factorial.local0] // test.c:1\n"
		"    str [sp + __frame_factorial.t8], r4 // test.c:1\n"
		"    ldr r4, [sp + __frame_factorial.t8] // test.c:1\n"
		"    ldr r5, [r4]          // test.c:1\n"
		"    str [sp + __frame_factorial.t9], r5 // test.c:1\n"
		"    li r4, 1              // test.c:1\n"
		"    str [sp + __frame_factorial.t10], r4 // test.c:1\n"
		"    ldr r4, [sp + __frame_factorial.t9] // test.c:1\n"
		"    ldr r5, [sp + __frame_factorial.t10] // test.c:1\n"
		"    sub r4, r4, r5        // test.c:1\n"
		"    str [sp + __frame_factorial.t11], r4 // test.c:1\n"
		"    ldr r0, [sp + __frame_factorial.t11] // test.c:1\n"
		"    call cc_factorial     // test.c:1\n"
		"    str [sp + __frame_factorial.t12], r0 // test.c:1\n"
		"    ldr r4, [sp + __frame_factorial.t7] // test.c:1\n"
		"    ldr r5, [sp + __frame_factorial.t12] // test.c:1\n"
		"    imul r4, r4, r5       // test.c:1\n"
		"    str [sp + __frame_factorial.t13], r4 // test.c:1\n"
		"    ldr r0, [sp + __frame_factorial.t13] // test.c:1\n"
		"    leave                 // test.c:1\n"
		"    ret                   // test.c:1\n");
}

TEST(codegen, float_division_and_int_conversion_use_the_float_bank_and_a_real_ftoii)
{
	// `2.0`'s bit pattern (1073741824) has no direct float-immediate load (no such instruction
	// exists, 05-Instruction-Set.md/06-Pseudo-Instructions.md): materialized into an int register
	// first, then reinterpreted bit-for-bit into the float bank with `mtf` - see IrOpcode::Const's
	// case in codegen.cpp.
	CHECK_EQ(generateCasm("int halveToInt(float x) { return (int)(x / 2.0); }"),
		"@text\n"
		"\n"
		"// halveToInt - test.c:1\n"
		"struct __frame_halveToInt\n"
		"    local0: f32\n"
		"    t0: u32\n"
		"    t1: u32\n"
		"    t2: u32\n"
		"    t3: u32\n"
		"    t4: u32\n"
		"endstruct\n"
		"cc_halveToInt:\n"
		"    enter __frame_halveToInt // test.c:1\n"
		"    str [sp + __frame_halveToInt.local0], f0 // test.c:1\n"
		".L0:\n"
		"    la r4, [sp + __frame_halveToInt.local0] // test.c:1\n"
		"    str [sp + __frame_halveToInt.t0], r4 // test.c:1\n"
		"    ldr r4, [sp + __frame_halveToInt.t0] // test.c:1\n"
		"    ldr f5, [r4]          // test.c:1\n"
		"    str [sp + __frame_halveToInt.t1], f5 // test.c:1\n"
		"    la r4, 1073741824     // test.c:1\n"
		"    mtf f4, r4            // test.c:1\n"
		"    str [sp + __frame_halveToInt.t2], f4 // test.c:1\n"
		"    ldr f4, [sp + __frame_halveToInt.t1] // test.c:1\n"
		"    ldr f5, [sp + __frame_halveToInt.t2] // test.c:1\n"
		"    div f4, f4, f5        // test.c:1\n"
		"    str [sp + __frame_halveToInt.t3], f4 // test.c:1\n"
		"    ldr f4, [sp + __frame_halveToInt.t3] // test.c:1\n"
		"    ftoii r4, f4          // test.c:1\n"
		"    str [sp + __frame_halveToInt.t4], r4 // test.c:1\n"
		"    ldr r0, [sp + __frame_halveToInt.t4] // test.c:1\n"
		"    leave                 // test.c:1\n"
		"    ret                   // test.c:1\n");
}
