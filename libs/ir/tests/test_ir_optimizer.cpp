#include <ceresc/ir/ir_builder.h>
#include <ceresc/ir/ir_optimizer.h>
#include <ceresc/ir/ir_printer.h>
#include <ceresc/parser/parser.h>
#include <ceresc/sema/sema.h>
#include <ceresc/lexer/lexer.h>
#include <ceresc/support/arena.h>
#include <ceresc/support/diagnostics.h>
#include <ceresc/support/optimization.h>
#include <ceresc/support/string_pool.h>

#include "framework.h"

#include <string>
#include <string_view>

// ir_optimizer.h's passes, one at a time. Each test enables exactly the pass it is about on top of
// -O0, so what changes in the printed IR is attributable to that pass alone rather than to whatever
// the rest of the pipeline happened to do afterwards - the whole-pipeline shape is what
// libs/codegen's golden tests and tests/e2e pin instead.
//
// Two passes are deliberately verified by what they do NOT do as well: constant folding leaves a
// division by zero alone (the VM traps rather than producing a value, 05-Instruction-Set.md), and
// dead-code elimination never removes a Load (a device register read can have a side effect, and
// `volatile` is out of v1 - see ir_optimizer.h's header comment on both).

using namespace ceresc;

namespace
{
	support::SourceId testSourceId() { return support::SourceId::make(1); }

	// Lowers `source` to IR, applies `options`, and returns the printed result. Building the IR
	// itself always uses the unoptimized settings so that what a test turns on in `options` is the
	// only thing acting on the IR it inspects.
	std::string optimizedIr(std::string_view source, const support::OptimizationOptions& options,
		std::string_view functionName = "main")
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

		ir::IrBuilder builder(arena, diagnostics, support::OptimizationOptions::none());
		ir::IrModule module = builder.build(*unit);
		ir::optimize(module, arena, options);

		for (const auto& function : module.functions())
			if (function->name() == functionName)
				return ir::IrPrinter{}.print(*function);
		return "<function-not-found>";
	}

	std::string wholeModuleIr(std::string_view source, const support::OptimizationOptions& options)
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

		ir::IrBuilder builder(arena, diagnostics, support::OptimizationOptions::none());
		ir::IrModule module = builder.build(*unit);
		ir::optimize(module, arena, options);
		return ir::IrPrinter{}.print(module);
	}

	support::OptimizationOptions only(bool support::OptimizationOptions::* flag)
	{
		support::OptimizationOptions options = support::OptimizationOptions::none();
		options.*flag = true;
		return options;
	}

	// Folding leaves dead constants behind on purpose (dropping them is dead-code elimination's
	// job), so most folding tests want both passes to see the final shape.
	support::OptimizationOptions foldAndClean()
	{
		support::OptimizationOptions options = support::OptimizationOptions::none();
		options.constantFolding = true;
		options.deadCodeElimination = true;
		return options;
	}

	// Induction-variable strength reduction matches `base + counter*C` only when `base` is already
	// loop invariant and defined before the loop - which is LICM's job. The two are enabled together
	// here, the order the real pipeline runs them in. Dead-code elimination joins them because the
	// pass leaves the old address computation behind for it to remove.
	support::OptimizationOptions loopAndInduction()
	{
		support::OptimizationOptions options = support::OptimizationOptions::none();
		options.loopInvariantMotion = true;
		options.inductionStrengthReduction = true;
		options.deadCodeElimination = true;
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

// ---- constant folding ---------------------------------------------------------------------------

TEST(ir_optimizer, constant_folding_collapses_an_arithmetic_expression)
{
	std::string text = optimizedIr("int main() { return 2 + 3 * 4; }", foldAndClean());
	CHECK_EQ(text,
		"function main(params=0, locals=0) {\n"
		"L0:\n"
		"  %4 = const 14\n"
		"  ret %4\n"
		"}\n");
}

TEST(ir_optimizer, constant_folding_is_off_at_O0)
{
	std::string text = optimizedIr("int main() { return 2 + 3 * 4; }", support::OptimizationOptions::none());
	CHECK(contains(text, "mul"));
	CHECK(contains(text, "add"));
	CHECK(!contains(text, "const 14"));
}

TEST(ir_optimizer, constant_folding_wraps_at_32_bits_like_C_does)
{
	// 2147483647 + 1 is INT_MIN in a 32-bit int, which is exactly what the machine computes - the
	// folded value has to agree with it rather than with a wider host integer.
	std::string text = optimizedIr("int main() { return 2147483647 + 1; }", foldAndClean());
	CHECK(contains(text, "const -2147483648"));
}

TEST(ir_optimizer, constant_folding_respects_shift_masking)
{
	// The machine masks a shift count to five bits (05-Instruction-Set.md), so `1 << 33` is `1 << 1`.
	std::string text = optimizedIr("int main() { return 1 << 33; }", foldAndClean());
	CHECK(contains(text, "const 2"));
}

TEST(ir_optimizer, constant_folding_leaves_a_division_by_zero_alone)
{
	// The VM sets the Trap flag and leaves the destination untouched rather than faulting
	// (05-Instruction-Set.md/§14) - folding it to any value would invent a result.
	std::string text = optimizedIr("int main() { return 8 / (2 - 2); }", foldAndClean());
	CHECK(contains(text, "div"));
}

TEST(ir_optimizer, constant_folding_evaluates_a_comparison)
{
	std::string text = optimizedIr("int main() { return 3 < 4; }", foldAndClean());
	CHECK(contains(text, "const 1"));
	CHECK(!contains(text, "cmp"));
}

TEST(ir_optimizer, constant_folding_handles_float_arithmetic)
{
	std::string text = optimizedIr("float main() { return 1.5 + 2.5; }", foldAndClean());
	CHECK(contains(text, "const 4"));
	CHECK(!contains(text, "add"));
}

TEST(ir_optimizer, constant_folding_leaves_an_out_of_range_float_to_int_conversion_alone)
{
	// The target conversion defines this edge case; a host cast here would be undefined, so it must
	// remain an ftoi instruction rather than being replaced by a host-dependent constant.
	std::string text = optimizedIr("int main() { return (int)2147483648.0; }", foldAndClean());
	CHECK(contains(text, "ftoi"));
}

// ---- algebraic simplification --------------------------------------------------------------------

TEST(ir_optimizer, algebraic_simplification_drops_an_identity_operation)
{
	support::OptimizationOptions options = support::OptimizationOptions::none();
	options.algebraicSimplification = true;
	std::string text = optimizedIr("int main() { int x = 5; return x + 0; }", options);
	CHECK(!contains(text, "add"));
}

TEST(ir_optimizer, algebraic_simplification_turns_a_multiply_by_zero_into_zero)
{
	support::OptimizationOptions options = support::OptimizationOptions::none();
	options.algebraicSimplification = true;
	std::string text = optimizedIr("int main() { int x = 5; return x * 0; }", options);
	CHECK(!contains(text, "mul"));
	CHECK(contains(text, "const 0"));
}

TEST(ir_optimizer, algebraic_simplification_leaves_float_identities_alone)
{
	// `x * 1.0f` is NOT x for a NaN or a signed zero, and this project makes no fast-math promise -
	// see simplifyBinOp()'s own note.
	support::OptimizationOptions options = support::OptimizationOptions::none();
	options.algebraicSimplification = true;
	std::string text = optimizedIr("float main() { float x = 5.0; return x * 1.0; }", options);
	CHECK(contains(text, "mul"));
}

// ---- optimization levels -------------------------------------------------------------------------

TEST(ir_optimizer, the_size_and_debug_levels_turn_off_the_right_O1_passes)
{
	support::OptimizationOptions o1 = support::OptimizationOptions::forLevel(support::OptimizationLevel::O1);
	support::OptimizationOptions os = support::OptimizationOptions::forLevel(support::OptimizationLevel::Os);
	support::OptimizationOptions og = support::OptimizationOptions::forLevel(support::OptimizationLevel::Og);

	// -Os drops the two transforms that grow the image, and nothing else.
	CHECK(o1.jumpTables && !os.jumpTables);
	CHECK(!os.inlining && os.constantFolding && os.registerAllocation);

	// -Og drops the transforms that obscure the code, and nothing else.
	CHECK(o1.localSlotReuse && !og.localSlotReuse);
	CHECK(!og.inlining && og.constantFolding && og.jumpTables);

	// Block layout is an ordinary O1 optimization, so it is off at O0 by construction.
	CHECK(o1.blockLayout);
	CHECK(!support::OptimizationOptions::forLevel(support::OptimizationLevel::O0).blockLayout);

	// The conditional-constant pass is an ordinary O1 optimization too, and -Og keeps it: resolving
	// a branch makes the code easier to follow, not harder.
	CHECK(o1.conditionalConstants && og.conditionalConstants);
	CHECK(!support::OptimizationOptions::forLevel(support::OptimizationLevel::O0).conditionalConstants);

	// Loop-invariant motion is an ordinary O1 optimization as well.
	CHECK(o1.loopInvariantMotion);
	CHECK(!support::OptimizationOptions::forLevel(support::OptimizationLevel::O0).loopInvariantMotion);

	// Induction-variable strength reduction is on at O1 and -Os, but -Og turns it off: a hidden
	// pointer slot is exactly the kind of rewriting that makes the code harder to follow.
	CHECK(o1.inductionStrengthReduction && os.inductionStrengthReduction && !og.inductionStrengthReduction);
	CHECK(!support::OptimizationOptions::forLevel(support::OptimizationLevel::O0).inductionStrengthReduction);
}

// ---- self-comparison folding ---------------------------------------------------------------------

TEST(ir_optimizer, a_comparison_of_a_value_with_itself_folds)
{
	// Once forwarding makes both reads of `x` the same value, `x == x` is 1 and `x < x` is 0.
	support::OptimizationOptions options = support::OptimizationOptions::none();
	options.loadForwarding = true;
	options.copyPropagation = true;
	options.algebraicSimplification = true;

	std::string text = optimizedIr("int f(int a) { int x = a; return (x == x) + (x < x); }", options, "f");
	CHECK(!contains(text, "cmp"));
	CHECK(contains(text, "const 1")); // x == x
	CHECK(contains(text, "const 0")); // x < x
}

TEST(ir_optimizer, a_self_comparison_folds_for_every_predicate)
{
	support::OptimizationOptions options = support::OptimizationOptions::none();
	options.loadForwarding = true;
	options.copyPropagation = true;
	options.algebraicSimplification = true;

	const struct { const char* op; bool truth; } cases[] = {
		{ "==", true }, { "!=", false }, { "<", false }, { "<=", true }, { ">", false }, { ">=", true },
	};
	for (const auto& test : cases)
	{
		std::string source = std::string("int f(int a) { int x = a; return x ") + test.op + " x; }";
		std::string text = optimizedIr(source, options, "f");
		CHECK(!contains(text, "cmp"));
		CHECK(contains(text, test.truth ? "const 1" : "const 0"));
	}
}

TEST(ir_optimizer, a_self_comparison_of_a_float_is_left_alone)
{
	// NaN makes `x == x` false, so the fold must not apply to floats - even when forwarding makes
	// both reads name the same value.
	support::OptimizationOptions options = support::OptimizationOptions::none();
	options.loadForwarding = true;
	options.copyPropagation = true;
	options.algebraicSimplification = true;
	std::string text = optimizedIr("float f(float a) { float x = a; return x == x; }", options, "f");
	CHECK(contains(text, "cmp"));
}

TEST(ir_optimizer, an_if_on_a_self_comparison_takes_its_branch)
{
	std::string text = optimizedIr("int f(int a) { int x = a; if (x == x) { return 1; } return 0; }",
		support::OptimizationOptions::forLevel(support::OptimizationLevel::O1), "f");
	CHECK(contains(text, "const 1"));
	CHECK(!contains(text, "const 0")); // the impossible arm is gone
}

// ---- sparse conditional constant propagation (SCCP) ----------------------------------------------

TEST(ir_optimizer, sccp_resolves_a_constant_switch_to_its_taken_case)
{
	// A dense switch lowers to a jump table, which the ordinary constant folder cannot reach: it
	// folds comparisons, and a table dispatch is not one. The conditional lattice knows the
	// discriminant is 3, so the whole table collapses to a jump into the case-3 body.
	support::OptimizationOptions options = support::OptimizationOptions::none();
	options.conditionalConstants = true;
	options.jumpTables = true;

	std::string text = optimizedIr(
		"int f() { switch (3) { case 1: return 11; case 2: return 22; case 3: return 33; case 4: return 44; case 5: return 55; } return 0; }",
		options, "f");
	CHECK(!contains(text, "tbl.jmp"));
	CHECK(contains(text, "jmp L3")); // the case-3 block
	CHECK(contains(text, "const 33"));
}

TEST(ir_optimizer, sccp_resolves_a_switch_whose_discriminant_it_computes)
{
	// `1 + 2` is not a Const instruction, so collectConstants() would not see it - the lattice folds
	// the add itself and still resolves the table. Value folding is left to the constant folder, so
	// the `add` instruction survives here (dead-code elimination would take it) - what matters is
	// that the dispatch is a direct jump.
	support::OptimizationOptions options = support::OptimizationOptions::none();
	options.conditionalConstants = true;
	options.jumpTables = true;

	std::string text = optimizedIr(
		"int f() { switch (1 + 2) { case 1: return 11; case 2: return 22; case 3: return 33; case 4: return 44; case 5: return 55; } return 0; }",
		options, "f");
	CHECK(contains(text, "add"));
	CHECK(!contains(text, "tbl.jmp"));
	CHECK(contains(text, "jmp L3"));
}

TEST(ir_optimizer, sccp_resolves_a_branch_on_a_constant)
{
	support::OptimizationOptions options = only(&support::OptimizationOptions::conditionalConstants);
	std::string text = optimizedIr("int f() { if (3 < 4) return 1; return 0; }", options, "f");
	CHECK(!contains(text, "br."));
	CHECK(contains(text, "jmp L1"));
	CHECK(contains(text, "const 1"));
}

TEST(ir_optimizer, sccp_plus_unreachable_elimination_drops_the_dead_arm)
{
	support::OptimizationOptions options = support::OptimizationOptions::none();
	options.conditionalConstants = true;
	options.unreachableBlockElimination = true;

	std::string text = optimizedIr("int f() { if (3 < 4) return 1; return 0; }", options, "f");
	CHECK(!contains(text, "br."));
	CHECK(!contains(text, "L2:")); // the `return 0` block the resolved branch left unreachable
	CHECK(contains(text, "const 1"));
}

TEST(ir_optimizer, sccp_leaves_a_division_by_zero_alone)
{
	// The lattice folds `2 - 2` to 0 so it can see the divisor, but a division by zero has no
	// compile-time value (the VM traps rather than producing one), so the div must survive.
	support::OptimizationOptions options = only(&support::OptimizationOptions::conditionalConstants);
	std::string text = optimizedIr("int f() { return 8 / (2 - 2); }", options, "f");
	CHECK(contains(text, "div"));
}

TEST(ir_optimizer, sccp_does_not_resolve_a_branch_on_a_twice_defined_temporary)
{
	// The ternary writes one shared result temporary on both arms (IrBuilder's own shape), so that
	// temporary has two definitions and is not SSA. Were the lattice to keep the first arm's
	// constant, the branch below would resolve to that arm and miscompile; it must stay.
	support::OptimizationOptions options = only(&support::OptimizationOptions::conditionalConstants);
	std::string text = optimizedIr("int f(int a) { if (a ? 1 : 0) return 10; return 20; }", options, "f");
	CHECK(contains(text, "br."));
}

// ---- loop-invariant code motion (LICM) ----------------------------------------------------------

TEST(ir_optimizer, licm_hoists_an_invariant_load_and_computation_into_the_preheader)
{
	// `a * b` and the loads of `a` and `b` are the same on every iteration (`a`/`b` are parameters
	// the loop never writes), so all of it moves into the preheader block L0; only the accumulate
	// stays in the body.
	support::OptimizationOptions options = only(&support::OptimizationOptions::loopInvariantMotion);
	std::string text = optimizedIr(
		"int f(int n, int a, int b) { int total = 0; for (int i = 0; i < n; i = i + 1) { total = total + a * b; } return total; }",
		options, "f");
	usize mul = text.find("mul");
	usize header = text.find("L1:");
	CHECK(mul != std::string::npos && header != std::string::npos);
	CHECK(mul < header); // the multiply is in the preheader, before the loop header
}

TEST(ir_optimizer, licm_does_not_hoist_a_division)
{
	// The VM raises Trap on a zero divisor, so hoisting a division could fault a loop that never
	// reaches it. The divisor's load moves out, but the division itself stays in the body.
	support::OptimizationOptions options = only(&support::OptimizationOptions::loopInvariantMotion);
	std::string text = optimizedIr(
		"int f(int n, int a) { int t = 0; for (int i = 0; i < n; i = i + 1) { t = t + 10 / a; } return t; }",
		options, "f");
	usize div = text.find("div");
	usize header = text.find("L1:");
	CHECK(div != std::string::npos && header != std::string::npos);
	CHECK(div > header); // the division is still in the loop body
}

TEST(ir_optimizer, licm_does_not_hoist_a_load_of_a_local_the_loop_writes)
{
	// `i` changes every iteration, so its load must stay in the loop; only the loop-invariant `n`
	// is read once, in the preheader.
	support::OptimizationOptions options = only(&support::OptimizationOptions::loopInvariantMotion);
	std::string text = optimizedIr(
		"int f(int n) { int t = 0; for (int i = 0; i < n; i = i + 1) { t = t + i; } return t; }",
		options, "f");
	// L1 is the header, where the loop counter is read and compared.
	CHECK(contains(text, "load.word"));
	usize header = text.find("L1:");
	CHECK(text.find("load.word", header) != std::string::npos);
}

TEST(ir_optimizer, licm_hoists_an_invariant_computation_that_uses_a_literal)
{
	// IrBuilder materializes `2` in the loop body, so the multiply's operand is a Const defined in
	// the loop. Const is hoistable (it has no operands), which is what lets the multiply follow.
	support::OptimizationOptions options = only(&support::OptimizationOptions::loopInvariantMotion);
	std::string text = optimizedIr(
		"int f(int n, int a) { int t = 0; for (int i = 0; i < n; i = i + 1) { t = t + a * 2; } return t; }",
		options, "f");
	usize mul = text.find("mul");
	usize header = text.find("L1:");
	CHECK(mul != std::string::npos && header != std::string::npos);
	CHECK(mul < header);
}

TEST(ir_optimizer, licm_does_not_hoist_a_float_to_int_conversion)
{
	// The conversion is undefined when its operand is out of range, so speculating it could turn a
	// conversion the loop never executed into undefined behaviour. Its operand's load moves out, the
	// conversion does not.
	support::OptimizationOptions options = only(&support::OptimizationOptions::loopInvariantMotion);
	std::string text = optimizedIr(
		"int f(int n, float x) { int t = 0; for (int i = 0; i < n; i = i + 1) { t = t + (int)x; } return t; }",
		options, "f");
	usize ftoi = text.find("ftoi");
	usize header = text.find("L1:");
	CHECK(ftoi != std::string::npos && header != std::string::npos);
	CHECK(ftoi > header);
}

TEST(ir_optimizer, licm_leaves_a_loop_that_calls_alone)
{
	// A value hoisted out of a loop stays live for the whole loop, so it would be live across any
	// call the loop makes - and the allocator's own "does this range span a call" test is linear in
	// emission order, which a back edge fools. A loop with a call is therefore left untouched.
	std::string_view source =
		"int g(int); int f(int n, int a) { int t = 0; for (int i = 0; i < n; i = i + 1) { t = t + a + g(i); } return t; }";
	CHECK_EQ(optimizedIr(source, only(&support::OptimizationOptions::loopInvariantMotion), "f"),
		optimizedIr(source, support::OptimizationOptions::none(), "f"));
}

// ---- induction-variable strength reduction (IV-SR) -----------------------------------------------

TEST(ir_optimizer, induction_strength_reduction_turns_a_struct_array_walk_into_a_pointer)
{
	// `arr[i].a` with a twelve-byte element strides by a multiply that is not a power of two. The
	// pass replaces the body's `mul` and its address add with a load of a pointer carried in a
	// fresh local slot (locals grows from 4 to 5) that the latch bumps by 12. With the pass off the
	// multiply is still in the body; with it on there is none left after the header.
	std::string_view source =
		"struct T { int a; int b; int c; }; "
		"int f(struct T* arr, int n) { int s = 0; for (int i = 0; i < n; i = i + 1) { s = s + arr[i].a; } return s; }";

	support::OptimizationOptions without = loopAndInduction();
	without.inductionStrengthReduction = false;

	std::string before = optimizedIr(source, without, "f");
	std::string after = optimizedIr(source, loopAndInduction(), "f");

	usize beforeHeader = before.find("L1:");
	usize afterHeader = after.find("L1:");
	CHECK(beforeHeader != std::string::npos && afterHeader != std::string::npos);
	CHECK(before.find("mul", beforeHeader) != std::string::npos); // the stride multiply is in the body
	CHECK(after.find("mul", afterHeader) == std::string::npos);   // and is gone with the pass on
	CHECK(contains(after, "locals=5"));                           // the pointer's own slot
}

TEST(ir_optimizer, induction_strength_reduction_scales_the_step_by_the_element_size)
{
	// `i = i + 2` over 4-byte elements advances the pointer by 8, not 4: the latch's bump is a
	// `const 8`, which only a step-aware pass produces.
	std::string_view source =
		"int f(int* arr, int n) { int s = 0; for (int i = 0; i < n; i = i + 2) { s = s + arr[i]; } return s; }";
	CHECK(contains(optimizedIr(source, loopAndInduction(), "f"), "const 8"));
}

TEST(ir_optimizer, induction_strength_reduction_handles_a_decreasing_counter)
{
	// `i = i - 1` over 4-byte elements makes the step negative, so the pointer must advance by -4,
	// not +4.
	std::string_view source =
		"int f(int* arr, int from) { int s = 0; for (int i = from; i > 0; i = i - 1) { s = s + arr[i]; } return s; }";
	CHECK(contains(optimizedIr(source, loopAndInduction(), "f"), "const -4"));
}

TEST(ir_optimizer, induction_strength_reduction_walks_a_byte_array_with_no_scale)
{
	// A one-byte element makes the address `base + i` with no multiply (ir_builder's lowerAddress),
	// so the scaled operand IS the counter load and no scale instruction is emitted. The pointer
	// still advances by the step and the function is one local wider.
	std::string_view source =
		"int f(char* s, int n) { int total = 0; for (int i = 0; i < n; i = i + 1) { total = total + s[i]; } return total; }";
	std::string after = optimizedIr(source, loopAndInduction(), "f");
	CHECK(contains(after, "locals=5")); // params(2) + total + i + the pointer
	CHECK(countOf(after, "shl") == 0);
	CHECK(countOf(after, "mul") == 0);
}

TEST(ir_optimizer, induction_strength_reduction_advances_a_global_array_walk)
{
	// A global array's address is loop invariant with no LICM help, so this is the pass on its own:
	// the pointer slot makes the function one local wider and the body has no multiply left.
	std::string_view source =
		"int data[8]; int f(int n) { int s = 0; for (int i = 0; i < n; i = i + 1) { s = s + data[i]; } return s; }";
	std::string after = optimizedIr(source, loopAndInduction(), "f");
	CHECK(contains(after, "locals=4")); // params(1) + s + i + the pointer
	CHECK(countOf(after, "mul") == 0);
}

TEST(ir_optimizer, induction_strength_reduction_leaves_a_loop_that_calls_alone)
{
	// A pointer carried across the loop is live across any call the loop makes, and the allocator's
	// live-range test does not understand back edges, so a loop with a call is left untouched. The
	// base is a global here, so invariance is not what makes the pass decline - the call is.
	std::string_view source =
		"int g(int); int data[8]; int f(int n) { int s = 0; for (int i = 0; i < n; i = i + 1) { s = s + data[i] + g(i); } return s; }";
	support::OptimizationOptions without = loopAndInduction();
	without.inductionStrengthReduction = false;
	CHECK_EQ(optimizedIr(source, loopAndInduction(), "f"), optimizedIr(source, without, "f"));
}

TEST(ir_optimizer, induction_strength_reduction_does_not_mistake_a_shift_for_a_multiply)
{
	// `p[1 << i]` is `1 << counter`, not `counter << 1`: the counter is the shift AMOUNT, so the
	// address is exponential in it and cannot become an advancing pointer. A multiply is
	// commutative, a shift is not - reading the counter from either side of a `<<` was a real
	// miscompile before this was pinned.
	std::string_view source =
		"int f(char* p, int n) { int total = 0; for (int i = 0; i < n; i = i + 1) { total = total + p[1 << i]; } return total; }";
	support::OptimizationOptions without = loopAndInduction();
	without.inductionStrengthReduction = false;
	CHECK_EQ(optimizedIr(source, loopAndInduction(), "f"), optimizedIr(source, without, "f"));
}

TEST(ir_optimizer, induction_strength_reduction_leaves_a_scaled_index_alone)
{
	// `arr[i * 2]` is still a walk, but the scaled operand is a derived value rather than a direct
	// read of the counter, so the canonical shape is absent and nothing changes.
	std::string_view source =
		"int f(int* arr, int n) { int s = 0; for (int i = 0; i < n; i = i + 1) { s = s + arr[i * 2]; } return s; }";
	support::OptimizationOptions without = loopAndInduction();
	without.inductionStrengthReduction = false;
	CHECK_EQ(optimizedIr(source, loopAndInduction(), "f"), optimizedIr(source, without, "f"));
}

// ---- block layout --------------------------------------------------------------------------------

TEST(ir_optimizer, block_layout_emits_a_branch_false_arm_before_its_true_arm)
{
	// `if (a < b) return 1; return 0;` - layout puts the false arm (return 0) right after the test,
	// so its `const 0` is emitted before the true arm's `const 1`.
	support::OptimizationOptions options = only(&support::OptimizationOptions::blockLayout);
	std::string text = optimizedIr("int f(int a, int b) { if (a < b) return 1; return 0; }", options, "f");
	usize falseArm = text.find("const 0");
	usize trueArm = text.find("const 1");
	CHECK(falseArm != std::string::npos && trueArm != std::string::npos);
	CHECK(falseArm < trueArm);
}

TEST(ir_optimizer, block_layout_leaves_an_if_body_where_it_was)
{
	// `if (a) { body }` already falls through the body and drops the merge jump; layout must not
	// sink the body behind the merge, which would add a jump back. So it changes nothing here.
	std::string_view source = "void use(int); int f(int a, int b) { if (a) { use(b); } return 0; }";
	support::OptimizationOptions options = only(&support::OptimizationOptions::blockLayout);
	std::string before = optimizedIr(source, support::OptimizationOptions::none(), "f");
	CHECK_EQ(optimizedIr(source, options, "f"), before);
}

// ---- common subexpression elimination ------------------------------------------------------------

TEST(ir_optimizer, cse_reuses_a_pure_expression_computed_twice_in_one_block)
{
	// Two identical `x * x` computations. Forwarding and copy propagation are on so both reads of
	// `x` name the same value (the operand canonicalization CSE needs); with CSE off the second mul
	// stays, with it on the second mul is the first.
	std::string_view source = "int f(int a) { int x = a; return (x * x) + (x * x); }";

	support::OptimizationOptions withoutCse = support::OptimizationOptions::none();
	withoutCse.loadForwarding = true;
	withoutCse.copyPropagation = true;

	support::OptimizationOptions withCse = withoutCse;
	withCse.commonSubexpressionElimination = true;

	CHECK_EQ(countOf(optimizedIr(source, withoutCse, "f"), "mul"), usize(2));
	CHECK_EQ(countOf(optimizedIr(source, withCse, "f"), "mul"), usize(1));
}

TEST(ir_optimizer, cse_leaves_a_load_alone_because_it_reads_memory)
{
	// Two reads of *p are NOT the same value - a store (or a device register) could sit between
	// them - so loads are never numbered, and CSE changes nothing here.
	support::OptimizationOptions options = only(&support::OptimizationOptions::commonSubexpressionElimination);
	std::string_view source = "int f(int* p) { return *p + *p; }";
	CHECK_EQ(countOf(optimizedIr(source, options, "f"), "load"),
		countOf(optimizedIr(source, support::OptimizationOptions::none(), "f"), "load"));
}

TEST(ir_optimizer, cse_is_off_at_O0)
{
	support::OptimizationOptions withoutCse = support::OptimizationOptions::none();
	withoutCse.loadForwarding = true;
	withoutCse.copyPropagation = true;
	std::string text = optimizedIr("int f(int a) { int x = a; return (x * x) + (x * x); }", withoutCse, "f");
	CHECK_EQ(countOf(text, "mul"), usize(2));
}

// ---- strength reduction --------------------------------------------------------------------------

TEST(ir_optimizer, strength_reduction_turns_a_multiply_by_a_power_of_two_into_a_shift)
{
	support::OptimizationOptions options = only(&support::OptimizationOptions::strengthReduction);
	std::string text = optimizedIr("int f(int x) { return x * 8; }", options, "f");
	CHECK(contains(text, "shl"));
	CHECK(!contains(text, "mul"));
}

TEST(ir_optimizer, strength_reduction_turns_unsigned_division_into_a_logical_shift)
{
	support::OptimizationOptions options = only(&support::OptimizationOptions::strengthReduction);
	std::string text = optimizedIr("unsigned f(unsigned x) { return x / 4; }", options, "f");
	CHECK(contains(text, "shr"));
	CHECK(!contains(text, "div"));
}

TEST(ir_optimizer, strength_reduction_turns_unsigned_remainder_into_a_mask)
{
	support::OptimizationOptions options = only(&support::OptimizationOptions::strengthReduction);
	std::string text = optimizedIr("unsigned f(unsigned x) { return x % 8; }", options, "f");
	CHECK(contains(text, "and"));
	CHECK(!contains(text, "mod"));
}

TEST(ir_optimizer, strength_reduction_biases_signed_division_before_the_arithmetic_shift)
{
	// C truncates toward zero; a bare arithmetic shift rounds toward -inf, so the dividend is
	// biased up by 2^k-1 when it is negative first (see rewriteStrength's own note).
	support::OptimizationOptions options = only(&support::OptimizationOptions::strengthReduction);
	std::string text = optimizedIr("int f(int x) { return x / 4; }", options, "f");
	CHECK(contains(text, "shr"));  // the bias read
	CHECK(contains(text, "add"));  // x + bias
	CHECK(contains(text, "sar"));  // the arithmetic shift
	CHECK(!contains(text, "div"));
}

TEST(ir_optimizer, strength_reduction_signed_remainder_keeps_the_sign_of_the_dividend)
{
	support::OptimizationOptions options = only(&support::OptimizationOptions::strengthReduction);
	std::string text = optimizedIr("int f(int x) { return x % 4; }", options, "f");
	CHECK(contains(text, "sub")); // x - ((x / 4) << 2)
	CHECK(!contains(text, "mod"));
}

TEST(ir_optimizer, strength_reduction_leaves_a_non_power_of_two_division_alone)
{
	support::OptimizationOptions options = only(&support::OptimizationOptions::strengthReduction);
	std::string text = optimizedIr("unsigned f(unsigned x) { return x / 10; }", options, "f");
	CHECK(contains(text, "div"));
}

TEST(ir_optimizer, strength_reduction_is_off_at_O0)
{
	std::string text = optimizedIr("unsigned f(unsigned x) { return x / 4; }", support::OptimizationOptions::none(), "f");
	CHECK(contains(text, "div"));
	CHECK(!contains(text, "shr"));
}

// ---- branch simplification / unreachable blocks ---------------------------------------------------

TEST(ir_optimizer, a_constant_condition_resolves_to_one_branch_and_the_other_arm_disappears)
{
	support::OptimizationOptions options = support::OptimizationOptions::none();
	options.constantFolding = true;
	options.branchSimplification = true;
	options.unreachableBlockElimination = true;
	options.deadCodeElimination = true;

	std::string text = optimizedIr("int main() { if (1 < 2) { return 7; } return 9; }", options);
	CHECK(contains(text, "const 7"));
	CHECK(!contains(text, "const 9")); // the arm that can never run is gone entirely
	CHECK(!contains(text, "br."));
}

TEST(ir_optimizer, unreachable_code_after_a_return_is_removed)
{
	support::OptimizationOptions options = support::OptimizationOptions::none();
	options.unreachableBlockElimination = true;
	options.deadCodeElimination = true;

	std::string text = optimizedIr("int main() { return 1; return 2; }", options);
	CHECK(contains(text, "const 1"));
	CHECK(!contains(text, "const 2"));
}

// ---- jump threading ------------------------------------------------------------------------------

TEST(ir_optimizer, jump_threading_routes_around_a_block_that_only_jumps)
{
	// A `while` whose body is empty leaves exactly this shape: the body block jumps straight back to
	// the condition, so the branch into it can go there directly instead.
	support::OptimizationOptions options = support::OptimizationOptions::none();
	options.jumpThreading = true;
	options.unreachableBlockElimination = true;

	std::string before = optimizedIr("int main() { int i = 0; while (i < 0) { } return i; }",
		support::OptimizationOptions::none());
	std::string after = optimizedIr("int main() { int i = 0; while (i < 0) { } return i; }", options);
	CHECK(countOf(after, "jmp ") < countOf(before, "jmp "));
}

// ---- dead code elimination ------------------------------------------------------------------------

TEST(ir_optimizer, dead_code_elimination_drops_an_unread_computation)
{
	// A discarded expression statement, rather than an unread variable: storing into a local is a
	// side effect as far as this pass is concerned, so removing THAT is dead-store elimination's job
	// (tested separately below), not this one's.
	std::string text = optimizedIr("int main() { int x = 1; 40 + 2; return x; }", foldAndClean());
	CHECK(!contains(text, "const 42"));
}

TEST(ir_optimizer, dead_code_elimination_never_drops_a_volatile_load)
{
	// Reading a device register can have a side effect, so a load the program marked observable
	// stays even with nothing reading its result. See ir_optimizer.h's header comment.
	support::OptimizationOptions options = only(&support::OptimizationOptions::deadCodeElimination);
	std::string text = optimizedIr(
		"int main() { volatile int* p = (volatile int*)0xFF000004; *p; return 0; }", options);
	CHECK(contains(text, "load.word.v"));
}

TEST(ir_optimizer, dead_code_elimination_drops_an_unread_ordinary_load)
{
	// The contrast, and the payoff of recording `volatile` on the access itself: EVERY load used to
	// stay, for want of anything that could tell a device register from ordinary memory. An
	// unmarked load reads ordinary memory and goes, like any other unread computation.
	support::OptimizationOptions options = only(&support::OptimizationOptions::deadCodeElimination);
	std::string text = optimizedIr("int main() { int* p = (int*)0xFF000004; *p; return 0; }", options);
	CHECK(!contains(text, "load"));
}

TEST(ir_optimizer, dead_code_elimination_keeps_stores_and_calls)
{
	support::OptimizationOptions options = only(&support::OptimizationOptions::deadCodeElimination);
	std::string text = optimizedIr("void side(); int main() { int x = 3; side(); return x; }", options);
	CHECK(contains(text, "call"));
	CHECK(contains(text, "store"));
}

// ---- copy propagation / load forwarding / dead stores ----------------------------------------------

TEST(ir_optimizer, load_forwarding_reuses_a_just_stored_value)
{
	support::OptimizationOptions options = support::OptimizationOptions::none();
	options.loadForwarding = true;
	options.copyPropagation = true;
	options.deadCodeElimination = true;

	std::string before = optimizedIr("int main() { int x = 7; return x; }", support::OptimizationOptions::none());
	std::string after = optimizedIr("int main() { int x = 7; return x; }", options);
	CHECK(contains(before, "load"));
	CHECK(!contains(after, "load")); // the value is already in hand - no need to read it back
}

TEST(ir_optimizer, load_forwarding_stops_at_a_local_whose_address_escaped)
{
	// `p` carries x's address away, so a store through it could be the thing that wrote x - the
	// value stored directly into x is no longer known to be what a later load reads.
	support::OptimizationOptions options = support::OptimizationOptions::none();
	options.loadForwarding = true;
	options.copyPropagation = true;

	std::string text = optimizedIr("void use(int* q); int main() { int x = 1; use(&x); return x; }", options);
	CHECK(contains(text, "load")); // x is still read from memory after the call
}

TEST(ir_optimizer, load_forwarding_forgets_what_a_narrower_store_overwrote)
{
	// A regression, and the sharpest edge in this pass. `*(char*)&x = 1` writes ONE byte of an int
	// local, and the escape analysis still calls that local non-escaping - correctly, since the
	// address is only ever a store operand. But the int stored earlier is no longer what a later
	// full-width load reads, so the tracked value has to be discarded rather than kept. Forwarding
	// it produced a program that printed 5 at -O2 and 1 at -O0.
	std::string text = optimizedIr(
		"int main() { int x = 5; char* c = (char*)&x; *c = 1; return x; }",
		support::OptimizationOptions::forLevel(support::OptimizationLevel::O2));

	// x is read back from memory rather than assumed. The `const 5` itself stays - the byte store
	// overwrote only one of its four bytes, so the other three still come from it; what matters is
	// that the load survives to combine them, instead of the return value being that constant.
	CHECK(contains(text, "load.word"));
	CHECK(contains(text, "store.byte"));
	CHECK(!contains(text, "ret %0")); // %0 is the const - returning it directly is the bug
}

TEST(ir_optimizer, load_forwarding_still_works_for_a_local_of_a_matching_width)
{
	// The other side of that fix: forgetting only happens on a MISMATCH, so the ordinary case is
	// unaffected and a same-width store still feeds the load after it - including the SECOND store,
	// which replaces what the first one made known rather than being ignored like the narrow one.
	support::OptimizationOptions options = support::OptimizationOptions::none();
	options.loadForwarding = true;
	options.copyPropagation = true;
	options.deadCodeElimination = true;

	std::string text = optimizedIr("int main() { int x = 5; x = 7; return x; }", options);
	CHECK(!contains(text, "load"));
	CHECK(contains(text, "ret %2")); // %2 is the 7 - the second store replaced what the first made known
}

TEST(ir_optimizer, restrict_lets_a_load_forward_past_a_store_through_another_restrict_pointer)
{
	// *q cannot alias *p because both are restrict, so the reload of *p forwards to the constant
	// straight through the intervening *q store.
	support::OptimizationOptions options = support::OptimizationOptions::none();
	options.loadForwarding = true;
	options.copyPropagation = true;
	options.deadCodeElimination = true;

	std::string text = optimizedIr("int f(int* restrict p, int* restrict q) { *p = 10; *q = 20; return *p; }", options, "f");
	CHECK(contains(text, "ret %0")); // %0 is the 10, forwarded across the *q store
}

TEST(ir_optimizer, restrict_is_conservative_across_a_store_through_an_ordinary_pointer)
{
	// `q` is not restrict, so a store through it may alias *p after all - the reload has to stay.
	support::OptimizationOptions options = support::OptimizationOptions::none();
	options.loadForwarding = true;
	options.copyPropagation = true;
	options.deadCodeElimination = true;

	std::string text = optimizedIr("int f(int* restrict p, int* q) { *p = 10; *q = 20; return *p; }", options, "f");
	CHECK(contains(text, "ret %8")); // %8 is the reload of *p, kept because *q may have clobbered it
}

TEST(ir_optimizer, restrict_forgets_everything_across_a_call)
{
	// A callee may write through any pointer it can reach, so a call clears whatever a restrict
	// pointer was known to hold - the reload after the call has to stay.
	support::OptimizationOptions options = support::OptimizationOptions::none();
	options.loadForwarding = true;
	options.copyPropagation = true;
	options.deadCodeElimination = true;

	std::string text = optimizedIr("void g(void); int f(int* restrict p) { *p = 10; g(); return *p; }", options, "f");
	CHECK(contains(text, "load.word"));
}

TEST(ir_optimizer, dead_store_elimination_drops_a_store_nothing_ever_reads)
{
	support::OptimizationOptions options = support::OptimizationOptions::none();
	options.loadForwarding = true;
	options.copyPropagation = true;
	options.deadStoreElimination = true;
	options.deadCodeElimination = true;

	std::string text = optimizedIr("int main() { int x = 7; return x; }", options);
	CHECK(!contains(text, "store"));
}

TEST(ir_optimizer, dead_store_elimination_drops_a_store_a_later_store_overwrites_unread)
{
	support::OptimizationOptions options = only(&support::OptimizationOptions::deadStoreElimination);
	std::string text = optimizedIr("int f(int a, int b) { int x = a; x = b; return x; }", options, "f");
	CHECK_EQ(countOf(text, "store"), usize(1)); // the `x = a` is never observed
}

TEST(ir_optimizer, dead_store_elimination_keeps_a_store_read_before_it_is_overwritten)
{
	support::OptimizationOptions options = only(&support::OptimizationOptions::deadStoreElimination);
	std::string text = optimizedIr("int f(int a, int b) { int x = a; int y = x; x = b; return x + y; }", options, "f");
	CHECK_EQ(countOf(text, "store"), usize(3)); // `x = a` is read into y, so none is dead
}

TEST(ir_optimizer, dead_store_elimination_keeps_a_store_to_an_escaping_local)
{
	support::OptimizationOptions options = support::OptimizationOptions::none();
	options.deadStoreElimination = true;

	std::string text = optimizedIr("void use(int* q); int main() { int x = 1; use(&x); return 0; }", options);
	CHECK(contains(text, "store")); // the callee can read it, so the store has to happen
}

TEST(ir_optimizer, copy_propagation_reads_through_a_copy)
{
	support::OptimizationOptions options = support::OptimizationOptions::none();
	options.loadForwarding = true;
	options.copyPropagation = true;
	options.deadCodeElimination = true;

	// After forwarding turns the load into a copy, propagation makes the copy itself unread, and
	// dead-code removal takes it away.
	std::string text = optimizedIr("int main() { int x = 7; return x; }", options);
	CHECK(!contains(text, " = %")); // no bare copy instruction left
}

// ---- inlining ---------------------------------------------------------------------------------------

TEST(ir_optimizer, inlining_splices_a_small_function_into_its_caller)
{
	support::OptimizationOptions options = support::OptimizationOptions::none();
	options.inlining = true;

	std::string text = optimizedIr("int add(int a, int b) { return a + b; } int main() { return add(3, 4); }", options);
	CHECK(!contains(text, "call"));
	CHECK(contains(text, "add"));
}

TEST(ir_optimizer, inlining_plus_the_rest_of_the_pipeline_folds_the_call_away_entirely)
{
	std::string text = optimizedIr("int add(int a, int b) { return a + b; } int main() { return add(3, 4); }",
		support::OptimizationOptions::forLevel(support::OptimizationLevel::O2));
	CHECK(contains(text, "const 7"));
	CHECK(!contains(text, "call"));
	CHECK(!contains(text, "add"));
}

TEST(ir_optimizer, inlining_leaves_a_recursive_function_alone)
{
	// A candidate may not contain a Call at all, which rules out recursion by construction - see
	// isInlinable().
	support::OptimizationOptions options = support::OptimizationOptions::none();
	options.inlining = true;

	std::string text = optimizedIr("int f(int n) { if (n <= 1) return 1; return f(n - 1); } int main() { return f(3); }",
		options, "f");
	CHECK(contains(text, "call"));
}

TEST(ir_optimizer, inlining_splices_a_function_that_calls_another_one)
{
	// A callee may itself call something: the nested call is copied along with the rest of the body.
	// The outer call is gone; the inner one is only gone too because the whole chain folds away.
	support::OptimizationOptions options = support::OptimizationOptions::none();
	options.inlining = true;

	std::string text = optimizedIr(
		"int add(int a, int b) { return a + b; } int twice(int x) { return add(x, x); } int main() { return twice(3); }",
		options);
	CHECK(!contains(text, "call twice"));
	CHECK(!contains(text, "call add"));
}

TEST(ir_optimizer, inlining_splices_a_function_with_control_flow_into_its_caller)
{
	support::OptimizationOptions options = support::OptimizationOptions::none();
	options.inlining = true;

	// A multi-block callee whose result is returned unchanged: the caller's own call is replaced by a
	// copy of the callee's blocks, so no `call pick` survives.
	std::string text = optimizedIr(
		"int pick(int n) { if (n) return 1; return 2; } int main(int argc) { return pick(argc); }", options);
	CHECK(!contains(text, "call pick"));
	CHECK(contains(text, "br.")); // the copied branch is there
}

TEST(ir_optimizer, inlining_leaves_a_function_with_inline_assembly_alone)
{
	// The asm text may define a label; copying it would define that label twice. `xs` must keep its
	// own body, and main must still call it.
	support::OptimizationOptions options = support::OptimizationOptions::none();
	options.inlining = true;

	std::string mainIr = optimizedIr(
		"void xs(void) { __asm__(\".spot:\\n\\tnop\"); } int main(void) { xs(); return 0; }", options, "main");
	CHECK(contains(mainIr, "call xs"));
}

TEST(ir_optimizer, inlining_declines_a_callee_that_falls_off_the_end_for_a_used_result)
{
	// `helper` has a path with a bare `ret` (no value) because its body can fall off the end, and
	// main reads the result: the call cannot be replaced by a copy of an invalid value.
	support::OptimizationOptions options = support::OptimizationOptions::none();
	options.inlining = true;

	std::string text = optimizedIr(
		"int helper(int c) { if (c) return 1; } int main(int argc) { return helper(argc); }", options);
	CHECK(contains(text, "call helper"));
}

TEST(ir_optimizer, inlining_is_off_at_O1)
{
	std::string text = optimizedIr("int add(int a, int b) { return a + b; } int main() { return add(3, 4); }",
		support::OptimizationOptions::forLevel(support::OptimizationLevel::O1));
	CHECK(contains(text, "call"));
}

// ---- attribute-driven behaviour: noinline, always_inline, pure/const -----------------------------

namespace
{
	// A single-block, call-free function whose body is far past the inliner's default size limit.
	std::string bigFunction(std::string_view attribute)
	{
		std::string body = "int big(int x) " + std::string(attribute) + " {";
		for (int i = 0; i < 20; i++)
			body += " x = x + 1;";
		body += " return x; }";
		return body + " int main() { return big(0); }";
	}
}

TEST(ir_optimizer, noinline_keeps_a_small_function_out_of_its_caller)
{
	support::OptimizationOptions options = support::OptimizationOptions::none();
	options.inlining = true;
	options.deadCodeElimination = true;
	std::string text = optimizedIr(
		"int __attribute__((noinline)) add(int a, int b) { return a + b; } int main() { return add(3, 4); }",
		options);
	CHECK(contains(text, "call")); // isInlinable() refuses it, however small it is
}

TEST(ir_optimizer, always_inline_splices_a_function_past_the_size_limit)
{
	support::OptimizationOptions options = support::OptimizationOptions::none();
	options.inlining = true;
	options.deadCodeElimination = true;

	CHECK(contains(optimizedIr(bigFunction(""), options), "call"));                    // over the limit: left alone
	CHECK(!contains(optimizedIr(bigFunction("__attribute__((always_inline))"), options), "call")); // forced
}

TEST(ir_optimizer, a_pure_call_whose_result_is_ignored_is_dropped)
{
	support::OptimizationOptions options = only(&support::OptimizationOptions::deadCodeElimination);
	std::string text = optimizedIr("int pure_fn(int) __attribute__((pure)); int main() { pure_fn(1); return 0; }", options);
	CHECK(!contains(text, "call"));
}

TEST(ir_optimizer, a_const_call_whose_result_is_ignored_is_dropped)
{
	support::OptimizationOptions options = only(&support::OptimizationOptions::deadCodeElimination);
	std::string text = optimizedIr("int const_fn(int) __attribute__((const)); int main() { const_fn(1); return 0; }", options);
	CHECK(!contains(text, "call"));
}

TEST(ir_optimizer, an_ordinary_call_whose_result_is_ignored_is_kept)
{
	// The contrast: without the attribute a discarded result proves nothing about side effects, so
	// the call stays - the same distinction tests/sema pins from the warning side.
	support::OptimizationOptions options = only(&support::OptimizationOptions::deadCodeElimination);
	std::string text = optimizedIr("int ordinary(int); int main() { ordinary(1); return 0; }", options);
	CHECK(contains(text, "call"));
}

// ---- unused function elimination ---------------------------------------------------------------------

TEST(ir_optimizer, a_function_nothing_calls_is_dropped)
{
	support::OptimizationOptions options = support::OptimizationOptions::none();
	options.unusedFunctionElimination = true;

	// `static` is load-bearing here, not decoration: a function with external linkage may be
	// called by another object at link time, so nothing in THIS unit calling it proves nothing.
	// Only an internal-linkage function can be dropped for being unreachable.
	std::string text = wholeModuleIr("static int unusedOne(int a) { return a; } int main() { return 1; }", options);
	CHECK(!contains(text, "function unusedOne"));
	CHECK(contains(text, "function main"));

	// The same function without `static` stays, for exactly that reason.
	std::string exported = wholeModuleIr("int unusedOne(int a) { return a; } int main() { return 1; }", options);
	CHECK(contains(exported, "function unusedOne"));
}

TEST(ir_optimizer, a_function_reached_only_indirectly_is_kept)
{
	support::OptimizationOptions options = support::OptimizationOptions::none();
	options.unusedFunctionElimination = true;

	std::string text = wholeModuleIr(
		"int deep(int a) { return a + 1; }"
		"int middle(int a) { return deep(a); }"
		"int main() { return middle(1); }", options);
	CHECK(contains(text, "function deep"));
	CHECK(contains(text, "function middle"));
}

TEST(ir_optimizer, nothing_is_dropped_from_a_fragment_with_no_entry_point)
{
	// Without a `main` there is no single root to measure reachability from, so every function is
	// potentially the one being compiled for - see removeUnusedFunctions().
	support::OptimizationOptions options = support::OptimizationOptions::none();
	options.unusedFunctionElimination = true;

	std::string text = wholeModuleIr("int helper(int a) { return a; }", options);
	CHECK(contains(text, "function helper"));
}

// ---- local slot reuse (IrBuilder's own, not a pass) ---------------------------------------------------

namespace
{
	// Unlike everything above, slot reuse happens while the IR is being BUILT (scopes only exist
	// then), so this helper passes the options to IrBuilder instead of to optimize().
	u32 localCountFor(std::string_view source, const support::OptimizationOptions& options)
	{
		support::Arena arena;
		support::DiagnosticEngine diagnostics;
		support::StringPool pool;
		lexer::Lexer lexer(source, testSourceId(), diagnostics, pool);
		parser::Parser parser(lexer, arena, diagnostics);

		ast::TranslationUnit* unit = parser.parseTranslationUnit();
		CHECK(unit != nullptr);
		if (!unit)
			return 0;
		sema::Sema sema(arena, diagnostics);
		CHECK(sema.check(*unit));

		ir::IrBuilder builder(arena, diagnostics, options);
		ir::IrModule module = builder.build(*unit);
		for (const auto& function : module.functions())
			if (function->name() == "main")
				return function->localCount();
		return 0;
	}
}

TEST(ir_optimizer, locals_in_disjoint_scopes_share_one_frame_slot)
{
	std::string_view source = "int main() { { int a = 1; } { int b = 2; } { int c = 3; } return 0; }";
	support::OptimizationOptions reuse = only(&support::OptimizationOptions::localSlotReuse);

	CHECK_EQ(localCountFor(source, support::OptimizationOptions::none()), u32(3));
	CHECK_EQ(localCountFor(source, reuse), u32(1)); // one slot, claimed again by each sibling scope
}

TEST(ir_optimizer, overlapping_locals_never_share_a_slot)
{
	// Nested, not sibling: `inner` exists while `outer` still does, so they cannot share.
	std::string_view source = "int main() { int outer = 1; { int inner = 2; return outer + inner; } }";
	support::OptimizationOptions reuse = only(&support::OptimizationOptions::localSlotReuse);
	CHECK_EQ(localCountFor(source, reuse), u32(2));
}

TEST(ir_optimizer, a_reused_slot_grows_to_the_widest_local_that_lived_in_it)
{
	// A `char` scope followed by an `int` one shares a slot that must end up int-sized, or the
	// second local would be storing four bytes into a one-byte field.
	std::string_view source = "int main() { { char a = 1; } { int b = 2; } return 0; }";
	support::OptimizationOptions reuse = only(&support::OptimizationOptions::localSlotReuse);

	support::Arena arena;
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	lexer::Lexer lexer(source, testSourceId(), diagnostics, pool);
	parser::Parser parser(lexer, arena, diagnostics);
	ast::TranslationUnit* unit = parser.parseTranslationUnit();
	CHECK(unit != nullptr);
	sema::Sema sema(arena, diagnostics);
	CHECK(sema.check(*unit));
	ir::IrBuilder builder(arena, diagnostics, reuse);
	ir::IrModule module = builder.build(*unit);

	for (const auto& function : module.functions())
	{
		if (function->name() != "main")
			continue;
		CHECK_EQ(function->localCount(), u32(1));
		CHECK_EQ(function->localSlots()[0].sizeInBytes, u32(4));
	}
}

TEST(ir_optimizer, inlining_leaves_a_variadic_function_alone)
{
	// Its arguments are not described by its parameter list, and its body reads them out of the
	// caller's frame - neither of which survives being spliced into a different frame.
	support::OptimizationOptions options = support::OptimizationOptions::none();
	options.inlining = true;

	std::string text = optimizedIr("int one(int a, ...) { return a; } int main() { return one(5, 9); }", options);
	CHECK(contains(text, "call"));
}

namespace
{
	usize countOccurrences(std::string_view haystack, std::string_view needle)
	{
		usize count = 0;
		for (usize at = haystack.find(needle); at != std::string_view::npos; at = haystack.find(needle, at + needle.size()))
			++count;
		return count;
	}
}

TEST(ir_optimizer, a_volatile_access_is_never_forwarded_or_dropped)
{
	// Both writes and both reads have to survive: each one is observable, so neither dead-store
	// elimination nor load forwarding may touch them - even though the object is a plain local
	// whose address never escapes, which is exactly the shape both passes normally act on.
	std::string text = optimizedIr(
		"int main() { volatile int x; x = 1; x = 2; return x + x; }",
		support::OptimizationOptions::forLevel(support::OptimizationLevel::O2));
	CHECK_EQ(countOccurrences(text, "store."), usize(2));
	CHECK_EQ(countOccurrences(text, "load."), usize(2));
}

TEST(ir_optimizer, the_same_shape_without_volatile_is_optimized)
{
	// The contrast that makes the test above about `volatile` rather than about this pass being
	// switched off: drop the qualifier and the first store and both loads go away.
	std::string text = optimizedIr(
		"int main() { int x; x = 1; x = 2; return x + x; }",
		support::OptimizationOptions::forLevel(support::OptimizationLevel::O2));
	CHECK(countOccurrences(text, "store.") < usize(2));
	CHECK(countOccurrences(text, "load.") < usize(2));
}
