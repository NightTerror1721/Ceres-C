#include <ceresc/codegen/codegen.h>
#include <ceresc/ir/ir_builder.h>
#include <ceresc/ir/ir_optimizer.h>
#include <ceresc/parser/parser.h>
#include <ceresc/sema/sema.h>
#include <ceresc/lexer/lexer.h>
#include <ceresc/support/arena.h>
#include <ceresc/support/diagnostics.h>
#include <ceresc/support/source_manager.h>
#include <ceresc/support/string_pool.h>

#include "framework.h"

#include <string>
#include <string_view>

using namespace ceresc;

namespace
{
	// Parses, type-checks, lowers to IR, optimizes and generates .casm text for `source` - the whole
	// pipeline short of --run, exercised end to end the same way the driver does (libs/driver).
	// `options` is what selects between the optimized output and the simplified -O0 one, which is
	// why the interesting programs below appear twice: the two shapes are pinned side by side.
	std::string generateCasm(std::string_view source, support::OptimizationOptions options)
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

		ir::IrBuilder builder(arena, diagnostics, options);
		ir::IrModule module = builder.build(*unit);
		ir::optimize(module, arena, options);

		codegen::CodeGen codeGen(sourceManager, diagnostics, options);
		std::string result = codeGen.generate(*unit, module);
		CHECK(!diagnostics.hasErrors());
		return result;
	}

	std::string atO0(std::string_view source)
	{
		return generateCasm(source, support::OptimizationOptions::forLevel(support::OptimizationLevel::O0));
	}

	std::string atO2(std::string_view source)
	{
		return generateCasm(source, support::OptimizationOptions::forLevel(support::OptimizationLevel::O2));
	}

	// One optimization at a time, on top of -O0 - what pins a single peephole's effect without the
	// rest of the pipeline reshaping the output around it.
	support::OptimizationOptions only(bool support::OptimizationOptions::* flag)
	{
		support::OptimizationOptions options = support::OptimizationOptions::none();
		options.*flag = true;
		return options;
	}

	// The mirror of only(): everything on EXCEPT one flag, which is how the "simplified" counterpart
	// of a single optimization is observed. -O0 would turn off all eighteen at once and tell you
	// nothing about which one produced a given difference.
	support::OptimizationOptions without(bool support::OptimizationOptions::* flag)
	{
		support::OptimizationOptions options = support::OptimizationOptions::forLevel(support::OptimizationLevel::O2);
		options.*flag = false;
		return options;
	}

	bool contains(std::string_view haystack, std::string_view needle)
	{
		return haystack.find(needle) != std::string_view::npos;
	}

	usize countOf(std::string_view haystack, std::string_view needle)
	{
		usize count = 0;
		for (usize at = haystack.find(needle); at != std::string_view::npos; at = haystack.find(needle, at + 1))
			++count;
		return count;
	}
}

// ---- golden tests (§12 of the architecture plan) --------------------------------------------
//
// Every program below was also verified for real, at BOTH -O0 and -O2: assembled with the actual
// `ceres asm` and executed with the actual `ceres run` from the sibling CeresASM checkout (never
// linked against - see codegen.h's own header comment on why libs/driver only ever spawns it as a
// subprocess), poking CeresASM's TerminalDevice MMIO register directly (this project has no printf
// yet) to make the real computed VALUE observable, since `ceres run`'s own process exit code never
// carries one (verified against CeresASM's Ceres/libs/driver/src/machine_runner.cpp - there is no
// register-to-exit-code channel at all, which is also why `main` halts the machine instead of an
// ordinary `leave`/`ret` - see codegen.h). That live check is what tests/e2e (§12) automates, at
// both levels; what is captured here is that the resulting .casm text, byte for byte, is the one
// that was actually verified.
//
// Pinning both levels is the point, not duplication: -O0 is the simplified behaviour every
// optimization has a counterpart for (support/optimization.h), and having both in front of you is
// what makes "what did this optimization actually change?" a diff rather than an argument.

TEST(codegen, returning_a_constant_at_O0)
{
	CHECK_EQ(atO0("int main() { return 42; }"),
		"@text\n"
		"\n"
		"// main - test.c:1\n"
		"struct __frame_main\n"
		"    slot0: u32\n"
		"endstruct\n"
		"global main:\n"
		"    enter __frame_main    // test.c:1\n"
		".L0:\n"
		"    li r4, 42             // test.c:1\n"
		"    str [sp + __frame_main.slot0], r4 // test.c:1\n"
		"    ldr r4, [sp + __frame_main.slot0] // test.c:1\n"
		"    mov r0, r4            // test.c:1\n"
		"    leave                 // test.c:1\n"
		"    la r4, 0xFFFF0000     // test.c:1\n"
		"    li r5, 1              // test.c:1\n"
		"    strb [r4 + 0], r5     // test.c:1\n"
		"    halt                  // test.c:1\n");
}

TEST(codegen, returning_a_constant_at_O2)
{
	// No frame at all, and the constant never makes the round trip through one.
	CHECK_EQ(atO2("int main() { return 42; }"),
		"@text\n"
		"\n"
		"// main - test.c:1\n"
		"global main:\n"
		".L0:\n"
		"    li r3, 42             // test.c:1\n"
		"    mov r0, r3            // test.c:1\n"
		"    la r4, 0xFFFF0000     // test.c:1\n"
		"    li r5, 1              // test.c:1\n"
		"    strb [r4 + 0], r5     // test.c:1\n"
		"    halt                  // test.c:1\n");
}

TEST(codegen, arithmetic_at_O0_reloads_every_operand_from_its_own_frame_slot)
{
	// Seven slots for one addition: every IR temporary has its own permanent field, and every
	// operand is written out and read back around each instruction. That uniformity is the whole
	// point of the -O0 path - it is trivially checkable by hand.
	CHECK_EQ(atO0("int add(int a, int b) { return a + b; }"),
		"@text\n"
		"\n"
		"// add - test.c:1\n"
		"struct __frame_add\n"
		"    slot0: u32\n"
		"    slot1: u32\n"
		"    slot2: u32\n"
		"    slot3: u32\n"
		"    slot4: u32\n"
		"    slot5: u32\n"
		"    slot6: u32\n"
		"endstruct\n"
		"global add:\n"
		"    enter __frame_add     // test.c:1\n"
		"    str [sp + __frame_add.slot0], r0 // test.c:1\n"
		"    str [sp + __frame_add.slot1], r1 // test.c:1\n"
		".L0:\n"
		"    la r4, [sp + __frame_add.slot0] // test.c:1\n"
		"    str [sp + __frame_add.slot2], r4 // test.c:1\n"
		"    ldr r4, [sp + __frame_add.slot2] // test.c:1\n"
		"    ldr r5, [r4]          // test.c:1\n"
		"    str [sp + __frame_add.slot3], r5 // test.c:1\n"
		"    la r4, [sp + __frame_add.slot1] // test.c:1\n"
		"    str [sp + __frame_add.slot4], r4 // test.c:1\n"
		"    ldr r4, [sp + __frame_add.slot4] // test.c:1\n"
		"    ldr r5, [r4]          // test.c:1\n"
		"    str [sp + __frame_add.slot5], r5 // test.c:1\n"
		"    ldr r4, [sp + __frame_add.slot3] // test.c:1\n"
		"    ldr r5, [sp + __frame_add.slot5] // test.c:1\n"
		"    add r4, r4, r5        // test.c:1\n"
		"    str [sp + __frame_add.slot6], r4 // test.c:1\n"
		"    ldr r4, [sp + __frame_add.slot6] // test.c:1\n"
		"    mov r0, r4            // test.c:1\n"
		"    leave                 // test.c:1\n"
		"    ret                   // test.c:1\n");
}

TEST(codegen, arithmetic_at_O2_is_frameless_and_stays_in_registers)
{
	// Both parameters are register-resident (never address-taken, and this is a leaf), so the
	// FrameAddr/Load pair collapses to nothing and no frame is opened: no struct, no enter/leave.
	CHECK_EQ(atO2("int add(int a, int b) { return a + b; }"),
		"@text\n"
		"\n"
		"// add - test.c:1\n"
		"global add:\n"
		".L0:\n"
		"    mov r3, r0            // test.c:1\n"
		"    mov r2, r1            // test.c:1\n"
		"    add r12, r3, r2       // test.c:1\n"
		"    mov r0, r12           // test.c:1\n"
		"    ret                   // test.c:1\n");
}

TEST(codegen, comparison_used_as_a_value_still_materializes_at_O2)
{
	// Nothing to fuse into here: the comparison IS the return value, so it has to become a real
	// 0/1 - 06-Pseudo-Instructions.md's own four-instruction shape. Contrast with the next test.
	CHECK_EQ(atO2("int lessThan(int a, int b) { return a < b; }"),
		"@text\n"
		"\n"
		"// lessThan - test.c:1\n"
		"global lessThan:\n"
		".L0:\n"
		"    mov r3, r0            // test.c:1\n"
		"    mov r2, r1            // test.c:1\n"
		"    ifls r3, r2, .cmp0_true // test.c:1\n"
		"    li r12, 0             // test.c:1\n"
		"    jp .cmp0_end          // test.c:1\n"
		".cmp0_true:\n"
		"    li r12, 1             // test.c:1\n"
		".cmp0_end:\n"
		"    mov r0, r12           // test.c:1\n"
		"    ret                   // test.c:1\n");
}

TEST(codegen, comparison_used_as_a_condition_fuses_into_one_branch_at_O2)
{
	// The same comparison, this time consumed by a branch: one `ifge` (the INVERTED predicate, so
	// the true arm falls through) replaces the whole materialize-then-test sequence.
	CHECK_EQ(atO2("int clamp(int a, int b) { if (a < b) return 1; return 0; }"),
		"@text\n"
		"\n"
		"// clamp - test.c:1\n"
		"global clamp:\n"
		".L0:\n"
		"    mov r3, r0            // test.c:1\n"
		"    mov r2, r1            // test.c:1\n"
		"    ifge r3, r2, .L2      // test.c:1\n"
		".L1:\n"
		"    li r3, 1              // test.c:1\n"
		"    mov r0, r3            // test.c:1\n"
		"    ret                   // test.c:1\n"
		".L2:\n"
		"    li r3, 0              // test.c:1\n"
		"    mov r0, r3            // test.c:1\n"
		"    ret                   // test.c:1\n");
}

TEST(codegen, comparison_used_as_a_condition_at_O0_materializes_then_branches)
{
	CHECK_EQ(atO0("int clamp(int a, int b) { if (a < b) return 1; return 0; }"),
		"@text\n"
		"\n"
		"// clamp - test.c:1\n"
		"struct __frame_clamp\n"
		"    slot0: u32\n"
		"    slot1: u32\n"
		"    slot2: u32\n"
		"    slot3: u32\n"
		"    slot4: u32\n"
		"    slot5: u32\n"
		"    slot6: u32\n"
		"    slot7: u32\n"
		"    slot8: u32\n"
		"    slot9: u32\n"
		"endstruct\n"
		"global clamp:\n"
		"    enter __frame_clamp   // test.c:1\n"
		"    str [sp + __frame_clamp.slot0], r0 // test.c:1\n"
		"    str [sp + __frame_clamp.slot1], r1 // test.c:1\n"
		".L0:\n"
		"    la r4, [sp + __frame_clamp.slot0] // test.c:1\n"
		"    str [sp + __frame_clamp.slot2], r4 // test.c:1\n"
		"    ldr r4, [sp + __frame_clamp.slot2] // test.c:1\n"
		"    ldr r5, [r4]          // test.c:1\n"
		"    str [sp + __frame_clamp.slot3], r5 // test.c:1\n"
		"    la r4, [sp + __frame_clamp.slot1] // test.c:1\n"
		"    str [sp + __frame_clamp.slot4], r4 // test.c:1\n"
		"    ldr r4, [sp + __frame_clamp.slot4] // test.c:1\n"
		"    ldr r5, [r4]          // test.c:1\n"
		"    str [sp + __frame_clamp.slot5], r5 // test.c:1\n"
		"    ldr r4, [sp + __frame_clamp.slot3] // test.c:1\n"
		"    ldr r5, [sp + __frame_clamp.slot5] // test.c:1\n"
		"    ifls r4, r5, .cmp0_true // test.c:1\n"
		"    li r4, 0              // test.c:1\n"
		"    jp .cmp0_end          // test.c:1\n"
		".cmp0_true:\n"
		"    li r4, 1              // test.c:1\n"
		".cmp0_end:\n"
		"    str [sp + __frame_clamp.slot6], r4 // test.c:1\n"
		"    li r4, 0              // test.c:1\n"
		"    str [sp + __frame_clamp.slot7], r4 // test.c:1\n"
		"    ldr r4, [sp + __frame_clamp.slot6] // test.c:1\n"
		"    ldr r5, [sp + __frame_clamp.slot7] // test.c:1\n"
		"    ifne r4, r5, .L1      // test.c:1\n"
		"    jp .L2                // test.c:1\n"
		".L1:\n"
		"    li r4, 1              // test.c:1\n"
		"    str [sp + __frame_clamp.slot8], r4 // test.c:1\n"
		"    ldr r4, [sp + __frame_clamp.slot8] // test.c:1\n"
		"    mov r0, r4            // test.c:1\n"
		"    leave                 // test.c:1\n"
		"    ret                   // test.c:1\n"
		".L2:\n"
		"    li r4, 0              // test.c:1\n"
		"    str [sp + __frame_clamp.slot9], r4 // test.c:1\n"
		"    ldr r4, [sp + __frame_clamp.slot9] // test.c:1\n"
		"    mov r0, r4            // test.c:1\n"
		"    leave                 // test.c:1\n"
		"    ret                   // test.c:1\n");
}

TEST(codegen, recursion_at_O0)
{
	CHECK_EQ(atO0("int factorial(int n) { if (n <= 1) return 1; return n * factorial(n - 1); }"),
		"@text\n"
		"\n"
		"// factorial - test.c:1\n"
		"struct __frame_factorial\n"
		"    slot0: u32\n"
		"    slot1: u32\n"
		"    slot2: u32\n"
		"    slot3: u32\n"
		"    slot4: u32\n"
		"    slot5: u32\n"
		"    slot6: u32\n"
		"    slot7: u32\n"
		"    slot8: u32\n"
		"    slot9: u32\n"
		"    slot10: u32\n"
		"    slot11: u32\n"
		"    slot12: u32\n"
		"    slot13: u32\n"
		"    slot14: u32\n"
		"endstruct\n"
		"global factorial:\n"
		"    enter __frame_factorial // test.c:1\n"
		"    str [sp + __frame_factorial.slot0], r0 // test.c:1\n"
		".L0:\n"
		"    la r4, [sp + __frame_factorial.slot0] // test.c:1\n"
		"    str [sp + __frame_factorial.slot1], r4 // test.c:1\n"
		"    ldr r4, [sp + __frame_factorial.slot1] // test.c:1\n"
		"    ldr r5, [r4]          // test.c:1\n"
		"    str [sp + __frame_factorial.slot2], r5 // test.c:1\n"
		"    li r4, 1              // test.c:1\n"
		"    str [sp + __frame_factorial.slot3], r4 // test.c:1\n"
		"    ldr r4, [sp + __frame_factorial.slot2] // test.c:1\n"
		"    ldr r5, [sp + __frame_factorial.slot3] // test.c:1\n"
		"    ifle r4, r5, .cmp0_true // test.c:1\n"
		"    li r4, 0              // test.c:1\n"
		"    jp .cmp0_end          // test.c:1\n"
		".cmp0_true:\n"
		"    li r4, 1              // test.c:1\n"
		".cmp0_end:\n"
		"    str [sp + __frame_factorial.slot4], r4 // test.c:1\n"
		"    li r4, 0              // test.c:1\n"
		"    str [sp + __frame_factorial.slot5], r4 // test.c:1\n"
		"    ldr r4, [sp + __frame_factorial.slot4] // test.c:1\n"
		"    ldr r5, [sp + __frame_factorial.slot5] // test.c:1\n"
		"    ifne r4, r5, .L1      // test.c:1\n"
		"    jp .L2                // test.c:1\n"
		".L1:\n"
		"    li r4, 1              // test.c:1\n"
		"    str [sp + __frame_factorial.slot6], r4 // test.c:1\n"
		"    ldr r4, [sp + __frame_factorial.slot6] // test.c:1\n"
		"    mov r0, r4            // test.c:1\n"
		"    leave                 // test.c:1\n"
		"    ret                   // test.c:1\n"
		".L2:\n"
		"    la r4, [sp + __frame_factorial.slot0] // test.c:1\n"
		"    str [sp + __frame_factorial.slot7], r4 // test.c:1\n"
		"    ldr r4, [sp + __frame_factorial.slot7] // test.c:1\n"
		"    ldr r5, [r4]          // test.c:1\n"
		"    str [sp + __frame_factorial.slot8], r5 // test.c:1\n"
		"    la r4, [sp + __frame_factorial.slot0] // test.c:1\n"
		"    str [sp + __frame_factorial.slot9], r4 // test.c:1\n"
		"    ldr r4, [sp + __frame_factorial.slot9] // test.c:1\n"
		"    ldr r5, [r4]          // test.c:1\n"
		"    str [sp + __frame_factorial.slot10], r5 // test.c:1\n"
		"    li r4, 1              // test.c:1\n"
		"    str [sp + __frame_factorial.slot11], r4 // test.c:1\n"
		"    ldr r4, [sp + __frame_factorial.slot10] // test.c:1\n"
		"    ldr r5, [sp + __frame_factorial.slot11] // test.c:1\n"
		"    sub r4, r4, r5        // test.c:1\n"
		"    str [sp + __frame_factorial.slot12], r4 // test.c:1\n"
		"    ldr r4, [sp + __frame_factorial.slot12] // test.c:1\n"
		"    mov r0, r4            // test.c:1\n"
		"    call factorial        // test.c:1\n"
		"    mov r4, r0            // test.c:1\n"
		"    str [sp + __frame_factorial.slot13], r4 // test.c:1\n"
		"    ldr r4, [sp + __frame_factorial.slot8] // test.c:1\n"
		"    ldr r5, [sp + __frame_factorial.slot13] // test.c:1\n"
		"    imul r4, r4, r5       // test.c:1\n"
		"    str [sp + __frame_factorial.slot14], r4 // test.c:1\n"
		"    ldr r4, [sp + __frame_factorial.slot14] // test.c:1\n"
		"    mov r0, r4            // test.c:1\n"
		"    leave                 // test.c:1\n"
		"    ret                   // test.c:1\n");
}

TEST(codegen, recursion_at_O2)
{
	// Fifteen frame slots become two. Note what survives and why: `factorial` calls something, so
	// its parameter cannot live in a register across the call (r0-r7/r12 are caller-saved,
	// 24-Calling-Convention.md) and the frame stays - frameless-leaf is not a blanket "drop the
	// frame", it is "drop it when the analysis says nothing needs it". The two remaining slots hold
	// the values live ACROSS the recursive call; everything whose range is call-free got a register.
	CHECK_EQ(atO2("int factorial(int n) { if (n <= 1) return 1; return n * factorial(n - 1); }"),
		"@text\n"
		"\n"
		"// factorial - test.c:1\n"
		"struct __frame_factorial\n"
		"    slot0: u32\n"
		"    slot1: u32\n"
		"endstruct\n"
		"global factorial:\n"
		"    enter __frame_factorial // test.c:1\n"
		"    str [sp + __frame_factorial.slot0], r0 // test.c:1\n"
		".L0:\n"
		"    la r12, [sp + __frame_factorial.slot0] // test.c:1\n"
		"    ldr r7, [r12]         // test.c:1\n"
		"    ifgr r7, 1, .L2       // test.c:1\n"
		".L1:\n"
		"    li r12, 1             // test.c:1\n"
		"    mov r0, r12           // test.c:1\n"
		"    leave                 // test.c:1\n"
		"    ret                   // test.c:1\n"
		".L2:\n"
		"    la r12, [sp + __frame_factorial.slot0] // test.c:1\n"
		"    ldr r5, [r12]         // test.c:1\n"
		"    str [sp + __frame_factorial.slot1], r5 // test.c:1\n"
		"    la r12, [sp + __frame_factorial.slot0] // test.c:1\n"
		"    ldr r7, [r12]         // test.c:1\n"
		"    sub r6, r7, 1         // test.c:1\n"
		"    mov r0, r6            // test.c:1\n"
		"    call factorial        // test.c:1\n"
		"    mov r6, r0            // test.c:1\n"
		"    ldr r4, [sp + __frame_factorial.slot1] // test.c:1\n"
		"    imul r7, r4, r6       // test.c:1\n"
		"    mov r0, r7            // test.c:1\n"
		"    leave                 // test.c:1\n"
		"    ret                   // test.c:1\n");
}

TEST(codegen, float_division_and_conversion_at_O2)
{
	// The float bank throughout (`mov f3, f0`, `div f1, ...` - the assembler picks FMOV/FDIV from
	// the register bank, 05-Instruction-Set.md), the literal 2.0 built as its raw bit pattern in an
	// integer register and moved across with `mtf`, and one real `ftoii` for the cast.
	CHECK_EQ(atO2("int halveToInt(float x) { return (int)(x / 2.0); }"),
		"@text\n"
		"\n"
		"// halveToInt - test.c:1\n"
		"global halveToInt:\n"
		".L0:\n"
		"    mov f3, f0            // test.c:1\n"
		"    la r4, 1073741824     // test.c:1\n"
		"    mtf f2, r4            // test.c:1\n"
		"    div f1, f3, f2        // test.c:1\n"
		"    ftoii r3, f1          // test.c:1\n"
		"    mov r0, r3            // test.c:1\n"
		"    ret                   // test.c:1\n");
}

TEST(codegen, constant_folding_collapses_a_whole_expression_at_O2)
{
	CHECK_EQ(atO2("int main() { return 2 + 3 * 4; }"),
		"@text\n"
		"\n"
		"// main - test.c:1\n"
		"global main:\n"
		".L0:\n"
		"    li r3, 14             // test.c:1\n"
		"    mov r0, r3            // test.c:1\n"
		"    la r4, 0xFFFF0000     // test.c:1\n"
		"    li r5, 1              // test.c:1\n"
		"    strb [r4 + 0], r5     // test.c:1\n"
		"    halt                  // test.c:1\n");
}

TEST(codegen, inlining_a_small_function_at_O2)
{
	// `add` is spliced in, the arguments fold through it, and the callee then has no remaining
	// caller at all - so unused-function elimination drops its body entirely. The generated text
	// has no `add` in it. It has to be `static` for that last step: a function with external
	// linkage is kept whatever this unit does with it, since another object may call it.
	CHECK_EQ(atO2("static int add(int a, int b) { return a + b; } int main() { return add(3, 4); }"),
		"@text\n"
		"\n"
		"// main - test.c:1\n"
		"global main:\n"
		".L0:\n"
		"    li r3, 7              // test.c:1\n"
		"    mov r0, r3            // test.c:1\n"
		"    la r4, 0xFFFF0000     // test.c:1\n"
		"    li r5, 1              // test.c:1\n"
		"    strb [r4 + 0], r5     // test.c:1\n"
		"    halt                  // test.c:1\n");
}

TEST(codegen, a_loop_at_O2)
{
	// A loop is where the liveness dataflow earns its keep: `total` and `i` are defined before the
	// back edge and read after it, so their registers (r6/r7) must stay reserved across the whole
	// loop rather than being handed to a temporary defined later in the body.
	CHECK_EQ(atO2("int sum(int n) { int total = 0; for (int i = 0; i < n; i = i + 1) { total = total + i; } return total; }"),
		"@text\n"
		"\n"
		"// sum - test.c:1\n"
		"global sum:\n"
		".L0:\n"
		"    li r3, 0              // test.c:1\n"
		"    mov r6, r3            // test.c:1\n"
		"    li r3, 0              // test.c:1\n"
		"    mov r7, r3            // test.c:1\n"
		".L1:\n"
		"    mov r3, r7            // test.c:1\n"
		"    mov r2, r0            // test.c:1\n"
		"    ifge r3, r2, .L4      // test.c:1\n"
		".L2:\n"
		"    mov r3, r6            // test.c:1\n"
		"    mov r2, r7            // test.c:1\n"
		"    add r1, r3, r2        // test.c:1\n"
		"    mov r6, r1            // test.c:1\n"
		".L3:\n"
		"    mov r3, r7            // test.c:1\n"
		"    add r1, r3, 1         // test.c:1\n"
		"    mov r7, r1            // test.c:1\n"
		"    jp .L1                // test.c:1\n"
		".L4:\n"
		"    mov r3, r6            // test.c:1\n"
		"    mov r0, r3            // test.c:1\n"
		"    ret                   // test.c:1\n");
}

// ---- one optimization at a time ----------------------------------------------------------------
//
// The goldens above pin the two endpoints. These pin each individual switch, so that a regression
// in one optimization names itself instead of showing up as a diff in a 40-line golden. Each test
// compares -O2 against -O2-minus-one-flag (without()), or -O0 against -O0-plus-one-flag (only()) -
// never the two levels, which would change eighteen things at once.

TEST(codegen, frameless_leaf_is_what_removes_enter_and_leave)
{
	std::string_view source = "int add(int a, int b) { return a + b; }";

	std::string optimized = atO2(source);
	CHECK(!contains(optimized, "enter"));
	CHECK(!contains(optimized, "leave"));

	// Simplified counterpart: a real frame for every function, no matter what the analysis found.
	// The frame is empty here (nothing spills), so it is a bare `enter` - still a valid prologue,
	// and still what you want when you are bisecting a suspected escape-analysis bug.
	std::string simplified = generateCasm(source, without(&support::OptimizationOptions::framelessLeaf));
	CHECK(contains(simplified, "\n    enter"));
	CHECK(!contains(simplified, "enter __frame_"));
	CHECK(contains(simplified, "leave"));
}

TEST(codegen, a_function_whose_parameter_is_address_taken_keeps_its_frame_even_at_O2)
{
	// The safety condition frameless-leaf rests on. `&a` escaping into a call means `a` needs a real
	// memory home, so the frame must exist - this is the case that used to make the optimization
	// look risky (value_placement.h), and it is checked here rather than assumed.
	std::string text = atO2("void use(int* p); int leaky(int a) { use(&a); return a; }");
	CHECK(contains(text, "enter __frame_leaky"));
	CHECK(contains(text, "slot0: u32"));
}

TEST(codegen, cmp_branch_fusion_is_what_collapses_the_materialized_comparison)
{
	std::string_view source = "int clamp(int a, int b) { if (a < b) return 1; return 0; }";

	std::string fused = atO2(source);
	CHECK(!contains(fused, ".cmp0_true"));
	CHECK_EQ(countOf(fused, "ifge "), usize(1)); // exactly one conditional instruction

	// Simplified counterpart: the comparison becomes a real 0/1 and the branch tests that value.
	std::string separate = generateCasm(source, without(&support::OptimizationOptions::cmpBranchFusion));
	CHECK(contains(separate, ".cmp0_true"));
	CHECK(contains(separate, "ifeq r12, 0, .L2")); // ...tested against zero, as its own instruction
}

TEST(codegen, a_float_branch_keeps_its_explicit_false_jump)
{
	// IEEE comparisons with NaN are not complements: both a < b and a >= b are false. The true arm
	// is next here, so an integer-style inverted branch would incorrectly fall through into it.
	std::string text = atO2("int f(float a, float b) { if (a < b) return 1; return 0; }");
	CHECK(contains(text, "ifbl"));
	CHECK(contains(text, "    jp .L"));
}

TEST(codegen, immediate_operands_keep_a_constant_out_of_a_register)
{
	std::string_view source = "int bump(int a) { return a + 1; }";

	std::string withImmediates = atO2(source);
	CHECK(contains(withImmediates, "add r1, r3, 1"));
	CHECK(!contains(withImmediates, "li r2, 1")); // nothing materializes the 1 at all

	std::string simplified = generateCasm(source, without(&support::OptimizationOptions::immediateOperands));
	CHECK(contains(simplified, "li r2, 1"));
	CHECK(contains(simplified, "add r1, r3, r2"));
}

TEST(codegen, a_constant_too_large_for_the_immediate_field_is_materialized_anyway)
{
	// 04-Instruction-Format.md gives the immediate 16 bits, and ADDI reads them zero-extended while
	// CMPI reads them sign-extended (verified in CeresASM's execution_engine.h) - so immediateFor()
	// only accepts [0, 32767], where both readings agree. Past that the constant has to go through
	// a register even with the peephole on.
	std::string text = atO2("int bump(int a) { return a + 100000; }");
	CHECK(contains(text, "la r2, 100000"));
	CHECK(contains(text, "add r1, r3, r2"));
}

TEST(codegen, register_allocation_is_what_keeps_values_out_of_the_frame)
{
	// With regalloc off, -O2's IR passes still run but every value gets its own permanent slot -
	// the simplified placement rule. The result is the -O0 shape again, which is exactly the point:
	// it is the one placement you can verify by reading.
	std::string simplified = generateCasm("int add(int a, int b) { return a + b; }",
		without(&support::OptimizationOptions::registerAllocation));
	CHECK(contains(simplified, "struct __frame_add"));
	CHECK(contains(simplified, "slot6: u32"));       // one field per value, none shared
	CHECK(contains(simplified, "enter __frame_add")); // and therefore a frame, whatever frameless-leaf says
}

TEST(codegen, local_slot_reuse_shares_one_field_between_disjoint_scopes)
{
	// Three locals, never alive at the same time. Reuse is measured on the frame struct rather than
	// the instruction stream, because that is where the saving actually shows up. regalloc is off
	// so that the locals really do land in the frame instead of in registers.
	std::string_view source =
		"void use(int* p);"
		"int scopes() { { int a = 1; use(&a); } { int b = 2; use(&b); } { int c = 3; use(&c); } return 0; }";

	support::OptimizationOptions reuse = support::OptimizationOptions::forLevel(support::OptimizationLevel::O2);
	reuse.registerAllocation = false;
	support::OptimizationOptions permanent = reuse;
	permanent.localSlotReuse = false;

	CHECK(countOf(generateCasm(source, reuse), ": u32") < countOf(generateCasm(source, permanent), ": u32"));
}

TEST(codegen, a_widened_reused_float_slot_is_declared_as_an_integer_word_array)
{
	// f32 is one word. Reusing its slot for a four-element int array must change the frame field's
	// type, or accesses to the latter three elements would run beyond the declared field.
	support::OptimizationOptions options = support::OptimizationOptions::forLevel(support::OptimizationLevel::O2);
	options.registerAllocation = false;
	std::string text = generateCasm("int f() { { float x = 1.0; } { int a[4]; a[1] = 2; return a[1]; } }", options);
	CHECK(contains(text, "slot0: u32[4]"));
	CHECK(!contains(text, "slot0: f32"));
}

TEST(codegen, fallthrough_drops_the_jump_to_the_block_emitted_next)
{
	std::string_view source = "int pick(int a) { if (a) { return 1; } return 2; }";

	std::string optimized = atO2(source);
	std::string simplified = generateCasm(source, without(&support::OptimizationOptions::fallthroughBranches));
	CHECK(countOf(optimized, "    jp ") < countOf(simplified, "    jp "));
}

TEST(codegen, unused_function_elimination_is_what_removes_the_inlined_callees_body)
{
	// Inlining alone does not delete the original - it only stops the caller from calling it. Both
	// halves have to be on for `add` to disappear, and this pins which one does which.
	std::string_view source = "static int add(int a, int b) { return a + b; } int main() { return add(3, 4); }";

	CHECK(!contains(atO2(source), "add"));
	CHECK(contains(generateCasm(source, without(&support::OptimizationOptions::unusedFunctionElimination)), "add:"));
}

TEST(codegen, an_exported_function_is_kept_even_when_nothing_in_this_file_calls_it)
{
	// No `main` in this translation unit, so there is no entry point to measure reachability from
	// and nothing may be dropped - see ir_optimizer.cpp's removeUnusedFunctions().
	std::string text = atO2("int helper(int a) { return a + 1; }");
	CHECK(contains(text, "helper:"));
}

TEST(codegen, turning_one_flag_on_top_of_O0_changes_only_that_one_thing)
{
	// The other direction of the same idea, and the reason only() exists: -O0 plus fallthrough
	// alone still writes every value through the frame, but the redundant jump is gone.
	std::string_view source = "int pick(int a) { if (a) { return 1; } return 2; }";

	std::string baseline = atO0(source);
	std::string oneFlag = generateCasm(source, only(&support::OptimizationOptions::fallthroughBranches));

	CHECK(contains(oneFlag, "struct __frame_pick")); // still the simplified placement
	CHECK(countOf(oneFlag, "    jp ") < countOf(baseline, "    jp "));
}

// ---- composite memory: indexed addressing and aggregate globals (Fase 7) -----------------------

TEST(codegen, an_array_element_read_uses_the_indexed_load_form)
{
	// 05-Instruction-Set.md's whole point about indexed addressing: "Walking an array used to cost
	// an add per element". One `mul` to scale the index, then the access reads base and index
	// together - the assembler picks LDRX from the operand shapes, exactly as it picks ADDI over ADD.
	CHECK_EQ(atO2("int sum(int* a, int i) { return a[i]; }"),
		"@text\n"
		"\n"
		"// sum - test.c:1\n"
		"global sum:\n"
		".L0:\n"
		"    mov r3, r0            // test.c:1\n"
		"    mov r2, r1            // test.c:1\n"
		"    mul r7, r2, 4         // test.c:1\n"
		"    ldr r3, [r3 + r7]     // test.c:1\n"
		"    mov r0, r3            // test.c:1\n"
		"    ret                   // test.c:1\n");
}

TEST(codegen, an_array_element_write_uses_the_indexed_store_form)
{
	std::string text = atO2("void put(int* a, int i, int v) { a[i] = v; }");
	CHECK(contains(text, "str [r12 + r5], r3"));
	CHECK(!contains(text, "\n    add ")); // no separate address computation left
}

TEST(codegen, a_struct_field_read_uses_a_constant_displacement_not_a_separate_add)
{
	CHECK_EQ(atO2("struct P { int x; int y; }; int gety(struct P* p) { return p->y; }"),
		"@text\n"
		"\n"
		"// gety - test.c:1\n"
		"global gety:\n"
		".L0:\n"
		"    mov r3, r0            // test.c:1\n"
		"    ldr r3, [r3 + 4]      // test.c:1\n"
		"    mov r0, r3            // test.c:1\n"
		"    ret                   // test.c:1\n");
}

TEST(codegen, without_address_folding_every_element_address_is_computed_into_a_register_first)
{
	// The simplified counterpart this optimization has (support/optimization.h): the `add` is
	// written out and the access reads a single register. Same program, one flag apart.
	std::string_view source = "int sum(int* a, int i) { return a[i]; }";
	std::string folded = atO2(source);
	std::string plain = generateCasm(source, without(&support::OptimizationOptions::addressFolding));

	CHECK(contains(folded, "ldr r3, [r3 + r7]"));
	CHECK(!contains(plain, "ldr r3, [r3 + r7]"));
	CHECK(contains(plain, "    add "));
	CHECK(countOf(plain, "\n") > countOf(folded, "\n"));
}

TEST(codegen, address_folding_is_off_at_O0)
{
	std::string text = atO0("int sum(int* a, int i) { return a[i]; }");
	CHECK(contains(text, "    add "));
	CHECK(!contains(text, " + r"));
}

TEST(codegen, an_indexed_store_whose_operands_all_live_in_frame_fields_keeps_the_plain_add)
{
	// The register budget: base, index and value are three reads at once and only r4/r5 are scratch
	// (§10). Turning register allocation off puts all three in frame fields, which is exactly the
	// shape findFoldableAddress() refuses - and it must still produce correct code, not a store
	// with two operands fighting over one register.
	support::OptimizationOptions options = support::OptimizationOptions::forLevel(support::OptimizationLevel::O2);
	options.registerAllocation = false;
	std::string text = generateCasm("void put(int* a, int i, int v) { a[i] = v; }", options);
	CHECK(contains(text, "    add "));
	CHECK(!contains(text, "str [r4 + r5]"));
}

TEST(codegen, aggregate_globals_get_a_let_of_their_own_shape)
{
	// An array of scalars keeps its real CASM shape - both because it reads like the C declaration
	// and because the assembler then gives it the element type's own alignment. Anything involving
	// a struct becomes a flat word array instead, with the type named in a comment: a struct-typed
	// `let` in CASM means `u8[P]`, whose alignment is one byte, so a word field of it could land
	// misaligned and fault (23-Structs.md - verified against the real assembler).
	CHECK_EQ(atO2(
		"int primes[4] = { 2, 3, 5, 7 };\n"
		"char name[8] = \"ada\";\n"
		"struct P { int x; int y; };\n"
		"struct P start = { 1, 2 };\n"
		"struct P cursor;\n"
		"int board[2][3];\n"
		"int first() { return primes[0]; }"),
		"@data\n"
		"global let primes: u32[4] = [2, 3, 5, 7]\n"
		"global let name: u8[8] = \"ada\"\n"
		"global let start: u32[2] = [0x00000001, 0x00000002]   // struct P (8 bytes)\n"
		"\n"
		"@bss\n"
		"global let cursor: u32[2]   // struct P (8 bytes)\n"
		"global let board: u32[2][3]\n"
		"\n"
		"@text\n"
		"\n"
		"// first - test.c:7\n"
		"global first:\n"
		".L0:\n"
		"    la r3, primes         // test.c:7\n"
		"    ldr r2, [r3]          // test.c:7\n"
		"    mov r0, r2            // test.c:7\n"
		"    ret                   // test.c:7\n");
}

TEST(codegen, a_nested_global_array_initializer_keeps_its_nesting)
{
	std::string text = atO2("int grid[2][3] = { { 1, 2, 3 }, { 4, 5, 6 } }; int first() { return grid[0][0]; }");
	CHECK(contains(text, "global let grid: u32[2][3] = [[1, 2, 3], [4, 5, 6]]"));
}

TEST(codegen, a_global_array_accepts_individually_braced_scalar_values)
{
	std::string text = atO2("int values[2] = { { 1 }, { 2 } }; int first() { return values[0]; }");
	CHECK(contains(text, "global let values: u32[2] = [1, 2]"));
}

TEST(codegen, a_nested_char_array_global_uses_byte_lists_for_string_rows)
{
	std::string text = atO2("char names[2][4] = { \"ab\", \"cd\" }; int first() { return names[1][0]; }");
	CHECK(contains(text, "global let names: u8[2][4] = [\"ab\", \"cd\"]"));
}

TEST(codegen, a_global_float_array_declares_f32_elements)
{
	std::string text = atO2("float scale[2] = { 1.5, 2.5 }; float first() { return scale[0]; }");
	CHECK(contains(text, "global let scale: f32[2] = [1.5, 2.5]"));
}

TEST(codegen, a_global_structs_word_image_places_narrow_fields_at_their_real_offsets)
{
	// `char a` at 0, `int b` at 4, `short c` at 8 - sema's layout (type_layout.h), the same rule
	// CASM's own `struct` follows, written out as the little-endian words the program will address.
	std::string text = atO2(
		"struct Mixed { char a; int b; short c; };"
		"struct Mixed m = { 1, 2, 3 };"
		"int first() { return m.b; }");
	CHECK(contains(text, "global let m: u32[3] = [0x00000001, 0x00000002, 0x00000003]   // struct Mixed (12 bytes)"));
}

TEST(codegen, a_non_constant_global_initializer_is_diagnosed_rather_than_guessed_at)
{
	// .data needs a literal at ASSEMBLE time, so there is nothing to emit here - and staying silent
	// would mean a zero nobody asked for.
	support::SourceManager sourceManager;
	support::SourceId sourceId = sourceManager.registerBuffer("test.c", "int n; int a[2] = { n, 1 };");
	support::Arena arena;
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	lexer::Lexer lexer("int n; int a[2] = { n, 1 };", sourceId, diagnostics, pool);
	parser::Parser parser(lexer, arena, diagnostics);
	ast::TranslationUnit* unit = parser.parseTranslationUnit();
	CHECK(unit != nullptr);
	sema::Sema sema(arena, diagnostics);
	CHECK(sema.check(*unit));

	support::OptimizationOptions options = support::OptimizationOptions::forLevel(support::OptimizationLevel::O2);
	ir::IrBuilder builder(arena, diagnostics, options);
	ir::IrModule module = builder.build(*unit);
	codegen::CodeGen codeGen(sourceManager, diagnostics, options);
	codeGen.generate(*unit, module);
	CHECK(diagnostics.hasErrors());
}

TEST(codegen, a_struct_returning_function_takes_a_hidden_destination_pointer_in_arg0)
{
	// The visible parameter moves to r1 because r0 carries the destination - the ABI table's
	// "argumentos visibles corridos uno". The callee returns that same pointer in ret0.
	std::string text = atO2(
		"struct P { int x; int y; };"
		"struct P scaled(int n) { struct P p; p.x = n; p.y = n; return p; }");
	CHECK(contains(text, "scaled:"));
	CHECK(contains(text, "mov r0, "));  // the destination pointer goes back out in ret0
	CHECK(contains(text, "r1"));        // n arrived one register along
}

TEST(codegen, a_struct_copy_moves_one_word_per_four_aligned_bytes)
{
	// Unrolled loads and stores, no call to a memcpy that does not exist (§14) - and exactly as many
	// pairs as the struct has words.
	std::string text = atO2(
		"struct Three { int a; int b; int c; };"
		"void copy(struct Three* to, struct Three* from) { *to = *from; }");
	CHECK_EQ(countOf(text, "    ldr "), usize(3));
	CHECK_EQ(countOf(text, "    str "), usize(3));
}

TEST(codegen, a_struct_of_bytes_is_copied_one_byte_at_a_time)
{
	// Its alignment is 1, so a word load off it could fault - the copy's piece size follows the
	// type, not the frame slot it happens to sit in.
	std::string text = atO2(
		"struct Bytes { char a; char b; char c; };"
		"void copy(struct Bytes* to, struct Bytes* from) { *to = *from; }");
	CHECK_EQ(countOf(text, "    ldrb "), usize(3));
	CHECK_EQ(countOf(text, "    strb "), usize(3));
}

TEST(codegen, integer_negation_writes_out_the_imul_expansion_instead_of_the_neg_pseudo)
{
	// `neg rd, rs` is the documented spelling (06-Pseudo-Instructions.md) and it is what this used
	// to emit - but the assembler currently expands it wrongly: `neg r2, r1` assembles to
	// `IMUL r2, r15, r0`, so the source register is dropped and every negation produces garbage.
	// Writing the expansion out by hand assembles correctly and means exactly the same thing.
	//
	// This test is the reason not to "tidy" it back: if it ever goes red because the emitter says
	// `neg` again, check the assembler first. docs/06-Known-Limitations.md has the reproducer.
	std::string text = atO0("int negate(int n) { return -n; }");
	CHECK(contains(text, "imul "));
	CHECK(contains(text, ", -1"));
	CHECK(!contains(text, "    neg "));
}

TEST(codegen, float_negation_still_uses_the_neg_pseudo)
{
	// The float form of the same pseudo maps to the real FNEG opcode and assembles correctly, so it
	// is left alone - the workaround above is as narrow as the bug it works around.
	std::string text = atO0("float negate(float x) { return -x; }");
	CHECK(contains(text, "neg f"));
}

// ---- integer conversions: width and signedness ------------------------------------------------

TEST(codegen, a_signed_narrow_load_uses_the_sign_extending_instruction)
{
	// `ldrb`/`ldrh` zero-extend and `ldrsb`/`ldrsh` sign-extend (05-Instruction-Set.md). Which pair
	// a load gets comes from the loaded type, and getting it from nowhere at all is what used to
	// make a negative `signed char` read back as 253.
	std::string signedByte = atO0("int read(signed char* p) { return *p; }");
	CHECK(contains(signedByte, "ldrsb "));
	CHECK(!contains(signedByte, "ldrb "));

	std::string unsignedByte = atO0("int read(unsigned char* p) { return *p; }");
	CHECK(contains(unsignedByte, "ldrb "));
	CHECK(!contains(unsignedByte, "ldrsb "));

	std::string signedHalf = atO0("int read(short* p) { return *p; }");
	CHECK(contains(signedHalf, "ldrsh "));

	std::string unsignedHalf = atO0("int read(unsigned short* p) { return *p; }");
	CHECK(contains(unsignedHalf, "ldrh "));
	CHECK(!contains(unsignedHalf, "ldrsh "));
}

TEST(codegen, a_word_load_never_asks_for_an_extension_it_does_not_need)
{
	// `int` is signed, but a word load already fills the register - there is no `ldrs` form and
	// asking for one by mechanically threading isSigned() through would be a syntax error.
	std::string text = atO0("int read(int* p) { return *p; }");
	CHECK(contains(text, "ldr "));
	CHECK(!contains(text, "ldrs"));
}

TEST(codegen, narrowing_to_a_signed_type_sign_extends_and_to_an_unsigned_one_masks)
{
	// `sxtb`/`sxth` for the signed side; `and` with a zero-extended immediate for the unsigned one
	// (verified against the assembler: ANDI's imm16 does NOT sign-extend, so 65535 arrives intact).
	std::string toChar = atO2("int opaque(int v); int f(int n) { return (signed char)opaque(n); }");
	CHECK(contains(toChar, "sxtb "));

	std::string toShort = atO2("int opaque(int v); int f(int n) { return (short)opaque(n); }");
	CHECK(contains(toShort, "sxth "));

	std::string toUChar = atO2("int opaque(int v); int f(int n) { return (unsigned char)opaque(n); }");
	CHECK(contains(toUChar, "and "));
	CHECK(contains(toUChar, ", 255"));

	std::string toUShort = atO2("int opaque(int v); int f(int n) { return (unsigned short)opaque(n); }");
	CHECK(contains(toUShort, ", 65535"));
}

TEST(codegen, converting_to_bool_is_one_unsigned_min_and_no_branch)
{
	// C says "zero stays zero, anything else becomes one", which unsigned `min` states exactly.
	// Doing it as a comparison would cost the four-instruction setcc synthesis instead (§9).
	std::string text = atO2("int opaque(int v); bool f(int n) { return opaque(n); }");
	CHECK(contains(text, "min "));
	CHECK(contains(text, ", 1"));
}

TEST(codegen, a_float_converted_to_bool_is_compared_with_zero)
{
	std::string text = atO2("bool f(float x) { return x; }");
	CHECK(contains(text, "ifne f"));
	CHECK(!contains(text, "ftoi"));
}

TEST(codegen, a_narrowing_conversion_of_a_constant_folds_away)
{
	// Every `char c = 'a';` goes through a Narrow now. If it survived to the back end, -O1 would
	// emit an `sxtb` of a literal in front of each one.
	std::string text = atO2("char letter() { return 'a'; }");
	CHECK(!contains(text, "sxtb "));
	CHECK(!contains(text, "and "));
}

// ---- the variadic calling convention ---------------------------------------------------------
// docs/09-Variadic-Convention.md is the contract these pin: fixed arguments follow the ordinary
// rule, and everything past them goes to the outgoing stack area whatever bank it belongs to.

TEST(codegen, a_variadic_calls_tail_goes_to_the_stack_even_with_argument_registers_free)
{
	// `f` declares one fixed parameter, so only `1` may use r0 - the other three arguments are the
	// tail and take outgoing words 0, 1 and 2, although r1-r3 are untouched.
	std::string casm = atO0("int f(int a, ...); int main() { return f(1, 2, 3, 4); }");
	CHECK(contains(casm, "mov r0, "));
	CHECK(contains(casm, "str [sp + 0],"));
	CHECK(contains(casm, "str [sp + 4],"));
	CHECK(contains(casm, "str [sp + 8],"));
	CHECK(!contains(casm, "mov r1, "));
}

TEST(codegen, a_float_in_the_tail_goes_to_the_stack_rather_than_to_a_float_register)
{
	std::string casm = atO0("int f(int a, ...); int main() { return f(1, 2.5); }");
	CHECK(contains(casm, "str [sp + 0],"));
	CHECK(!contains(casm, "mov f0, "));
}

TEST(codegen, an_ordinary_call_still_uses_both_banks_of_argument_registers)
{
	// The same shape without the ellipsis, so the contrast above is about the ellipsis and not
	// about how these arguments happen to be written.
	std::string casm = atO0("int f(int a, float b); int main() { return f(1, 2.5); }");
	CHECK(contains(casm, "mov f0, "));
	CHECK(!contains(casm, "str [sp + 0],"));
}

TEST(codegen, a_variadic_function_reads_its_tail_past_its_own_stack_parameters)
{
	// One fixed parameter, which arrives in r0 and takes no incoming stack word, so the tail
	// starts at the first one: [fp + 8].
	std::string casm = atO0("int f(int a, ...) { va_list ap; va_start(ap, a); return va_arg(ap, int); }");
	CHECK(contains(casm, "la r"));
	CHECK(contains(casm, ", [fp + 8]"));

	// Six fixed parameters: r0-r3 hold four of them and the last two arrive on the stack at
	// [fp + 8] and [fp + 12], so the tail can only begin at [fp + 16].
	std::string spilled = atO0(
		"int g(int a, int b, int c, int d, int e, int h, ...)"
		"{ va_list ap; va_start(ap, h); return va_arg(ap, int); }");
	CHECK(contains(spilled, ", [fp + 16]"));
}

TEST(codegen, a_variadic_function_always_gets_a_frame)
{
	// Nothing else in this function needs one, but it cannot address [fp + N] without an `enter`.
	std::string casm = atO2("int f(int a, ...) { va_list ap; va_start(ap, a); return va_arg(ap, int); }");
	CHECK(contains(casm, "enter"));
	CHECK(contains(casm, "leave"));
}

// ---- volatile keeps a memory home --------------------------------------------------------------

TEST(codegen, a_volatile_parameter_keeps_a_frame_slot_instead_of_a_register)
{
	// docs/06 promises that a volatile object always retains a memory home, and ValuePlacement has
	// the guard that keeps it - but it reads IrLocalSlot::isVolatile, and the slots reserved for
	// PARAMETERS used to be built without that flag. The accesses were marked, so the optimizer
	// left them alone, and the back end then deleted the object they were accesses to: `x + x`
	// compiled to two `mov`s off r0 and touched memory exactly never.
	std::string casm = atO2("int f(volatile int x) { return x + x; }");
	CHECK(contains(casm, "struct __frame_f"));
	CHECK_EQ(countOf(casm, "ldr r"), usize(2)); // one real load per read, neither forwarded
}

TEST(codegen, an_ordinary_parameter_is_still_promoted_to_a_register)
{
	// The contrast that makes the test above about `volatile` rather than about parameters never
	// getting registers at all.
	std::string casm = atO2("int f(int x) { return x + x; }");
	CHECK(!contains(casm, "struct __frame_f"));
	CHECK_EQ(countOf(casm, "ldr r"), usize(0));
}
