#include <ceresc/ir/ir_builder.h>
#include <ceresc/ir/ir_printer.h>
#include <ceresc/parser/parser.h>
#include <ceresc/sema/sema.h>
#include <ceresc/lexer/lexer.h>
#include <ceresc/ast/decl.h>
#include <ceresc/support/arena.h>
#include <ceresc/support/diagnostics.h>
#include <ceresc/support/string_pool.h>

#include <vector>

#include "framework.h"

using namespace ceresc;

namespace
{
	support::SourceId testSourceId() { return support::SourceId::make(1); }

	// Parses, type-checks and lowers `source` to IR, then prints the named function's IR as text
	// (ir_printer.h) - the same "print both sides, compare strings" trick test_sema.cpp/AstPrinter
	// already use, since IrFunction/BasicBlock have no std::formatter either. Asserts the whole
	// pipeline succeeded (parse + sema) before printing, so a broken fixture fails loudly instead of
	// silently comparing against "<not-found>".
	std::string functionIr(std::string_view source, std::string_view functionName = "main")
	{
		support::Arena arena;
		support::DiagnosticEngine diagnostics;
		support::StringPool pool;
		lexer::Lexer lexer(source, testSourceId(), diagnostics, pool);
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

		for (const auto& function : module.functions())
		{
			if (function->name() == functionName)
				return ir::IrPrinter{}.print(*function);
		}
		return "<function-not-found>";
	}

	// Same pipeline as functionIr(), but prints the whole IrModule (IrPrinter::print(const
	// IrModule&)) instead of a single function - the only path that also renders
	// IrModule::stringLiterals() (`global "..." = "..."`), which functionIr()'s per-function
	// print(const IrFunction&) never exercises.
	std::string moduleIr(std::string_view source)
	{
		support::Arena arena;
		support::DiagnosticEngine diagnostics;
		support::StringPool pool;
		lexer::Lexer lexer(source, testSourceId(), diagnostics, pool);
		parser::Parser parser(lexer, arena, diagnostics);

		ast::TranslationUnit* unit = parser.parseTranslationUnit();
		CHECK(unit != nullptr);
		if (!unit)
			return "<parse-failed>";

		sema::Sema sema(arena, diagnostics);
		CHECK(sema.check(*unit));

		ir::IrBuilder builder(arena, diagnostics);
		ir::IrModule module = builder.build(*unit);
		return ir::IrPrinter{}.print(module);
	}

	// Same pipeline as functionIr(), but for tests that care about what IrBuilder itself reports
	// through its DiagnosticEngine (see requireScalarValue()) rather than the IR text. sema IS
	// expected to accept these (and CHECK()ed below) - they deliberately feed IrBuilder a program
	// sema's own isAssignable() considers well-typed (a struct-by-value use), just one this subset
	// cannot actually lower correctly, so it is IrBuilder, not sema, that must be the one to report
	// it.
	bool irBuilderReports(std::string_view source, std::string_view needle)
	{
		support::Arena arena;
		support::DiagnosticEngine diagnostics;
		support::StringPool pool;
		lexer::Lexer lexer(source, testSourceId(), diagnostics, pool);
		parser::Parser parser(lexer, arena, diagnostics);

		ast::TranslationUnit* unit = parser.parseTranslationUnit();
		CHECK(unit != nullptr);
		if (!unit)
			return false;

		sema::Sema sema(arena, diagnostics);
		CHECK(sema.check(*unit));

		usize diagnosticsBeforeIr = diagnostics.diagnosticCount();
		ir::IrBuilder builder(arena, diagnostics);
		builder.build(*unit);

		for (usize i = diagnosticsBeforeIr; i < diagnostics.diagnosticCount(); ++i)
		{
			if (diagnostics.diagnostics()[i].message.find(needle) != std::string::npos)
				return true;
		}
		return false;
	}
}

// ---- literals / arithmetic ---------------------------------------------------------------------

TEST(ir, int_literal_return_lowers_to_a_single_const_and_ret)
{
	std::string text = functionIr("int main() { return 42; }");
	CHECK_EQ(text,
		"function main(params=0, locals=0) {\n"
		"L0:\n"
		"  %0 = const 42\n"
		"  ret %0\n"
		"}\n");
}

TEST(ir, arithmetic_expression_respects_precedence_via_temporaries)
{
	// 1 + 2 * 3 - the multiply's own temp must be computed before the add uses it.
	std::string text = functionIr("int main() { return 1 + 2 * 3; }");
	CHECK_EQ(text,
		"function main(params=0, locals=0) {\n"
		"L0:\n"
		"  %0 = const 1\n"
		"  %1 = const 2\n"
		"  %2 = const 3\n"
		"  %3 = mul %1, %2\n"
		"  %4 = add %0, %3\n"
		"  ret %4\n"
		"}\n");
}

TEST(ir, unsigned_division_sets_the_isUnsigned_flag_the_signed_one_does_not)
{
	std::string signedText = functionIr("int main() { int a; int b; return a / b; }");
	std::string unsignedText = functionIr("int main() { unsigned int a; unsigned int b; return a / b; }");
	CHECK(signedText.find("div") != std::string::npos);
	CHECK(unsignedText.find("div") != std::string::npos);
	CHECK(signedText != unsignedText);
}

TEST(ir, compound_assignment_promotes_its_operand_types_the_same_way_the_equivalent_binary_form_does)
{
	// `x %= y` means `x = x % y` by definition - both must agree on signedness (and, for `>>=`,
	// Shr vs Sar) because C's usual arithmetic conversions promote a narrow `unsigned char` target
	// to `int` (signed) before the operation itself ever runs, the same way sema's own
	// integerPromote()/commonArithmeticType() (sema.cpp) do for `x = x % y`. Getting this wrong
	// used to pick the *target*'s own raw (unsigned) signedness instead - this pins the two forms
	// producing the same isUnsigned choice for `mod`.
	std::string compoundText = functionIr("int main() { unsigned char x; int y; x %= y; return 0; }");
	std::string equivalentText = functionIr("int main() { unsigned char x; int y; x = x % y; return 0; }");
	CHECK(compoundText.find("mod ") != std::string::npos);   // signed mod, no ".u" suffix
	CHECK(equivalentText.find("mod ") != std::string::npos);
	CHECK(compoundText.find("mod.u") == std::string::npos);
	CHECK(equivalentText.find("mod.u") == std::string::npos);
}

TEST(ir, compound_shift_assignment_picks_sar_or_shr_from_the_promoted_type_not_the_raw_target)
{
	std::string text = functionIr("int main() { unsigned char x; int y; x >>= y; return 0; }");
	// Promoted to `int` (signed) before shifting, exactly like `x = x >> y;` would - never the
	// logical `shr` a raw unsigned char target would otherwise wrongly select.
	CHECK(text.find("sar ") != std::string::npos);
	CHECK(text.find("shr") == std::string::npos);
}

// ---- local/global/parameter addressing ----------------------------------------------------------

TEST(ir, local_variable_declaration_and_use_goes_through_a_frame_slot)
{
	std::string text = functionIr("int main() { int x = 5; return x; }");
	CHECK_EQ(text,
		"function main(params=0, locals=1) {\n"
		"L0:\n"
		"  %0 = const 5\n"
		"  %1 = &local 0\n"
		"  store.word [%1], %0\n"
		"  %2 = &local 0\n"
		"  %3 = load.word [%2]\n"
		"  ret %3\n"
		"}\n");
}

TEST(ir, parameters_occupy_slots_0_through_paramCount_minus_1)
{
	std::string text = functionIr("int add(int a, int b) { return a + b; }", "add");
	CHECK_EQ(text,
		"function add(params=2, locals=2) {\n"
		"L0:\n"
		"  %0 = &local 0\n"
		"  %1 = load.word [%0]\n"
		"  %2 = &local 1\n"
		"  %3 = load.word [%2]\n"
		"  %4 = add %1, %3\n"
		"  ret %4\n"
		"}\n");
}

TEST(ir, global_variable_use_goes_through_globaladdr_not_a_frame_slot)
{
	std::string text = functionIr("int g; int main() { return g; }");
	CHECK_EQ(text,
		"function main(params=0, locals=0) {\n"
		"L0:\n"
		"  %0 = &global \"g\"\n"
		"  %1 = load.word [%0]\n"
		"  ret %1\n"
		"}\n");
}

// ---- string literals ----------------------------------------------------------------------------

TEST(ir, a_string_literal_synthesizes_a_global_label_that_the_module_printer_resolves_to_its_value)
{
	std::string text = moduleIr("char* main() { return \"hi\"; }");
	// The function's own GlobalAddr references the label by name; IrModule::stringLiterals() (only
	// rendered by IrPrinter::print(const IrModule&), not the per-function overload) is what maps
	// that same label back to the literal's actual bytes.
	CHECK_EQ(text,
		"function main(params=0, locals=0) {\n"
		"L0:\n"
		"  %0 = &global \".str0\"\n"
		"  ret %0\n"
		"}\n"
		"global \".str0\" = \"hi\"\n");
}

// ---- calls ----------------------------------------------------------------------------------------

TEST(ir, call_emits_one_param_per_argument_before_the_call_itself)
{
	std::string text = functionIr("int add(int a, int b); int main() { return add(1, 2); }");
	CHECK_EQ(text,
		"function main(params=0, locals=0) {\n"
		"L0:\n"
		"  %0 = const 1\n"
		"  %1 = const 2\n"
		"  param %0\n"
		"  param %1\n"
		"  %2 = call add, 2\n"
		"  ret %2\n"
		"}\n");
}

TEST(ir, nested_call_as_an_argument_finishes_its_own_param_call_pair_before_the_outer_one_starts)
{
	std::string text = functionIr("int g(int x); int f(int x, int y); int main() { return f(g(1), 2); }");
	CHECK_EQ(text,
		"function main(params=0, locals=0) {\n"
		"L0:\n"
		"  %0 = const 1\n"
		"  param %0\n"
		"  %1 = call g, 1\n"
		"  %2 = const 2\n"
		"  param %1\n"
		"  param %2\n"
		"  %3 = call f, 2\n"
		"  ret %3\n"
		"}\n");
}

// ---- if / else --------------------------------------------------------------------------------

TEST(ir, if_without_else_branches_directly_to_the_merge_block_on_false)
{
	std::string text = functionIr("int main() { int x = 0; if (x) { x = 1; } return x; }");
	CHECK(text.find("br.ne") != std::string::npos);
	// then-block and merge-block both exist and the false edge of the condition skips straight to
	// whichever block follows the if - verified precisely by the for/if/break/continue golden test
	// below; this test only pins the coarse shape (a conditional branch exists) for a simpler input.
}

TEST(ir, if_else_both_branches_join_at_one_merge_block)
{
	std::string text = functionIr("int main() { int x = 0; if (x) { x = 1; } else { x = 2; } return x; }");
	CHECK_EQ(text,
		"function main(params=0, locals=1) {\n"
		"L0:\n"
		"  %0 = const 0\n"
		"  %1 = &local 0\n"
		"  store.word [%1], %0\n"
		"  %2 = &local 0\n"
		"  %3 = load.word [%2]\n"
		"  %4 = const 0\n"
		"  br.ne %3, %4, L1, L2\n"
		"L1:\n"
		"  %5 = &local 0\n"
		"  %6 = const 1\n"
		"  store.word [%5], %6\n"
		"  jmp L3\n"
		"L2:\n"
		"  %7 = &local 0\n"
		"  %8 = const 2\n"
		"  store.word [%7], %8\n"
		"  jmp L3\n"
		"L3:\n"
		"  %9 = &local 0\n"
		"  %10 = load.word [%9]\n"
		"  ret %10\n"
		"}\n");
}

// ---- while / do-while ---------------------------------------------------------------------------

TEST(ir, while_loop_jumps_back_to_a_header_block_that_tests_the_condition)
{
	std::string text = functionIr("int main() { int x = 0; while (x) { x = x - 1; } return x; }");
	CHECK_EQ(text,
		"function main(params=0, locals=1) {\n"
		"L0:\n"
		"  %0 = const 0\n"
		"  %1 = &local 0\n"
		"  store.word [%1], %0\n"
		"  jmp L1\n"
		"L1:\n"
		"  %2 = &local 0\n"
		"  %3 = load.word [%2]\n"
		"  %4 = const 0\n"
		"  br.ne %3, %4, L2, L3\n"
		"L2:\n"
		"  %5 = &local 0\n"
		"  %6 = &local 0\n"
		"  %7 = load.word [%6]\n"
		"  %8 = const 1\n"
		"  %9 = sub %7, %8\n"
		"  store.word [%5], %9\n"
		"  jmp L1\n"
		"L3:\n"
		"  %10 = &local 0\n"
		"  %11 = load.word [%10]\n"
		"  ret %11\n"
		"}\n");
}

TEST(ir, do_while_runs_the_body_once_before_its_first_condition_check)
{
	std::string text = functionIr("int main() { int x = 0; do { x = x + 1; } while (x); return x; }");
	CHECK_EQ(text,
		"function main(params=0, locals=1) {\n"
		"L0:\n"
		"  %0 = const 0\n"
		"  %1 = &local 0\n"
		"  store.word [%1], %0\n"
		"  jmp L1\n"
		"L1:\n"
		"  %2 = &local 0\n"
		"  %3 = &local 0\n"
		"  %4 = load.word [%3]\n"
		"  %5 = const 1\n"
		"  %6 = add %4, %5\n"
		"  store.word [%2], %6\n"
		"  jmp L2\n"
		"L2:\n"
		"  %7 = &local 0\n"
		"  %8 = load.word [%7]\n"
		"  %9 = const 0\n"
		"  br.ne %8, %9, L1, L3\n"
		"L3:\n"
		"  %10 = &local 0\n"
		"  %11 = load.word [%10]\n"
		"  ret %11\n"
		"}\n");
}

// ---- for, with break/continue nested in an if - the phase's own stated exit criterion -----------

TEST(ir, for_loop_with_break_and_continue_nested_in_an_if_has_exactly_the_expected_blocks_and_jumps)
{
	std::string text = functionIr(
		"int main() {\n"
		"    int i;\n"
		"    for (i = 0; i < 10; i = i + 1) {\n"
		"        if (i == 5) {\n"
		"            break;\n"
		"        } else {\n"
		"            continue;\n"
		"        }\n"
		"    }\n"
		"    return i;\n"
		"}\n");
	// Block creation order (not execution order) is what IrPrinter walks: FunctionDecl's entry
	// (L0), then ForStmt's own header/body/inc/exit (L1-L4), then the nested IfStmt's own
	// then/else/merge (L5-L7) - L7 (the if's merge block) is created but never reached, since both
	// branches (break/continue) end in their own unconditional jump; it stays as an empty,
	// unreachable "jmp L3" the way any not-yet-optimized compiler leaves dead code - see
	// lowerArithmetic()/emitVoid()'s own note on this phase doing no reachability analysis (that is
	// Fase 9's job, §13).
	CHECK_EQ(text,
		"function main(params=0, locals=1) {\n"
		"L0:\n"
		"  %0 = &local 0\n"
		"  %1 = const 0\n"
		"  store.word [%0], %1\n"
		"  jmp L1\n"
		"L1:\n"
		"  %2 = &local 0\n"
		"  %3 = load.word [%2]\n"
		"  %4 = const 10\n"
		"  %5 = cmp.lt %3, %4\n"
		"  %6 = const 0\n"
		"  br.ne %5, %6, L2, L4\n"
		"L2:\n"
		"  %7 = &local 0\n"
		"  %8 = load.word [%7]\n"
		"  %9 = const 5\n"
		"  %10 = cmp.eq %8, %9\n"
		"  %11 = const 0\n"
		"  br.ne %10, %11, L5, L6\n"
		"L3:\n"
		"  %12 = &local 0\n"
		"  %13 = &local 0\n"
		"  %14 = load.word [%13]\n"
		"  %15 = const 1\n"
		"  %16 = add %14, %15\n"
		"  store.word [%12], %16\n"
		"  jmp L1\n"
		"L4:\n"
		"  %17 = &local 0\n"
		"  %18 = load.word [%17]\n"
		"  ret %18\n"
		"L5:\n"
		"  jmp L4\n"
		"L6:\n"
		"  jmp L3\n"
		"L7:\n"
		"  jmp L3\n"
		"}\n");
}

// ---- switch -----------------------------------------------------------------------------------

TEST(ir, switch_lowers_to_a_comparison_chain_not_a_jump_table)
{
	std::string text = functionIr(
		"int main() {\n"
		"    int x = 1;\n"
		"    switch (x) {\n"
		"        case 1: return 10;\n"
		"        case 2: return 20;\n"
		"        default: return 0;\n"
		"    }\n"
		"}\n");
	// One cmp.eq-shaped br.eq per case value, in source order, falling through on mismatch.
	usize firstCase = text.find("br.eq");
	usize secondCase = text.find("br.eq", firstCase + 1);
	CHECK(firstCase != std::string::npos);
	CHECK(secondCase != std::string::npos);
	CHECK(text.find("jmp") != std::string::npos); // the final fallthrough to `default`
	CHECK(text.find("const 10") != std::string::npos);
	CHECK(text.find("const 20") != std::string::npos);
}

TEST(ir, switch_case_without_break_falls_through_to_the_next_case_via_an_explicit_jump)
{
	std::string text = functionIr(
		"int main() {\n"
		"    int x = 1;\n"
		"    int y = 0;\n"
		"    switch (x) {\n"
		"        case 1: y = y + 1;\n"
		"        case 2: y = y + 2; break;\n"
		"    }\n"
		"    return y;\n"
		"}\n");
	// The case-1 body block must end in an unconditional jmp *straight into the case-2 body block*
	// (no terminator of its own otherwise), never a `br.eq`/`br.ne`, and never left unterminated -
	// pin the exact shape with a golden comparison, not just that some `jmp` occurs somewhere
	// (every switch/case already emits several: the dispatch chain's own final fallthrough, and
	// case-2's own `break`, would both also satisfy a bare "a jmp exists somewhere" check).
	CHECK_EQ(text,
		"function main(params=0, locals=2) {\n"
		"L0:\n"
		"  %0 = const 1\n"
		"  %1 = &local 0\n"
		"  store.word [%1], %0\n"
		"  %2 = const 0\n"
		"  %3 = &local 1\n"
		"  store.word [%3], %2\n"
		"  %4 = &local 0\n"
		"  %5 = load.word [%4]\n"
		"  %6 = const 1\n"
		"  br.eq %5, %6, L1, L4\n"
		"L1:\n"
		"  %8 = &local 1\n"
		"  %9 = &local 1\n"
		"  %10 = load.word [%9]\n"
		"  %11 = const 1\n"
		"  %12 = add %10, %11\n"
		"  store.word [%8], %12\n"
		"  jmp L2\n"
		"L2:\n"
		"  %13 = &local 1\n"
		"  %14 = &local 1\n"
		"  %15 = load.word [%14]\n"
		"  %16 = const 2\n"
		"  %17 = add %15, %16\n"
		"  store.word [%13], %17\n"
		"  jmp L3\n"
		"L3:\n"
		"  %18 = &local 1\n"
		"  %19 = load.word [%18]\n"
		"  ret %19\n"
		"L4:\n"
		"  %7 = const 2\n"
		"  br.eq %5, %7, L2, L5\n"
		"L5:\n"
		"  jmp L3\n"
		"}\n");
}

// ---- goto / label -------------------------------------------------------------------------------

TEST(ir, goto_jumps_directly_to_its_labels_block)
{
	std::string text = functionIr(
		"int main() {\n"
		"    int x = 0;\n"
		"    goto skip;\n"
		"    x = 1;\n"
		"skip:\n"
		"    return x;\n"
		"}\n");
	// `skip`'s block (L1) is created up front by the function-wide label pre-scan (collectLabelBlocks()
	// - a label can be goto'd to before its own textual declaration, so every label needs a block
	// before lowering the body even starts), before the entry block's own content is lowered - and
	// the unconditional `goto skip;` means `x = 1;` is genuinely unreachable: no edge in this CFG
	// ever reaches L2 (dead code lowered but never jumped into, since this phase does no
	// reachability analysis - see emitVoid()'s own note). Temp ids are handed out in the order
	// IrBuilder actually visits each expression (goto, then the dead `x = 1;`, then `skip:`'s own
	// `return x;`), not in block-print order, which is why L1 uses %4/%5 while L2 (visited earlier
	// in AST order, printed after only because of its later block id) uses %2/%3.
	CHECK_EQ(text,
		"function main(params=0, locals=1) {\n"
		"L0:\n"
		"  %0 = const 0\n"
		"  %1 = &local 0\n"
		"  store.word [%1], %0\n"
		"  jmp L1\n"
		"L1:\n"
		"  %4 = &local 0\n"
		"  %5 = load.word [%4]\n"
		"  ret %5\n"
		"L2:\n"
		"  %2 = &local 0\n"
		"  %3 = const 1\n"
		"  store.word [%2], %3\n"
		"  jmp L1\n"
		"}\n");
}

// ---- && / || short-circuit ------------------------------------------------------------------------

TEST(ir, logical_and_never_evaluates_the_right_hand_side_unless_the_left_is_true)
{
	std::string text = functionIr("int f(int x); int main() { int a; int b; return a && f(b); }");
	// f(b) must sit behind a conditional branch, not be evaluated unconditionally before it.
	usize branchPos = text.find("br.ne");
	usize callPos = text.find("call f");
	CHECK(branchPos != std::string::npos);
	CHECK(callPos != std::string::npos);
	CHECK(branchPos < callPos);
}

TEST(ir, logical_or_never_evaluates_the_right_hand_side_unless_the_left_is_false)
{
	std::string text = functionIr("int f(int x); int main() { int a; int b; return a || f(b); }");
	usize branchPos = text.find("br.ne");
	usize callPos = text.find("call f");
	CHECK(branchPos != std::string::npos);
	CHECK(callPos != std::string::npos);
	CHECK(branchPos < callPos);
}

TEST(ir, logical_and_used_as_a_plain_value_materializes_0_or_1)
{
	std::string text = functionIr("int main() { int a; int b; return a && b; }");
	CHECK(text.find("const 1") != std::string::npos);
	CHECK(text.find("const 0") != std::string::npos);
}

// ---- ternary --------------------------------------------------------------------------------------

TEST(ir, ternary_only_evaluates_the_taken_branch_and_both_sides_copy_into_one_shared_result)
{
	std::string text = functionIr("int f(int x); int g(int x); int main() { int c; return c ? f(1) : g(2); }");
	// Both `call f` and `call g` appearing somewhere, and a `br.ne` appearing somewhere, does not
	// by itself distinguish this from a broken lowering that evaluates *both* operands
	// unconditionally and only then selects one - pin the full shape (both calls genuinely sitting
	// behind the branch, on their own then/else side, and copying into the *same* result temp).
	CHECK_EQ(text,
		"function main(params=0, locals=1) {\n"
		"L0:\n"
		"  %0 = &local 0\n"
		"  %1 = load.word [%0]\n"
		"  %2 = const 0\n"
		"  br.ne %1, %2, L1, L2\n"
		"L1:\n"
		"  %4 = const 1\n"
		"  param %4\n"
		"  %5 = call f, 1\n"
		"  %3 = %5\n"
		"  jmp L3\n"
		"L2:\n"
		"  %6 = const 2\n"
		"  param %6\n"
		"  %7 = call g, 1\n"
		"  %3 = %7\n"
		"  jmp L3\n"
		"L3:\n"
		"  ret %3\n"
		"}\n");
}

// ---- struct member addressing -----------------------------------------------------------------

TEST(ir, struct_member_access_adds_the_fields_byte_offset_to_the_base_address)
{
	std::string text = functionIr(
		"struct P { int x; char y; };\n"
		"int main() { struct P p; p.y = 1; return p.x; }\n");
	// `p.x` (offset 0, folds away - see lowerAddress()'s own note) must read straight off &local
	// with no offset added; `p.y` (a non-zero offset) must add exactly that offset, not some other
	// wrong-but-plausible constant a looser "an add exists somewhere" check would miss.
	CHECK_EQ(text,
		"function main(params=0, locals=1) {\n"
		"L0:\n"
		"  %0 = &local 0\n"
		"  %1 = const 4\n"
		"  %2 = add.u %0, %1\n"
		"  %3 = const 1\n"
		"  store.byte [%2], %3\n"
		"  %4 = &local 0\n"
		"  %5 = load.word [%4]\n"
		"  ret %5\n"
		"}\n");
}

// ---- array-to-pointer decay under unary * -------------------------------------------------------

namespace
{
	support::SourceLocation astLoc() { return support::SourceLocation(testSourceId(), 1, 1, 0); }

	// Arrays have no declarator syntax in the parser yet (see test_sema.cpp's own
	// array_of_self_by_value_... test, which hand-builds its fixture for the same reason) - sema
	// itself already explicitly allows `*arr`/`arr[i]` for an array-typed operand
	// (sema.cpp's own UnaryExpr::Deref/IndexExpr cases), so this is real, reachable behavior once
	// array declarators land, not speculative. Builds `int main() { int arr[4]; <bodyExpr as a
	// statement>; return <returnValue>; }` by hand, pre-annotated exactly as sema would leave it,
	// and returns the printed IR of `main`.
	std::string arrayFixtureIr(ast::Expr* bodyExpr, ast::Expr* returnValue)
	{
		support::Arena arena;
		support::DiagnosticEngine diagnostics;
		support::SourceLocation loc = astLoc();

		const ast::Type* arrayType = ast::Type::makeArray(arena, &ast::Type::Int, 4);
		ast::VarDecl* arrDecl = arena.create<ast::VarDecl>(loc, std::string_view("arr"), arrayType);
		ast::DeclStmt* declStmt = arena.create<ast::DeclStmt>(loc, arrDecl);
		ast::ReturnStmt* ret = arena.create<ast::ReturnStmt>(loc, returnValue);

		std::vector<ast::Stmt*> stmts{ declStmt };
		if (bodyExpr)
			stmts.push_back(arena.create<ast::ExprStmt>(loc, bodyExpr));
		stmts.push_back(ret);
		ast::CompoundStmt* body = arena.create<ast::CompoundStmt>(loc, std::span<ast::Stmt* const>(stmts));

		ast::FunctionDecl* func = arena.create<ast::FunctionDecl>(loc, std::string_view("main"), &ast::Type::Int, std::span<const ast::Param>{}, body);
		std::vector<ast::Decl*> decls{ func };
		ast::TranslationUnit unit(loc, std::span<ast::Decl* const>(decls));

		ir::IrBuilder builder(arena, diagnostics);
		ir::IrModule module = builder.build(unit);

		CHECK_EQ(module.functions().size(), static_cast<usize>(1));
		if (module.functions().empty())
			return "<no-function>";
		return ir::IrPrinter{}.print(*module.functions()[0]);
	}

	ast::NameExpr* arrayNameExpr(support::Arena& arena, const ast::Type* arrayType)
	{
		ast::NameExpr* name = arena.create<ast::NameExpr>(astLoc(), std::string_view("arr"));
		name->setType(arrayType);
		return name;
	}
}

TEST(ir, dereferencing_an_array_reads_through_the_arrays_own_address_not_a_word_loaded_from_it)
{
	// `*arr` must go straight from &local to a single load, with no offset arithmetic and no
	// second load of a bogus "pointer value" in between - an array's own storage IS what `*`
	// dereferences (`*arr == arr[0]`), never a pointer value loaded from somewhere else (an array
	// is never itself "pointed to" by anything stored in its own slot).
	support::Arena arena;
	const ast::Type* arrayType = ast::Type::makeArray(arena, &ast::Type::Int, 4);
	ast::UnaryExpr* deref = arena.create<ast::UnaryExpr>(astLoc(), ast::UnaryOp::Deref, arrayNameExpr(arena, arrayType));
	deref->setType(&ast::Type::Int);

	std::string text = arrayFixtureIr(nullptr, deref);
	CHECK_EQ(text,
		"function main(params=0, locals=1) {\n"
		"L0:\n"
		"  %0 = &local 0\n"
		"  %1 = load.word [%0]\n"
		"  ret %1\n"
		"}\n");
}

TEST(ir, storing_through_a_dereferenced_array_writes_to_the_arrays_own_address)
{
	support::Arena arena;
	const ast::Type* arrayType = ast::Type::makeArray(arena, &ast::Type::Int, 4);
	ast::UnaryExpr* deref = arena.create<ast::UnaryExpr>(astLoc(), ast::UnaryOp::Deref, arrayNameExpr(arena, arrayType));
	deref->setType(&ast::Type::Int);
	ast::IntLiteralExpr* seven = arena.create<ast::IntLiteralExpr>(astLoc(), u64(7));
	seven->setType(&ast::Type::Int);
	ast::AssignExpr* assign = arena.create<ast::AssignExpr>(astLoc(), ast::AssignOp::Assign, deref, seven);
	assign->setType(&ast::Type::Int);
	ast::IntLiteralExpr* zero = arena.create<ast::IntLiteralExpr>(astLoc(), u64(0));
	zero->setType(&ast::Type::Int);

	std::string text = arrayFixtureIr(assign, zero);
	CHECK_EQ(text,
		"function main(params=0, locals=1) {\n"
		"L0:\n"
		"  %0 = &local 0\n"
		"  %1 = const 7\n"
		"  store.word [%0], %1\n"
		"  %2 = const 0\n"
		"  ret %2\n"
		"}\n");
}

// ---- struct-by-value is diagnosed, never silently truncated ------------------------------------

TEST(ir, assigning_one_struct_variable_to_another_is_diagnosed_not_silently_truncated)
{
	// Sema's own isAssignable() allows this (struct P == struct P, see sema.cpp) even though this
	// subset's documented scope is pointer-only struct passing - IrBuilder is the one that must
	// catch it (requireScalarValue()), since a plain word Load/Store would otherwise silently copy
	// only the first 4 bytes of `b` with no diagnostic anywhere in the pipeline.
	CHECK(irBuilderReports(
		"struct P { int a; int b; };\n"
		"int main() { struct P x; struct P y; x = y; return 0; }\n",
		"using a struct by value"));
}

TEST(ir, passing_a_struct_variable_by_value_as_an_argument_is_diagnosed)
{
	CHECK(irBuilderReports(
		"struct P { int a; int b; };\n"
		"void f(struct P p);\n"
		"int main() { struct P x; f(x); return 0; }\n",
		"using a struct by value"));
}

TEST(ir, reading_a_struct_field_that_is_itself_a_struct_is_diagnosed)
{
	CHECK(irBuilderReports(
		"struct Inner { int a; int b; };\n"
		"struct Outer { struct Inner in; };\n"
		"int main() { struct Outer o; struct Inner copy; copy = o.in; return 0; }\n",
		"using a struct by value"));
}

TEST(ir, an_ordinary_scalar_struct_field_read_is_not_diagnosed)
{
	CHECK(!irBuilderReports(
		"struct P { int a; };\n"
		"int main() { struct P p; return p.a; }\n",
		"using a struct by value"));
}

TEST(ir, a_struct_returning_calls_result_used_directly_is_diagnosed)
{
	// A function declared to return `struct P` by value passes sema (isAssignable(P, P) is true,
	// sema.cpp) even though §10/§14's real ABI support for it (a hidden pointer in arg0) is Fase
	// 7's job - visit(CallExpr&) must still catch the result being used as a plain scalar now,
	// same as every other struct-by-value site.
	CHECK(irBuilderReports(
		"struct P { int a; int b; };\n"
		"struct P make();\n"
		"int main() { struct P x; x = make(); return 0; }\n",
		"using a struct by value"));
}

TEST(ir, a_member_access_on_a_struct_returning_call_result_is_diagnosed_not_read_through_a_bogus_address)
{
	// `make()` is not one of this subset's lvalue forms (sema::Sema::isLValue(), sema.cpp), but
	// sema's own visit(MemberExpr&) only requires the `.` base to have struct *type* - not to be an
	// lvalue - so `make().b` type-checks. lowerAddress()'s fallback for a non-addressable base must
	// diagnose this itself rather than silently treating the call's result temp as if it were a
	// real address to add a field offset to and load through.
	CHECK(irBuilderReports(
		"struct P { int a; int b; };\n"
		"struct P make();\n"
		"int main() { return make().b; }\n",
		"using a struct by value"));
}
