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

#include <algorithm>
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

	// The mirror of generateCasm(): the same pipeline, but the diagnostics are the RESULT rather
	// than something asserted away. For the handful of things only the back end can refuse.
	struct BackEndOutcome
	{
		std::string casm;
		std::vector<std::string> errors;
	};

	BackEndOutcome generateExpectingDiagnostics(std::string_view source)
	{
		support::SourceManager sourceManager;
		support::SourceId sourceId = sourceManager.registerBuffer("test.c", std::string(source));
		support::Arena arena;
		support::DiagnosticEngine diagnostics;
		support::StringPool pool;
		support::OptimizationOptions options = support::OptimizationOptions::forLevel(support::OptimizationLevel::O2);

		lexer::Lexer lexer(source, sourceId, diagnostics, pool);
		parser::Parser parser(lexer, arena, diagnostics);
		ast::TranslationUnit* unit = parser.parseTranslationUnit();
		CHECK(unit != nullptr);
		sema::Sema sema(arena, diagnostics);
		CHECK(sema.check(*unit));

		ir::IrBuilder builder(arena, diagnostics, options);
		ir::IrModule module = builder.build(*unit);
		codegen::CodeGen codeGen(sourceManager, diagnostics, options);

		BackEndOutcome outcome;
		outcome.casm = codeGen.generate(*unit, module);
		for (const support::Diagnostic& diagnostic : diagnostics.diagnostics())
			if (diagnostic.severity == support::DiagnosticSeverity::Error)
				outcome.errors.push_back(support::diagnosticCode(diagnostic.id) + ": " + diagnostic.message);
		return outcome;
	}
}

TEST(codegen, a_trailing_comment_names_the_file_the_line_was_written_in)
{
	// The comments are the whole product (README), and until a line map reached CodeGen every one
	// of them cited a line of the preprocessor's EXPANDED buffer under the .c file's name. With no
	// `#include` that happens to be the same number - a directive leaves its blank line behind - so
	// the bug was invisible until a header shifted everything below it.
	//
	// Built by hand rather than by running the preprocessor: libs/codegen does not depend on it,
	// which is exactly why the map lives in libs/support.
	support::SourceManager sourceManager;
	support::SourceId header = sourceManager.registerBuffer("header.h", "int helper(int v) { return v + 1; }\n");
	std::string_view expandedText = "int helper(int v) { return v + 1; }\nint caller(int n) { return helper(n); }\n";
	support::SourceId expanded = sourceManager.registerBuffer("main.c", std::string(expandedText));

	support::LineMap lineMap;
	lineMap.append(1, header, 1);   // line 1 of the expansion came from the header
	lineMap.append(2, expanded, 2); // line 2 is main.c's own second line
	lineMap.setExpandedSourceId(expanded);

	support::Arena arena;
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	support::OptimizationOptions options = support::OptimizationOptions::forLevel(support::OptimizationLevel::O0);
	lexer::Lexer lexer(expandedText, expanded, diagnostics, pool);
	parser::Parser parser(lexer, arena, diagnostics);
	ast::TranslationUnit* unit = parser.parseTranslationUnit();
	CHECK(unit != nullptr);
	sema::Sema sema(arena, diagnostics);
	CHECK(sema.check(*unit));
	ir::IrBuilder builder(arena, diagnostics, options);
	ir::IrModule module = builder.build(*unit);

	codegen::CodeGen mapped(sourceManager, diagnostics, options);
	mapped.setLineMap(&lineMap);
	std::string text = mapped.generate(*unit, module);
	CHECK(contains(text, "// header.h:1"));
	CHECK(contains(text, "// main.c:2"));
	CHECK(!contains(text, "// main.c:1"));

	// Without the map, both functions claim the same file - which is what the output used to say.
	codegen::CodeGen unmapped(sourceManager, diagnostics, options);
	std::string plain = unmapped.generate(*unit, module);
	CHECK(contains(plain, "// main.c:1"));
	CHECK(!contains(plain, "header.h"));
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
		"    shl r5, r0, 8         // test.c:1\n"
		"    or r5, r5, 1          // test.c:1\n"
		"    str [r4 + 0], r5      // test.c:1\n"
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
		"    shl r5, r0, 8         // test.c:1\n"
		"    or r5, r5, 1          // test.c:1\n"
		"    str [r4 + 0], r5      // test.c:1\n"
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
		"    add r3, r0, r1        // test.c:1\n"
		"    mov r0, r3            // test.c:1\n"
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
		"    ifls r0, r1, .cmp0_true // test.c:1\n"
		"    li r3, 0              // test.c:1\n"
		"    jp .cmp0_end          // test.c:1\n"
		".cmp0_true:\n"
		"    li r3, 1              // test.c:1\n"
		".cmp0_end:\n"
		"    mov r0, r3            // test.c:1\n"
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
		"    ifge r0, r1, .L2      // test.c:1\n"
		".L1:\n"
		"    li r3, 1              // test.c:1\n"
		"    mov r0, r3            // test.c:1\n"
		"    ret                   // test.c:1\n"
		".L2:\n"
		"    li r3, 0              // test.c:1\n"
		"    mov r0, r3            // test.c:1\n"
		"    ret                   // test.c:1\n");
}

TEST(codegen, a_short_circuit_boolean_used_as_a_value_keeps_a_register_and_no_frame)
{
	// `&&`/`||`/`!` used as a VALUE lower to a phi-shaped temporary (materializeBoolean,
	// ir_builder.cpp): one result defined in two branch blocks and read in the join. The per-block
	// register scan cannot hold a register across the branch, so until now the value always
	// spilled - and in a leaf, that single spill was what opened a frame. The cross-block pass now
	// keeps it in a register, so the function is frameless and touches no memory at all.
	std::string casm = atO2("int isupper(int c) { return c >= 65 && c <= 90; }");
	CHECK(!contains(casm, "struct __frame_isupper"));
	CHECK(!contains(casm, "enter"));
	CHECK(!contains(casm, "leave"));
	CHECK(!contains(casm, "[sp +"));
	CHECK(contains(casm, "li r3, 1"));
	CHECK(contains(casm, "li r3, 0"));
}

TEST(codegen, a_ternary_value_keeps_a_register_and_no_frame)
{
	// visit(TernaryExpr&) emits the same phi shape - a Copy into one result in each branch - so
	// `?:` used as a value gets the same treatment as `&&`/`||`/`!`.
	std::string casm = atO2("int pick(int a, int b) { return a ? b : 0; }");
	CHECK(!contains(casm, "struct __frame_pick"));
	CHECK(!contains(casm, "enter"));
	CHECK(!contains(casm, "[sp +"));
}

TEST(codegen, a_float_ternary_value_keeps_the_float_bank)
{
	// The cross-block pass picks the bank off the defining instruction, so a float `?:` lives in
	// the float registers rather than spilling to an integer frame field.
	std::string casm = atO2("float pick(float a, float b) { return a ? b : 0.0; }");
	CHECK(!contains(casm, "struct __frame_pick"));
	CHECK(!contains(casm, "[sp +"));
	CHECK(contains(casm, "mov f3, f1"));
}

TEST(codegen, a_short_circuit_value_passed_to_a_call_uses_the_call_free_pool)
{
	// In a calling function the cross-block result takes a caller-saved register (r12 here) whose
	// live range is call-free: it is set up as the argument, and the call clobbers it only after
	// its last read. No frame is needed for it, so the whole function is frameless.
	std::string casm = atO2("int g(int v); int pass(int a, int b) { return g(a && b); }");
	CHECK(!contains(casm, "struct __frame_pass"));
	CHECK(!contains(casm, "enter"));
	CHECK(!contains(casm, "[sp +"));
	CHECK(contains(casm, "li r12, 1"));
	CHECK(contains(casm, "li r12, 0"));
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
	// Fifteen frame slots become none. `factorial` calls itself, so its parameter cannot live in a
	// caller-saved register across the call - but it can live in a callee-saved one (r8), saved and
	// restored by the pushm/popm pair around the body (Fase 1 of the register-allocation plan,
	// docs/12-Register-Allocation-Extension-Plan.md). Fase 5's load-of-a-register-resident-local
	// aliasing keeps `n` in r8 across the recursive call with no copy, so the frame is empty and the
	// function is frameless: no struct, no enter/leave.
	CHECK_EQ(atO2("int factorial(int n) { if (n <= 1) return 1; return n * factorial(n - 1); }"),
		"@text\n"
		"\n"
		"// factorial - test.c:1\n"
		"global factorial:\n"
		"    pushm 0x0100          // test.c:1\n"
		"    mov r8, r0            // test.c:1\n"
		".L0:\n"
		"    ifgr r8, 1, .L2       // test.c:1\n"
		".L1:\n"
		"    li r12, 1             // test.c:1\n"
		"    mov r0, r12           // test.c:1\n"
		"    popm 0x0100           // test.c:1\n"
		"    ret                   // test.c:1\n"
		".L2:\n"
		"    sub r7, r8, 1         // test.c:1\n"
		"    mov r0, r7            // test.c:1\n"
		"    call factorial        // test.c:1\n"
		"    mov r7, r0            // test.c:1\n"
		"    imul r12, r8, r7      // test.c:1\n"
		"    mov r0, r12           // test.c:1\n"
		"    popm 0x0100           // test.c:1\n"
		"    ret                   // test.c:1\n");
}

TEST(codegen, a_load_aliased_to_a_register_is_snapshotted_before_a_store_redefines_it)
{
	// `buf[i++] = ...` reads `i` twice: once to store the digit and once, later, to compute the
	// array index. `i` lives in a register, so Fase 5 aliases its load to that register - but the
	// increment is a Store to `i` that rewrites the register in between, so the index-add would read
	// the post-increment value and the store would land one past the last digit (the most significant
	// one, which is the only byte that then goes missing). The aliased load must instead be a copy
	// taken before the increment, which the store then folds as its index.
	std::string casm = atO2(
		"int f(unsigned v) { char buf[3]; int i = 0; while (v > 0) { buf[i++] = (char)(48 + (v % 10)); v /= 10; } return buf[0]; }");
	CHECK(contains(casm, "mov r1, r6"));          // the snapshot of i, taken before the increment
	CHECK(contains(casm, "add r7, r1, 1"));       // i + 1 computed from the snapshot, not from i itself
	CHECK(contains(casm, "mov r6, r7"));          // i = i + 1
	CHECK(contains(casm, "strb [r2 + r1], r3"));  // the store indexes by the snapshot
	CHECK(!contains(casm, "strb [r2 + r6]"));     // ...not by the register the increment just clobbered
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
		"    la r4, 1073741824     // test.c:1\n"
		"    mtf f3, r4            // test.c:1\n"
		"    div f2, f0, f3        // test.c:1\n"
		"    ftoii r3, f2          // test.c:1\n"
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
		"    shl r5, r0, 8         // test.c:1\n"
		"    or r5, r5, 1          // test.c:1\n"
		"    str [r4 + 0], r5      // test.c:1\n"
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
		"    shl r5, r0, 8         // test.c:1\n"
		"    or r5, r5, 1          // test.c:1\n"
		"    str [r4 + 0], r5      // test.c:1\n"
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
		"    ifge r7, r0, .L4      // test.c:1\n"
		".L2:\n"
		"    add r3, r6, r7        // test.c:1\n"
		"    mov r6, r3            // test.c:1\n"
		".L3:\n"
		"    add r2, r7, 1         // test.c:1\n"
		"    mov r7, r2            // test.c:1\n"
		"    jp .L1                // test.c:1\n"
		".L4:\n"
		"    mov r0, r6            // test.c:1\n"
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
	CHECK(contains(separate, "ifeq r3, 0, .L2")); // ...tested against zero, as its own instruction
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
	CHECK(contains(withImmediates, "add r2, r0, 1"));
	CHECK(!contains(withImmediates, "li ")); // nothing materializes the 1 at all

	std::string simplified = generateCasm(source, without(&support::OptimizationOptions::immediateOperands));
	CHECK(contains(simplified, "li r3, 1"));
	CHECK(contains(simplified, "add r2, r0, r3"));
}

TEST(codegen, a_constant_too_large_for_the_immediate_field_is_materialized_anyway)
{
	// 04-Instruction-Format.md gives the immediate 16 bits, and ADDI reads them zero-extended while
	// CMPI reads them sign-extended (verified in CeresASM's execution_engine.h) - so immediateFor()
	// only accepts [0, 32767], where both readings agree. Past that the constant has to go through
	// a register even with the peephole on.
	std::string text = atO2("int bump(int a) { return a + 100000; }");
	CHECK(contains(text, "la r3, 100000"));
	CHECK(contains(text, "add r2, r0, r3"));
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

TEST(codegen, a_local_narrower_than_a_word_still_earns_a_register)
{
	// A `char` and a `short` fit in a register with room to spare, so nothing about their width
	// should force a frame field. The IR keeps a narrow value in its narrowed representation at all
	// times (ir_instr.h), which is what makes the register hold exactly what a byte field would
	// have held.
	std::string text = atO2("int f(int n) { char c; short s; c = (char)n; s = (short)n; return c + s; }");
	CHECK(!contains(text, "struct __frame_f"));
	CHECK(!contains(text, "enter"));
}

TEST(codegen, a_sub_word_parameter_in_a_register_is_narrowed_by_the_prologue)
{
	// The one value a function does not produce itself. A `strb` into a frame field truncated the
	// caller's word for free; the register home has to say it, in the same one instruction, or a
	// caller that passed a wider word (hand-written CASM, say - docs/07-CASM-Interop.md) would be
	// read back unnarrowed.
	CHECK(contains(atO2("int f(char c) { char x = c; return x; }"), "sxtb r0, r0"));
	CHECK(contains(atO2("int f(unsigned char c) { unsigned char x = c; return x; }"), "and r0, r0, 255"));
	CHECK(contains(atO2("int f(short s) { short x = s; return x; }"), "sxth r0, r0"));
	CHECK(contains(atO2("int f(unsigned short s) { unsigned short x = s; return x; }"), "and r0, r0, 65535"));

	// And an int-width parameter keeps costing nothing at all.
	CHECK(!contains(atO2("int f(int v) { int x = v; return x; }"), "sxtb"));
}

TEST(codegen, a_local_wider_than_a_register_still_takes_a_frame_field)
{
	// The rule is "fits in a register", not "is not an int": an array does not fit however few
	// elements it has.
	std::string text = atO2("int f() { int a[2]; a[0] = 1; a[1] = 2; return a[0] + a[1]; }");
	CHECK(contains(text, "struct __frame_f"));
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
		"    mul r2, r1, 4         // test.c:1\n"
		"    ldr r2, [r0 + r2]     // test.c:1\n"
		"    mov r0, r2            // test.c:1\n"
		"    ret                   // test.c:1\n");
}

TEST(codegen, an_array_element_write_uses_the_indexed_store_form)
{
	std::string text = atO2("void put(int* a, int i, int v) { a[i] = v; }");
	CHECK(contains(text, "str [r0 + r12], r2"));
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
		"    ldr r3, [r0 + 4]      // test.c:1\n"
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

	CHECK(contains(folded, "ldr r2, [r0 + r2]"));
	CHECK(!contains(plain, "ldr r2, [r0 + r2]"));
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

TEST(codegen, a_string_literal_initializing_a_POINTER_writes_the_literals_address)
{
	// A pointer initializer asks for the literal's ADDRESS, not its bytes - so `char* n[2] =
	// {"a", "b"}` comes out as two symbol names, never {97, 98}. Each literal gets its own .rodata
	// label, minted here because the IR only sees string literals that appear in a function body.
	std::string pointerArray = atO2("char* names[2] = { \"a\", \"b\" };");
	CHECK(contains(pointerArray, "global let names: u32[2] = [__cclit0, __cclit1]"));
	CHECK(contains(pointerArray, "let __cclit0: u8[2] = \"a\""));
	CHECK(contains(pointerArray, "let __cclit1: u8[2] = \"b\""));
	CHECK(!contains(pointerArray, "97"));
	CHECK(!contains(pointerArray, "98"));

	std::string pointerField = atO2("struct S { char* s; }; struct S s = { \"x\" };");
	CHECK(contains(pointerField, "global let s: u32[1] = [__cclit0]"));
	CHECK(contains(pointerField, "let __cclit0: u8[2] = \"x\""));

	std::string plainPointer = atO2("char* p = \"abc\";");
	CHECK(contains(plainPointer, "global let p: u32 = __cclit0"));
	CHECK(contains(plainPointer, "let __cclit0: u8[4] = \"abc\""));
}

TEST(codegen, an_address_constant_initializer_names_the_symbol_it_points_at)
{
	// `&g` and an array's name both decay to the object's address, which is now a symbol the
	// assembler relocates - not "must be a compile-time constant", and not the object's first byte.
	std::string addressOf = atO2("int g; int* p = &g;");
	CHECK(contains(addressOf, "global let p: u32 = g"));

	std::string decayedArray = atO2("int a[2]; int* p = a;");
	CHECK(contains(decayedArray, "global let p: u32 = a"));

	// And something that really is not constant keeps the message that fits it.
	BackEndOutcome notConstant = generateExpectingDiagnostics("int n; int a[2] = { n, 1 };");
	CHECK_EQ(notConstant.errors.size(), usize{ 1 });
	CHECK(contains(notConstant.errors[0], "E4003"));
	CHECK(contains(notConstant.errors[0], "must be a compile-time constant"));
}

TEST(codegen, an_address_constant_with_an_offset_is_still_refused)
{
	// `a` decays to the array's address and works; `&a[3]` is that address plus a displacement,
	// which nothing can spell into a .data initializer - it is still a diagnostic, and a precise one.
	std::string base = atO2("int a[4]; int* p = a;");
	CHECK(contains(base, "global let p: u32 = a"));

	BackEndOutcome offset = generateExpectingDiagnostics("int a[4]; int* p = &a[3];");
	CHECK_EQ(offset.errors.size(), usize{ 1 });
	CHECK(contains(offset.errors[0], "E4006"));
	CHECK(contains(offset.errors[0], "an address with an offset"));
}

TEST(codegen, a_string_literal_still_fills_a_char_array_at_every_depth)
{
	// The other side of the same guard: an ARRAY takes the bytes, and that must not have changed.
	std::string text = atO2(
		"char greet[8] = \"hola\";"
		"char rows[2][4] = { \"ab\", \"cd\" };"
		"struct S { char name[4]; int n; };"
		"struct S s = { \"ab\", 7 };");
	CHECK(contains(text, "global let greet: u8[8] = \"hola\""));
	CHECK(contains(text, "global let rows: u8[2][4] = [\"ab\", \"cd\"]"));
	CHECK(contains(text, "global let s: u32[2] = [0x00006261, 0x00000007]"));
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
	// "argumentos visibles corridos uno". The callee writes through that same pointer in place, so
	// r0 never has to be moved anywhere and it still ends its life as the returned pointer.
	std::string text = atO2(
		"struct P { int x; int y; };"
		"struct P scaled(int n) { struct P p; p.x = n; p.y = n; return p; }");
	CHECK(contains(text, "scaled:"));
	CHECK(contains(text, "str [r0"));  // the destination is written through r0 directly
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
	std::string casm = atO0("int f(int a, ...) { __builtin_va_list ap; __builtin_va_start(ap, a); return __builtin_va_arg(ap, int); }");
	CHECK(contains(casm, "la r"));
	CHECK(contains(casm, ", [fp + 8]"));

	// Six fixed parameters: r0-r3 hold four of them and the last two arrive on the stack at
	// [fp + 8] and [fp + 12], so the tail can only begin at [fp + 16].
	std::string spilled = atO0(
		"int g(int a, int b, int c, int d, int e, int h, ...)"
		"{ __builtin_va_list ap; __builtin_va_start(ap, h); return __builtin_va_arg(ap, int); }");
	CHECK(contains(spilled, ", [fp + 16]"));
}

TEST(codegen, a_variadic_function_always_gets_a_frame)
{
	// Nothing else in this function needs one, but it cannot address [fp + N] without an `enter`.
	std::string casm = atO2("int f(int a, ...) { __builtin_va_list ap; __builtin_va_start(ap, a); return __builtin_va_arg(ap, int); }");
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

// ---- register reorders the allocator's preferences ---------------------------------------------

TEST(codegen, register_wins_the_pool_over_a_local_that_did_not_ask)
{
	// IrLocalSlot::preferRegister was written by IrBuilder and read by nobody: the pool was handed
	// out in declaration order, so a `register` local declared late lost to ordinary variables
	// declared early and the keyword changed nothing at all in the generated code.
	//
	// Loop-carried locals, because that is what survives to ValuePlacement: in straight-line code
	// store-to-load forwarding removes the locals entirely long before the allocator sees them, and
	// the only thing left to place is temporaries.
	std::string_view program =
		"int f(int n)"
		"{"
		"    int a = n; int b = n; int c = n; int d = n; int e = n; int g = n;"
		"    {}REGISTER{}int hot = 0;"
		"    int k = 0;"
		"    while (k < n) { hot = hot + a + b + c + d + e + g; k = k + 1; }"
		"    return hot;"
		"}";
	std::string withRegister(program);
	withRegister.replace(withRegister.find("{}REGISTER{}"), 12, "register ");
	std::string without(program);
	without.replace(without.find("{}REGISTER{}"), 12, "");

	std::string asked = atO2(withRegister);
	std::string did_not = atO2(without);
	CHECK(asked != did_not);

	// The variable that asked keeps its value in a register across the loop, so the body stops
	// paying for it. Same frame either way - one local still spills, just not this one.
	CHECK(countOf(asked, "ldr r") < countOf(did_not, "ldr r"));
	CHECK(countOf(asked, "str [") < countOf(did_not, "str ["));
}

TEST(codegen, a_register_parameter_competes_for_the_pool_like_any_other_register_local)
{
	// A parameter is a local slot like any other, so `register` on one has to reach the same queue.
	// Loop-carried locals for the same reason as the test above, and enough of them asking to drain
	// a call-free function's pool - which is the only situation in which the ORDER can show.
	std::string_view program =
		"int f(int n, {}REGISTER{}int p)"
		"{"
		"    register int a = n; register int b = n; register int c = n; register int d = n;"
		"    register int e = n; register int g = n; register int h = n;"
		"    int k = 0; int s = 0;"
		"    while (k < n) { s = s + a + b + c + d + e + g + h + p; k = k + 1; }"
		"    return s;"
		"}";
	std::string asked(program);
	asked.replace(asked.find("{}REGISTER{}"), 12, "register ");
	std::string did_not(program);
	did_not.replace(did_not.find("{}REGISTER{}"), 12, "");

	// The parameter that asked keeps the register it arrived in, so the prologue has nothing at all
	// to emit for it; the one that did not is spilled to a field on the way past.
	CHECK(contains(atO2(did_not), "], r1 //"));
	CHECK(!contains(atO2(asked), "], r1 //"));
}

TEST(codegen, register_never_relaxes_a_rule_that_is_there_for_correctness)
{
	// The keyword reorders preferences and nothing else: a volatile local needs a memory home
	// whatever it asked for, so the frame stays.
	std::string casm = atO2("int f(int n) { register volatile int x = n; return x + 1; }");
	CHECK(contains(casm, "struct __frame_f"));
}

TEST(codegen, a_call_free_temporary_takes_an_argument_register_in_a_calling_function)
{
	// Fase 3: r0-r3/f0-f3 are only the argument registers while a call is being set up. A temporary
	// whose live range is call-free, and which is not itself a call argument, may use one even in a
	// function that calls - here, under enough pressure, `i` loaded out of its frame field lands in
	// r3 and feeds the `(h + i)` add directly, a register the pre-Fase-3 pool never handed out to a
	// call-making function.
	std::string casm = atO2("int g(int v); int f(int a, int b, int c, int d, int e, int h, int i, int j) { return g(a) + (b+c) + (d+e) + (h+i) + (a+j); }");
	CHECK(contains(casm, "add r6, r12, r3"));
}

// ---- machine builtins --------------------------------------------------------------------------

TEST(codegen, the_machine_builtins_emit_their_one_instruction)
{
	std::string casm = atO2("int main(void) { __builtin_cli(); __builtin_sti(); return 0; }");
	CHECK(contains(casm, "\n    cli"));
	CHECK(contains(casm, "\n    sti"));
}

TEST(codegen, a_machine_builtin_does_not_cost_a_function_its_register_window)
{
	// It is not a Call and clobbers nothing, so ValuePlacement must not treat it as one - a leaf
	// function that only masks interrupts still keeps its locals in registers.
	std::string casm = atO2("int f(int n) { __builtin_cli(); return n + n; }");
	CHECK(!contains(casm, "struct __frame_f"));
}

// ---- interrupt handlers ------------------------------------------------------------------------

TEST(codegen, an_interrupt_handler_saves_every_register_it_could_touch_and_ends_in_iret)
{
	// The hardware pushes the flags and the PC and nothing else, and the code this preempted never
	// agreed to lose a register - so the caller/callee split of the calling convention does not
	// apply. r0-r12 go back in one pushm/popm pair (0x1FFF: bits 0-12).
	std::string casm = atO2("__interrupt void h(void) { char* p = (char*)0xFF000004; *p = 65; }");
	CHECK(contains(casm, "pushm 0x1FFF"));
	CHECK(contains(casm, "popm 0x1FFF"));
	CHECK(contains(casm, "iret"));
	CHECK(!contains(casm, "\n    ret"));

	// No float in sight, so no fpushm either.
	CHECK(!contains(casm, "fpushm"));
}

TEST(codegen, an_interrupt_handler_that_touches_the_float_bank_saves_it_too)
{
	// The caller-saved half of the float bank (f0-f7) goes back in one fpushm/fpopm pair. The
	// callee-saved f8-f15 never join the mask: only a PARAMETER can earn one (a non-parameter local
	// is either forwarded away or escaped to memory), and a handler has no parameters.
	std::string casm = atO2("float g; __interrupt void h(void) { g = g + 1.0; }");
	CHECK(contains(casm, "fpushm 0x00FF"));
	CHECK(contains(casm, "fpopm 0x00FF"));
	CHECK(!contains(casm, "fpushm 0x01FF"));
}

TEST(codegen, an_interrupt_handler_saves_before_it_opens_its_frame)
{
	// `leave` restores sp from fp, so the restore only finds the saved registers where it left them
	// if the save happened before `enter`.
	std::string casm = atO2(
		"__interrupt void h(void) { int a[4]; a[0] = 1; a[1] = a[0]; }");
	usize save = casm.find("pushm");
	usize open = casm.find("enter");
	usize close = casm.find("leave");
	usize restore = casm.find("popm");
	CHECK(save < open);
	CHECK(close < restore);
}

TEST(codegen, a_static_interrupt_handler_survives_unused_function_elimination)
{
	// Nothing calls it and nothing names it - the machine reaches it through the vector table, an
	// edge with no instruction at all at the far end. Without rooting it, -O2 deleted it.
	std::string casm = atO2("static __interrupt void h(void) { char* p = (char*)0xFF000004; *p = 65; }");
	CHECK(contains(casm, "h:"));
	CHECK(contains(casm, "iret"));
}

TEST(codegen, an_interrupt_vector_binding_becomes_one_top_level_interrupt_line)
{
	// It emits neither code nor data - only a binding the linker resolves and the loader applies
	// before the program's first instruction - so it goes above every section.
	std::string casm = atO2(
		"enum Irq { Terminal = 17 };"
		"__interrupt void h(void) { char* p = (char*)0xFF000004; *p = 65; }"
		"__interrupt_vector(Terminal, h);");

	// The NUMBER, not the name the C source used: an enum constant means nothing to the assembler.
	CHECK(contains(casm, "interrupt 17: h"));
	CHECK(casm.find("interrupt 17: h") < casm.find("@text"));
}

// ---- function pointers ---------------------------------------------------------------------------

TEST(codegen, an_indirect_call_goes_through_a_register)
{
	// `call rN` is the same mnemonic with a register operand - the assembler picks CALLR from the
	// operand's shape. r12 is free at the point of a call by the same rule that makes it
	// allocatable: nothing holds an allocatable register across one.
	std::string casm = atO2("int f(int x); int main() { int (*p)(int) = f; return p(1); }");
	CHECK(contains(casm, "la r"));        // the function's address, materialized
	CHECK(contains(casm, "call r12"));
}

TEST(codegen, a_call_by_name_still_names_its_callee)
{
	std::string casm = atO2("int f(int x); int main() { return f(1); }");
	CHECK(contains(casm, "call f"));
	CHECK(!contains(casm, "call r"));
}

TEST(codegen, a_static_function_whose_address_is_taken_survives_unused_function_elimination)
{
	// Nothing calls it by name - the only edge is the address itself. removeUnusedFunctions builds
	// its root set from Call instructions, so without also following a GlobalAddr that names a
	// function, -O2 deleted the body out from under the pointer.
	std::string casm = atO2(
		"static int hidden(int x) { return x + 1; }"
		"int (*get(void))(int) { return hidden; }");
	CHECK(contains(casm, "hidden:"));
}

// ---- words CeresASM reserves, and `__asm__("label")` -----------------------------------------------------------

TEST(codegen, a_c_name_that_is_a_reserved_word_of_the_assembler_is_written_with_a_prefix)
{
	std::string text = atO0(
		"int at(int x) { return x + 1; }\n"
		"int half = 20;\n"
		"static int word = 3;\n"
		"int r5(void) { return 2; }\n"
		"int main(void) { static int global = 4; return at(1) + half + word + r5() + global; }");
	CHECK(contains(text, "global __c_at:"));
	CHECK(contains(text, "call __c_at"));
	CHECK(contains(text, "global let __c_half: u32 = 20"));
	CHECK(contains(text, "la r"));                       // the references use the same name
	CHECK(contains(text, "__c_half"));
	CHECK(contains(text, "let __c_word: u32 = 3"));      // a static too
	CHECK(!contains(text, "global let __c_word"));
	CHECK(contains(text, "global __c_r5:"));
	CHECK(contains(text, "call __c_r5"));
	CHECK(!contains(text, "global at:"));
	CHECK(!contains(text, "global let half"));
}

TEST(codegen, names_that_only_look_like_reserved_words_are_left_alone)
{
	std::string text = atO0(
		"int add(int a) { return a; }\n"
		"int r16(void) { return 1; }\n"
		"int r(void) { return 2; }\n"
		"int f1x(void) { return 3; }\n"
		"int words = 4;\n"
		"int main(void) { return add(1) + r16() + r() + f1x() + words; }");
	CHECK(contains(text, "global add:"));
	CHECK(contains(text, "global r16:"));
	CHECK(contains(text, "global r:"));
	CHECK(contains(text, "global f1x:"));
	CHECK(contains(text, "global let words: u32 = 4"));
	CHECK(!contains(text, "__c_"));
}

TEST(codegen, a_reserved_name_is_written_the_same_wherever_it_is_used)
{
	// As the target of a call through a pointer built at load time, and as an interrupt handler
	std::string text = atO0(
		"int half(int x) { return x; }\n"
		"int (*table[1])(int) = { half };\n"
		"__interrupt void at(void) { }\n"
		"__interrupt_vector(20, at);\n"
		"int main(void) { return table[0](1); }");
	CHECK(contains(text, "global let table: u32[1] = __c_half") || contains(text, "__c_half"));
	CHECK(contains(text, "interrupt 20: __c_at"));
	CHECK(contains(text, "global __c_at:"));
}

TEST(codegen, an_asm_label_is_the_name_the_assembler_sees)
{
	std::string text = atO0(
		"extern int mine(int n) __asm__(\"hand_made\");\n"
		"int counter __asm__(\"the_counter\") = 5;\n"
		"int shown(int x) __asm__(\"shown_label\") { return x; }\n"
		"int main(void) { return mine(counter) + shown(1); }");
	CHECK(contains(text, "call hand_made"));
	CHECK(contains(text, "global let the_counter: u32 = 5"));
	CHECK(contains(text, "la r"));
	CHECK(contains(text, "the_counter"));
	CHECK(contains(text, "global shown_label:"));
	CHECK(contains(text, "call shown_label"));
	CHECK(!contains(text, "call mine"));
	CHECK(!contains(text, "global counter"));
}

TEST(codegen, an_asm_label_wins_over_the_prefix_and_reaches_every_declaration)
{
	std::string text = atO0(
		"int at(int) __asm__(\"plain_at\");\n"
		"int main(void) { return at(1); }\n"
		"int at(int x) { return x; }");
	CHECK(contains(text, "call plain_at"));
	CHECK(contains(text, "global plain_at:"));     // the definition carries it although only the first declaration wrote it
	CHECK(!contains(text, "__c_at"));
}

TEST(codegen, the_declarations_file_uses_the_names_the_assembler_sees)
{
	support::SourceManager sourceManager;
	const std::string source =
		"int at(int x) { return x; }\n"
		"extern int counter __asm__(\"the_counter\");\n"
		"int half = 1;\n"
		"int f(void) __asm__(\"f_label\");\n";
	support::SourceId sourceId = sourceManager.registerBuffer("test.c", std::string(source));
	support::Arena arena;
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	lexer::Lexer lexer(source, sourceId, diagnostics, pool);
	parser::Parser parser(lexer, arena, diagnostics);
	ast::TranslationUnit* unit = parser.parseTranslationUnit();
	CHECK(unit != nullptr);
	if (!unit) return;
	sema::Sema sema(arena, diagnostics);
	CHECK(sema.check(*unit));

	codegen::CodeGen codeGen(sourceManager, diagnostics, support::OptimizationOptions::forLevel(support::OptimizationLevel::O2));
	std::vector<std::string> names;
	for (const codegen::ExternalDeclaration& declaration : codeGen.collectExternalDeclarations(*unit))
		names.push_back(declaration.name);
	auto has = [&](const char* name) { return std::find(names.begin(), names.end(), name) != names.end(); };
	CHECK(has("__c_at"));
	CHECK(has("the_counter"));
	CHECK(has("__c_half"));
	CHECK(has("f_label"));
	CHECK(!has("at"));
	CHECK(!has("half"));
}

TEST(codegen, an_asm_label_that_is_itself_reserved_is_refused)
{
	BackEndOutcome outcome = generateExpectingDiagnostics("int f(void) __asm__(\"half\");\nint main(void) { return f(); }");
	CHECK_EQ(outcome.errors.size(), usize{ 1 });
	CHECK(contains(outcome.errors[0], "E4004"));
	CHECK(contains(outcome.errors[0], "asm label"));
}

TEST(codegen, a_name_that_would_meet_the_prefix_is_refused)
{
	// `at` is written __c_at, so a symbol the program calls __c_at would be the same one
	BackEndOutcome outcome = generateExpectingDiagnostics("int __c_at(void) { return 1; }\nint main(void) { return __c_at(); }");
	CHECK_EQ(outcome.errors.size(), usize{ 1 });
	CHECK(contains(outcome.errors[0], "E4007"));
	// A name that only starts like the prefix is fine
	CHECK(generateExpectingDiagnostics("int __c_mine(void) { return 1; }\nint main(void) { return __c_mine(); }").errors.empty());
}

// ---- __asm__ ------------------------------------------------------------------------------------------------------

TEST(codegen, inline_assembly_goes_into_the_function_as_written_a_line_at_a_time)
{
	std::string text = atO2(
		"void f(void)\n"
		"{\n"
		"    __asm__(\"li r0, 7\\n\\t  la r12, 0xFF000004  \\n.again:\\n\\tstrb [r12 + 0], r0\\n\\n  jnz .again\");\n"
		"}\n");
	CHECK(contains(text, "    li r0, 7"));                 // an instruction is indented and trimmed
	CHECK(contains(text, "    la r12, 0xFF000004"));
	CHECK(contains(text, "\n.again:\n"));                  // a label stands at the left margin
	CHECK(contains(text, "    strb [r12 + 0], r0"));
	CHECK(contains(text, "    jnz .again"));
	CHECK(!contains(text, "call"));                        // nothing is called: the text is the asm's own
}

TEST(codegen, a_function_with_inline_assembly_is_never_spliced_into_its_callers)
{
	// Small enough to be inlined if it were only what it looks like; its label would be defined twice if it were
	std::string text = atO2(
		"static int one(void) { __asm__(\".spot:\\n\\tnop\"); return 1; }\n"
		"int main(void) { return one() + one(); }\n");
	CHECK(contains(text, "call one"));
	size_t first = text.find(".spot:");
	CHECK(first != std::string::npos);
	CHECK(text.find(".spot:", first + 1) == std::string::npos);   // written once, in the function itself
}
