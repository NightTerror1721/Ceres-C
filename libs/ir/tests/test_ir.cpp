#include <ceresc/ir/ir_builder.h>
#include <ceresc/ir/ir_printer.h>
#include <ceresc/parser/parser.h>
#include <ceresc/sema/sema.h>
#include <ceresc/lexer/lexer.h>
#include <ceresc/ast/decl.h>
#include <ceresc/support/arena.h>
#include <ceresc/support/diagnostics.h>
#include <ceresc/support/string_pool.h>

#include <algorithm>
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
	std::string functionIr(std::string_view source, std::string_view functionName = "main",
		const support::OptimizationOptions& options = support::OptimizationOptions::none())
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

		ir::IrBuilder builder(arena, diagnostics, options);
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

		ir::IrBuilder builder(arena, diagnostics, support::OptimizationOptions::none());
		ir::IrModule module = builder.build(*unit);
		return ir::IrPrinter{}.print(module);
	}

	// Runs the same pipeline and returns every diagnostic message it reported, for the F3.1a guard:
	// a 64-bit value the IR cannot lower is reported (E5002) rather than truncated to 32 bits.
	std::vector<std::string> loweringDiagnostics(std::string_view source)
	{
		support::Arena arena;
		support::DiagnosticEngine diagnostics;
		support::StringPool pool;
		lexer::Lexer lexer(source, testSourceId(), diagnostics, pool);
		parser::Parser parser(lexer, arena, diagnostics);

		ast::TranslationUnit* unit = parser.parseTranslationUnit();
		if (unit && !diagnostics.hasErrors())
		{
			sema::Sema sema(arena, diagnostics);
			if (sema.check(*unit))
			{
				ir::IrBuilder builder(arena, diagnostics, support::OptimizationOptions::none());
				builder.build(*unit);
			}
		}

		std::vector<std::string> messages;
		for (const support::Diagnostic& diagnostic : diagnostics.diagnostics())
			messages.push_back(diagnostic.message);
		return messages;
	}

	bool containsMessage(const std::vector<std::string>& messages, std::string_view needle)
	{
		for (const std::string& message : messages)
		{
			if (message.find(needle) != std::string::npos)
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

TEST(ir, a_64_bit_value_lowers_as_a_two_word_pair)
{
	// F3.1b: a wide value is the address of its two words, so a local, an assignment, a comparison
	// and the add/sub/bitwise operators all lower with no diagnostic. Values are checked end to end
	// by examples/35_int64.c; this pins the shape.
	CHECK(!containsMessage(loweringDiagnostics("int main() { long long x = 5; return (int)x; }"), "not supported in generated code"));
	CHECK(!containsMessage(loweringDiagnostics("int main() { long long a = 1, b = 2; return (int)(a + b); }"), "not supported in generated code"));
	CHECK(!containsMessage(loweringDiagnostics("int main() { long long a = 1, b = 2; return a < b; }"), "not supported in generated code"));
	CHECK(!containsMessage(loweringDiagnostics("int main() { long long a = 1; a += 2; return (int)a; }"), "not supported in generated code"));
	CHECK(!containsMessage(loweringDiagnostics("int main() { long long a = 1, b = 2; return (int)(a - b + (a & b) + (a | b) + (a ^ b)); }"), "not supported in generated code"));
	CHECK(!containsMessage(loweringDiagnostics("int main() { long long a = 1; bool b = a; return b; }"), "not supported in generated code"));
	CHECK(!containsMessage(loweringDiagnostics("int main() { long long a = 1; long long b = a++; return (int)b; }"), "not supported in generated code"));

	// A wide local's initializer writes both halves, and the high word is the sign extension.
	std::string text = functionIr("int main() { long long x = 5; return (int)x; }");
	CHECK(text.find("store.word") != std::string::npos);
	CHECK(text.find("sar") != std::string::npos);
}

TEST(ir, the_64_bit_operations_this_phase_lacks_are_refused)
{
	// A wide discriminant (F9) and a wide builtin operand (there is no 64-bit machine builtin) are
	// still refused; F3.4 lowered the wide parameter and return conventions these once stood in for.
	CHECK(containsMessage(loweringDiagnostics("int main() { long long a = 1; switch (a) { case 1: return 0; } return 1; }"), "not supported in generated code"));
	CHECK(containsMessage(loweringDiagnostics("int main() { long long a = 1; return __builtin_clz(a); }"), "not supported in generated code"));
}

TEST(ir, the_64_bit_shifts_and_conversions_lower)
{
	// F3.3: shifts and the float conversions of a 64-bit value all lower now.
	CHECK(!containsMessage(loweringDiagnostics("int main() { long long a = 1; return (int)(a << 1); }"), "not supported in generated code"));
	CHECK(!containsMessage(loweringDiagnostics("int main() { long long a = 4; return (int)(a >> 1); }"), "not supported in generated code"));
	CHECK(!containsMessage(loweringDiagnostics("int main() { long long a = 1; double d = (double)a; return (int)d; }"), "not supported in generated code"));
	CHECK(!containsMessage(loweringDiagnostics("int main() { long long a = (long long)1.5; return (int)a; }"), "not supported in generated code"));
	CHECK(!containsMessage(loweringDiagnostics("int main() { long long a = 1; float f = a + 1.5f; return (int)f; }"), "not supported in generated code"));
	CHECK(!containsMessage(loweringDiagnostics("int main() { long long a = 1; return a < 1.5f; }"), "not supported in generated code"));
	CHECK(!containsMessage(loweringDiagnostics("int main() { long long a = 1; bool b = a; return b; }"), "not supported in generated code"));

	// A shift by 32 branches on the count, so the fully-optimized pipeline sees real control flow.
	CHECK(functionIr("int main() { long long a = 1; return (int)(a << 40); }").find("shl") != std::string::npos);

	// The three arms of a 64-bit shift: 0, 1..31 and 32..63 are all lowered (a small right shift of a
	// negative value picks the arithmetic form; the same shift unsigned picks the logical one).
	CHECK(!containsMessage(loweringDiagnostics("int main() { long long a = 1; return (int)((a << 0) + (a << 1) + (a << 32)); }"), "not supported in generated code"));
	CHECK(!containsMessage(loweringDiagnostics("int main() { long long a = -1; return (int)((a >> 1) + (a >> 32) + ((unsigned long long)a >> 1)); }"), "not supported in generated code"));
}

TEST(ir, the_64_bit_mul_div_mod_lower_to_the_symbol_and_the_emitted_routine)
{
	// F3.2: a 64-bit multiply composes from the 32-bit mul and the unsigned multiply-high; a divide
	// or remainder calls the compiler's own `__cc_div64`.
	CHECK(!containsMessage(loweringDiagnostics("int main() { long long a = 3, b = 4; return (int)(a * b); }"), "not supported in generated code"));
	CHECK(!containsMessage(loweringDiagnostics("int main() { long long a = 12, b = 4; return (int)(a / b); }"), "not supported in generated code"));
	CHECK(!containsMessage(loweringDiagnostics("int main() { long long a = 12, b = 5; return (int)(a % b); }"), "not supported in generated code"));

	std::string mul = functionIr("int main() { long long a = 3, b = 4; return (int)(a * b); }");
	CHECK(mul.find("__builtin_mulhu") != std::string::npos);
	CHECK(functionIr("int main() { long long a = 12, b = 4; return (int)(a / b); }").find("call __cc_div64") != std::string::npos);
	CHECK(functionIr("int main() { long long a = 12, b = 5; return (int)(a % b); }").find("call __cc_div64") != std::string::npos);
}

TEST(ir, a_64_bit_parameter_and_return_lower_to_the_two_word_convention)
{
	// F3.4: a wide parameter crosses as the address of its pair, and a wide return goes back in two
	// words. Both lower with no diagnostic now (they were the "not supported" cases before this
	// phase).
	CHECK(!containsMessage(loweringDiagnostics("long long f(long long v) { return v; } int main() { return 0; }"), "not supported in generated code"));
	CHECK(!containsMessage(loweringDiagnostics("long long f() { return 0x100000000LL; } int main() { return (int)f(); }"), "not supported in generated code"));

	// A wide argument is a Param marked wide, and a wide result defines a second (high) temp on the
	// Call. The `ret` of a wide function returns both words of the pair.
	std::string idBody = functionIr("long long id(long long v) { return v; } int main() { return (int)id(1); }", "id");
	CHECK(idBody.find("ret.wide") != std::string::npos);

	std::string call = functionIr("long long id(long long v) { return v; } int main() { return (int)id(1); }", "main");
	CHECK(call.find("call.wide id") != std::string::npos);
	CHECK(call.find("param.wide") != std::string::npos);
}

TEST(ir, a_64_bit_value_works_in_a_ternary_and_as_an_index)
{
	// A wide ternary copies the chosen arm's two words; a wide index is truncated to a word, which
	// is the `int` C converts a subscript to. Both must lower with no diagnostic.
	CHECK(!containsMessage(loweringDiagnostics("int main() { long long a = 1, b = 2; long long t = a < b ? a : b; return (int)t; }"), "not supported in generated code"));
	CHECK(!containsMessage(loweringDiagnostics("int main() { long long a = 1; int arr[2]; arr[a] = 3; return arr[0]; }"), "not supported in generated code"));
	CHECK(!containsMessage(loweringDiagnostics("int main() { long long a = 1; int arr[2]; long long* p = (long long*)arr; long long* q = p + a; return (int)q; }"), "not supported in generated code"));

	// A wide discriminant (F9) and a wide builtin operand (F3.3) have no lowering yet.
	CHECK(containsMessage(loweringDiagnostics("int main() { long long a = 1; switch (a) { case 1: return 0; } return 1; }"), "not supported in generated code"));
	CHECK(containsMessage(loweringDiagnostics("int main() { long long a = 1; return __builtin_clz(a); }"), "not supported in generated code"));
}

TEST(ir, a_signed_min_or_max_ternary_lowers_to_one_builtin_when_enabled)
{
	support::OptimizationOptions options = support::OptimizationOptions::none();
	options.minMaxIdioms = true;

	CHECK(functionIr("int mn(int a, int b) { return a < b ? a : b; }", "mn", options).find("__builtin_imin") != std::string::npos);
	CHECK(functionIr("int mx(int a, int b) { return a > b ? a : b; }", "mx", options).find("__builtin_imax") != std::string::npos);
	CHECK(functionIr("int mx(int a, int b) { return a < b ? b : a; }", "mx", options).find("__builtin_imax") != std::string::npos);
	CHECK(functionIr("int mn(int a, int b) { return a >= b ? b : a; }", "mn", options).find("__builtin_imin") != std::string::npos);
}

TEST(ir, an_unsigned_min_or_max_ternary_picks_the_unsigned_builtin)
{
	support::OptimizationOptions options = support::OptimizationOptions::none();
	options.minMaxIdioms = true;

	CHECK(functionIr("unsigned mn(unsigned a, unsigned b) { return a < b ? a : b; }", "mn", options).find("__builtin_umin") != std::string::npos);
	CHECK(functionIr("unsigned mx(unsigned a, unsigned b) { return a > b ? a : b; }", "mx", options).find("__builtin_umax") != std::string::npos);
}

TEST(ir, the_abs_ternary_lowers_to_one_builtin_and_its_mirror_does_not)
{
	support::OptimizationOptions options = support::OptimizationOptions::none();
	options.minMaxIdioms = true;

	CHECK(functionIr("int ab(int a) { return a < 0 ? -a : a; }", "ab", options).find("__builtin_abs") != std::string::npos);
	CHECK(functionIr("int ab(int a) { return 0 > a ? -a : a; }", "ab", options).find("__builtin_abs") != std::string::npos);
	CHECK(functionIr("int ab(int a) { return a > 0 ? a : -a; }", "ab", options).find("__builtin_abs") != std::string::npos);
	// The non-strict mirrors of the same shapes.
	CHECK(functionIr("int ab(int a) { return a <= 0 ? -a : a; }", "ab", options).find("__builtin_abs") != std::string::npos);
	CHECK(functionIr("int ab(int a) { return a >= 0 ? a : -a; }", "ab", options).find("__builtin_abs") != std::string::npos);
	CHECK(functionIr("int ab(int a) { return 0 <= a ? a : -a; }", "ab", options).find("__builtin_abs") != std::string::npos);
	// `0 > a ? a : -a` selects the negative value when a is negative - that is -abs, not abs.
	CHECK(functionIr("int na(int a) { return 0 > a ? a : -a; }", "na", options).find("__builtin_abs") == std::string::npos);
}

TEST(ir, a_float_or_pointer_min_max_keeps_the_diamond)
{
	support::OptimizationOptions options = support::OptimizationOptions::none();
	options.minMaxIdioms = true;

	// A float select is not fmin/fmax (NaN and -0.0 make them differ), and a pointer select has no
	// integer min/max to map to.
	CHECK(functionIr("float f(float a, float b) { return a < b ? a : b; }", "f", options).find("__builtin_") == std::string::npos);
	CHECK(functionIr("int* f(int* a, int* b) { return a < b ? a : b; }", "f", options).find("__builtin_") == std::string::npos);
}

TEST(ir, a_volatile_operand_keeps_the_diamond_so_no_observable_read_is_dropped)
{
	support::OptimizationOptions options = support::OptimizationOptions::none();
	options.minMaxIdioms = true;

	// The ternary reads the selected operand a second time (one load per arm), so folding to a
	// single builtin would drop volatile reads. The fold must decline.
	std::string text = functionIr("int f(volatile int a, volatile int b) { return a < b ? a : b; }", "f", options);
	CHECK(text.find("__builtin_") == std::string::npos);
	usize loads = 0;
	for (usize at = text.find("load.word.v"); at != std::string::npos; at = text.find("load.word.v", at + 1))
		++loads;
	CHECK(loads > 2); // the diamond's two condition loads plus one per arm
}

TEST(ir, a_non_idiom_ternary_and_side_effecting_operands_keep_the_diamond)
{
	support::OptimizationOptions options = support::OptimizationOptions::none();
	options.minMaxIdioms = true;

	// `==` is not a min/max test even though its arms are the operands.
	std::string equality = functionIr("int f(int a, int b) { return a == b ? a : b; }", "f", options);
	CHECK(equality.find("__builtin_") == std::string::npos);
	CHECK(equality.find("cmp.eq") != std::string::npos);

	// A call in the arms would be evaluated twice by the original, once by the builtin.
	std::string calls = functionIr("int f(void); int g(void); int h(void) { return f() < g() ? f() : g(); }", "h", options);
	CHECK(calls.find("__builtin_") == std::string::npos);
}

TEST(ir, the_min_max_idiom_flag_off_keeps_the_branch_diamond)
{
	std::string text = functionIr("int mn(int a, int b) { return a < b ? a : b; }", "mn");
	CHECK(text.find("__builtin_") == std::string::npos);
	CHECK(text.find("cmp.lt") != std::string::npos);
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
		"  %5 = const 1\n"
		"  %6 = &local 0\n"
		"  store.word [%6], %5\n"
		"  jmp L3\n"
		"L2:\n"
		"  %7 = const 2\n"
		"  %8 = &local 0\n"
		"  store.word [%8], %7\n"
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
		"  %6 = load.word [%5]\n"
		"  %7 = const 1\n"
		"  %8 = sub %6, %7\n"
		"  %9 = &local 0\n"
		"  store.word [%9], %8\n"
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
		"  %3 = load.word [%2]\n"
		"  %4 = const 1\n"
		"  %5 = add %3, %4\n"
		"  %6 = &local 0\n"
		"  store.word [%6], %5\n"
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
		"  %0 = const 0\n"
		"  %1 = &local 0\n"
		"  store.word [%1], %0\n"
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
		"  %13 = load.word [%12]\n"
		"  %14 = const 1\n"
		"  %15 = add %13, %14\n"
		"  %16 = &local 0\n"
		"  store.word [%16], %15\n"
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
		"  %9 = load.word [%8]\n"
		"  %10 = const 1\n"
		"  %11 = add %9, %10\n"
		"  %12 = &local 1\n"
		"  store.word [%12], %11\n"
		"  jmp L2\n"
		"L2:\n"
		"  %13 = &local 1\n"
		"  %14 = load.word [%13]\n"
		"  %15 = const 2\n"
		"  %16 = add %14, %15\n"
		"  %17 = &local 1\n"
		"  store.word [%17], %16\n"
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

// ---- switch: jump table / binary search (options().jumpTables) ---------------------------------

TEST(ir, a_dense_switch_lowers_to_a_jump_table_when_enabled)
{
	support::OptimizationOptions options = support::OptimizationOptions::none();
	options.jumpTables = true;
	std::string text = functionIr(
		"int main() {\n"
		"    int x = 2;\n"
		"    switch (x) {\n"
		"        case 1: return 10;\n"
		"        case 2: return 20;\n"
		"        case 3: return 30;\n"
		"        case 4: return 40;\n"
		"        case 5: return 50;\n"
		"        default: return 0;\n"
		"    }\n"
		"}\n", "main", options);
	// One multi-way terminator over the five case blocks (in value order), `default` for the rest -
	// and none of the comparison chain's `br.eq` tests.
	CHECK(text.find("tbl.jmp") != std::string::npos);
	CHECK(text.find("- 1, [L1, L2, L3, L4, L5], default L6") != std::string::npos);
	CHECK(text.find("br.eq") == std::string::npos);
}

TEST(ir, a_sparse_switch_lowers_to_a_balanced_tree_when_enabled)
{
	support::OptimizationOptions options = support::OptimizationOptions::none();
	options.jumpTables = true;
	std::string text = functionIr(
		"int main() {\n"
		"    int x = 2;\n"
		"    switch (x) {\n"
		"        case 0:   return 1;\n"
		"        case 100: return 2;\n"
		"        case 200: return 3;\n"
		"        case 300: return 4;\n"
		"        case 400: return 5;\n"
		"        case 500: return 6;\n"
		"        case 600: return 7;\n"
		"        case 700: return 8;\n"
		"        default:  return 0;\n"
		"    }\n"
		"}\n", "main", options);
	// Too wide for a table, so the dispatch is a tree: ordering tests (`br.lt`) that split the range
	// and equality tests (`br.eq`) at the leaves - never a table, never the linear chain.
	CHECK(text.find("tbl.jmp") == std::string::npos);
	CHECK(text.find("br.lt") != std::string::npos);
	CHECK(text.find("br.eq") != std::string::npos);
}

TEST(ir, a_small_switch_keeps_the_comparison_chain_even_with_jump_tables_on)
{
	support::OptimizationOptions options = support::OptimizationOptions::none();
	options.jumpTables = true;
	std::string text = functionIr(
		"int main() {\n"
		"    int x = 2;\n"
		"    switch (x) {\n"
		"        case 1: return 10;\n"
		"        case 2: return 20;\n"
		"        default: return 0;\n"
		"    }\n"
		"}\n", "main", options);
	// Below both thresholds: neither a table nor a tree, just the plain chain.
	CHECK(text.find("tbl.jmp") == std::string::npos);
	CHECK(text.find("br.lt") == std::string::npos);
	CHECK(text.find("br.eq") != std::string::npos);
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
		"  %2 = const 1\n"
		"  %3 = &local 0\n"
		"  store.word [%3], %2\n"
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
	//
	// The `narrow.byte` in front of the store is the int-to-char conversion `p.y = 1` performs. It
	// is emitted even for a literal that already fits: this builder tracks no constants, and
	// constant folding removes it at every level above -O0 (ir_optimizer.cpp's foldUnOp).
	CHECK_EQ(text,
		"function main(params=0, locals=1) {\n"
		"L0:\n"
		"  %0 = const 1\n"
		"  %1 = narrow.byte %0\n"
		"  %2 = &local 0\n"
		"  %3 = const 4\n"
		"  %4 = add.u %2, %3\n"
		"  store.byte [%4], %1\n"
		"  %5 = &local 0\n"
		"  %6 = load.word [%5]\n"
		"  ret %6\n"
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
		ast::Decl* arrDecl = arena.create<ast::VarDecl>(loc, std::string_view("arr"), arrayType);
		ast::DeclStmt* declStmt = arena.create<ast::DeclStmt>(loc, std::span<ast::Decl* const>(&arrDecl, 1));
		ast::ReturnStmt* ret = arena.create<ast::ReturnStmt>(loc, returnValue);

		std::vector<ast::Stmt*> stmts{ declStmt };
		if (bodyExpr)
			stmts.push_back(arena.create<ast::ExprStmt>(loc, bodyExpr));
		stmts.push_back(ret);
		ast::CompoundStmt* body = arena.create<ast::CompoundStmt>(loc, std::span<ast::Stmt* const>(stmts));

		ast::FunctionDecl* func = arena.create<ast::FunctionDecl>(loc, std::string_view("main"), &ast::Type::Int, std::span<const ast::Param>{}, body);
		std::vector<ast::Decl*> decls{ func };
		ast::TranslationUnit unit(loc, std::span<ast::Decl* const>(decls));

		ir::IrBuilder builder(arena, diagnostics, support::OptimizationOptions::none());
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
		"  %0 = const 7\n"
		"  %1 = &local 0\n"
		"  store.word [%1], %0\n"
		"  %2 = const 0\n"
		"  ret %2\n"
		"}\n");
}

// ---- array declarators end-to-end (now that libs/parser can actually produce them) --------------

TEST(ir, array_decays_to_its_frame_address_when_passed_as_a_call_argument)
{
	std::string text = functionIr("void f(int* p); int main() { int arr[4]; f(arr); return 0; }");
	CHECK_EQ(text,
		"function main(params=0, locals=1) {\n"
		"L0:\n"
		"  %0 = &local 0\n"
		"  param %0\n"
		"  call f, 1\n"
		"  %1 = const 0\n"
		"  ret %1\n"
		"}\n");
}

TEST(ir, indexing_a_local_array_scales_the_index_by_the_element_size_and_loads)
{
	std::string text = functionIr("int main() { int arr[4]; return arr[2]; }");
	CHECK_EQ(text,
		"function main(params=0, locals=1) {\n"
		"L0:\n"
		"  %0 = &local 0\n"
		"  %1 = const 2\n"
		"  %2 = const 4\n"
		"  %3 = mul.u %1, %2\n"
		"  %4 = add.u %0, %3\n"
		"  %5 = load.word [%4]\n"
		"  ret %5\n"
		"}\n");
}

TEST(ir, two_dimensional_array_indexing_computes_the_row_address_before_the_column_offset)
{
	// `m[1][2]` on int m[3][4]: the outer index scales by the ROW size (4 ints = 16 bytes), the
	// inner one by the element size (4 bytes) - not the same constant twice.
	std::string text = functionIr("int main() { int m[3][4]; return m[1][2]; }");
	CHECK_EQ(text,
		"function main(params=0, locals=1) {\n"
		"L0:\n"
		"  %0 = &local 0\n"
		"  %1 = const 1\n"
		"  %2 = const 16\n"
		"  %3 = mul.u %1, %2\n"
		"  %4 = add.u %0, %3\n"
		"  %5 = const 2\n"
		"  %6 = const 4\n"
		"  %7 = mul.u %5, %6\n"
		"  %8 = add.u %4, %7\n"
		"  %9 = load.word [%8]\n"
		"  ret %9\n"
		"}\n");
}

TEST(ir, pointer_arithmetic_on_a_decayed_array_scales_by_the_element_size)
{
	// Regression: lowerArithmetic() used to check ONLY isPointer(), so `arr + 1` on an
	// undecayed-at-the-node-level array operand (sema annotates node.lhs() with its real Array
	// type, only decaying a local copy for its own checks - see Sema::decayArray()) silently fell
	// through to plain integer addition, skipping the *4 element-size scaling entirely.
	std::string text = functionIr("int main() { int arr[4]; int* p = arr + 1; return *p; }");
	CHECK_EQ(text,
		"function main(params=0, locals=2) {\n"
		"L0:\n"
		"  %0 = &local 0\n"
		"  %1 = const 1\n"
		"  %2 = const 4\n"
		"  %3 = mul.u %1, %2\n"
		"  %4 = add.u %0, %3\n"
		"  %5 = &local 1\n"
		"  store.word [%5], %4\n"
		"  %6 = &local 1\n"
		"  %7 = load.word [%6]\n"
		"  %8 = load.word [%7]\n"
		"  ret %8\n"
		"}\n");
}

// ---- float support (IrBinOp/IrUnOp/IrCmp/IrLoad/IrStore/IrCall/IrReturn's isFloat, plus the real
// ---- IntToFloat/FloatToInt conversion ops CastExpr/mixed arithmetic now emit) -------------------

TEST(ir, float_arithmetic_uses_the_float_binop_form)
{
	std::string text = functionIr("float main() { float a; float b; return a + b; }");
	CHECK_EQ(text,
		"function main(params=0, locals=2) {\n"
		"L0:\n"
		"  %0 = &local 0\n"
		"  %1 = load.word.f [%0]\n"
		"  %2 = &local 1\n"
		"  %3 = load.word.f [%2]\n"
		"  %4 = add.f.u %1, %3\n"
		"  ret.f %4\n"
		"}\n");
}

TEST(ir, mixed_int_and_float_arithmetic_converts_the_int_operand_first)
{
	// Sema unifies `int + float` onto float without inserting a cast node (commonArithmeticType(),
	// sema.cpp) - this is the regression that used to compute `fadd` directly on the int's raw bit
	// pattern instead of its numeric value. See IrBuilder::toFloatIfNeeded()'s header comment.
	std::string text = functionIr("float main() { int a; float b; return a + b; }");
	CHECK_EQ(text,
		"function main(params=0, locals=2) {\n"
		"L0:\n"
		"  %0 = &local 0\n"
		"  %1 = load.word [%0]\n"
		"  %2 = &local 1\n"
		"  %3 = load.word.f [%2]\n"
		"  %4 = itof %1\n"
		"  %5 = add.f.u %4, %3\n"
		"  ret.f %5\n"
		"}\n");
}

TEST(ir, float_comparison_used_as_a_condition_is_marked_float_and_unsigned)
{
	// `ifXX` on a float pair must read through the UNSIGNED branch family no matter the mathematical
	// sign of the comparison - FCMP puts `fs < ft` directly in Carry (05-Instruction-Set.md) - see
	// isUnsignedComparison()'s isFloat case.
	std::string text = functionIr("int main() { float a; float b; if (a < b) return 1; return 0; }");
	CHECK_EQ(text,
		"function main(params=0, locals=2) {\n"
		"L0:\n"
		"  %0 = &local 0\n"
		"  %1 = load.word.f [%0]\n"
		"  %2 = &local 1\n"
		"  %3 = load.word.f [%2]\n"
		"  %4 = cmp.lt.f.u %1, %3\n"
		"  %5 = const 0\n"
		"  br.ne %4, %5, L1, L2\n"
		"L1:\n"
		"  %6 = const 1\n"
		"  ret %6\n"
		"L2:\n"
		"  %7 = const 0\n"
		"  ret %7\n"
		"}\n");
}

TEST(ir, explicit_cast_from_int_to_float_emits_a_real_conversion)
{
	// Regression: visit(CastExpr&) used to just pass the operand through unconverted, silently
	// reinterpreting an int's bit pattern as a float instead of actually converting its value.
	std::string text = functionIr("float main() { int a; return (float)a; }");
	CHECK_EQ(text,
		"function main(params=0, locals=1) {\n"
		"L0:\n"
		"  %0 = &local 0\n"
		"  %1 = load.word [%0]\n"
		"  %2 = itof %1\n"
		"  ret.f %2\n"
		"}\n");
}

TEST(ir, explicit_cast_from_float_to_int_emits_a_real_conversion)
{
	std::string text = functionIr("int main() { float a; return (int)a; }");
	CHECK_EQ(text,
		"function main(params=0, locals=1) {\n"
		"L0:\n"
		"  %0 = &local 0\n"
		"  %1 = load.word.f [%0]\n"
		"  %2 = ftoi %1\n"
		"  ret %2\n"
		"}\n");
}

TEST(ir, float_function_parameter_and_call_argument_are_both_marked_float)
{
	std::string text = functionIr("float g(float x); float main() { float a; return g(a); }");
	CHECK_EQ(text,
		"function main(params=0, locals=1) {\n"
		"L0:\n"
		"  %0 = &local 0\n"
		"  %1 = load.word.f [%0]\n"
		"  param.f %1\n"
		"  %2 = call.f g, 1\n"
		"  ret.f %2\n"
		"}\n");
}

// ---- composite memory: initializers, whole-struct moves, the struct ABI (Fase 7) ---------------

TEST(ir, an_array_initializer_list_stores_each_element_and_zero_fills_the_rest)
{
	// One store per listed element at its own constant offset, then one more per element the list
	// did not reach - C promises the rest is zero, and there is no memset to call (§14).
	std::string text = functionIr("int main() { int a[3] = { 7, 8 }; return a[0]; }");
	CHECK_EQ(text,
		"function main(params=0, locals=1) {\n"
		"L0:\n"
		"  %0 = &local 0\n"
		"  %1 = const 7\n"
		"  store.word [%0], %1\n"
		"  %2 = const 8\n"
		"  %3 = const 4\n"
		"  %4 = add.u %0, %3\n"
		"  store.word [%4], %2\n"
		"  %5 = const 0\n"
		"  %6 = const 8\n"
		"  %7 = add.u %0, %6\n"
		"  store.word [%7], %5\n"
		"  %8 = &local 0\n"
		"  %9 = const 0\n"
		"  %10 = const 4\n"
		"  %11 = mul.u %9, %10\n"
		"  %12 = add.u %8, %11\n"
		"  %13 = load.word [%12]\n"
		"  ret %13\n"
		"}\n");
}

TEST(ir, a_char_array_initialized_from_a_string_literal_stores_its_bytes_not_a_pointer)
{
	// The literal's bytes go into the array itself - nothing reaches the string-literal table,
	// which is what a pointer-to-.rodata initializer would produce instead.
	support::Arena arena;
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	lexer::Lexer lexer("int main() { char s[4] = \"hi\"; return s[0]; }", testSourceId(), diagnostics, pool);
	parser::Parser parser(lexer, arena, diagnostics);
	ast::TranslationUnit* unit = parser.parseTranslationUnit();
	CHECK(unit != nullptr);
	sema::Sema sema(arena, diagnostics);
	CHECK(sema.check(*unit));

	ir::IrBuilder builder(arena, diagnostics, support::OptimizationOptions::none());
	ir::IrModule module = builder.build(*unit);
	CHECK_EQ(module.stringLiterals().size(), usize(0));

	std::string text = ir::IrPrinter{}.print(module);
	CHECK(text.find("store.byte") != std::string::npos);
	CHECK(text.find("&global") == std::string::npos);
}

TEST(ir, assigning_one_struct_to_another_copies_it_word_by_word)
{
	// A struct-typed expression IS its address (ir_builder.h), so this is a Load/Store pair per
	// word rather than one scalar store - and one pair, not two, per four aligned bytes.
	std::string text = functionIr(
		"struct P { int x; int y; };"
		"int main() { struct P a; struct P b; b = a; return b.x; }");
	CHECK_EQ(text,
		"function main(params=0, locals=2) {\n"
		"L0:\n"
		"  %0 = &local 0\n"
		"  %1 = &local 1\n"
		"  %2 = load.word [%0]\n"
		"  store.word [%1], %2\n"
		"  %3 = const 4\n"
		"  %4 = add.u %0, %3\n"
		"  %5 = load.word [%4]\n"
		"  %6 = const 4\n"
		"  %7 = add.u %1, %6\n"
		"  store.word [%7], %5\n"
		"  %8 = &local 1\n"
		"  %9 = load.word [%8]\n"
		"  ret %9\n"
		"}\n");
}

TEST(ir, a_struct_of_chars_is_copied_byte_by_byte_because_its_alignment_says_so)
{
	// The copy's piece size comes from the TYPE's alignment, not from the frame slot's - a pointer
	// to such a struct may point anywhere, and a word load off a one-aligned address faults.
	std::string text = functionIr(
		"struct Bytes { char a; char b; char c; };"
		"int main() { struct Bytes x; struct Bytes y; y = x; return y.a; }");
	CHECK(text.find("load.byte") != std::string::npos);
	CHECK(text.find("load.word") == std::string::npos);
}

TEST(ir, returning_a_struct_wider_than_a_word_uses_a_hidden_destination_pointer)
{
	// The architecture plan's own "puntero oculto en arg0, argumentos visibles corridos uno":
	// `make` takes one parameter it never declared (local 0, the destination), copies its result
	// there, and returns that same pointer.
	constexpr std::string_view source =
		"struct P { int x; int y; };"
		"struct P make() { struct P p; p.x = 1; return p; }"
		"int main() { struct P q = make(); return q.x; }";

	CHECK_EQ(functionIr(source, "make"),
		"function make(params=1, locals=2) {\n"
		"L0:\n"
		"  %0 = const 1\n"
		"  %1 = &local 1\n"
		"  store.word [%1], %0\n"
		"  %2 = &local 1\n"
		"  %3 = &local 0\n"
		"  %4 = load.word [%3]\n"
		"  %5 = load.word [%2]\n"
		"  store.word [%4], %5\n"
		"  %6 = const 4\n"
		"  %7 = add.u %2, %6\n"
		"  %8 = load.word [%7]\n"
		"  %9 = const 4\n"
		"  %10 = add.u %4, %9\n"
		"  store.word [%10], %8\n"
		"  ret %4\n"
		"}\n");

	// The caller hands over a frame slot of its own, then copies out of it - the second copy this
	// version deliberately does not elide (see ir_builder.h).
	std::string caller = functionIr(source, "main");
	CHECK(caller.find("param %1\n  call make, 1") != std::string::npos);
	CHECK(caller.find("%2 = load.word [%1]") != std::string::npos);
}

TEST(ir, returning_a_struct_that_fits_one_word_comes_back_in_the_return_register)
{
	// 1/2/4 bytes travel in ret0 - no hidden parameter at all, so `small` really does take none.
	constexpr std::string_view source =
		"struct One { int x; };"
		"struct One make() { struct One s; s.x = 7; return s; }"
		"int main() { struct One s = make(); return s.x; }";
	CHECK(functionIr(source, "make").find("function make(params=0,") != std::string::npos);
	CHECK(functionIr(source, "main").find("call make, 0") != std::string::npos);
}

TEST(ir, a_three_byte_struct_goes_through_memory_even_though_it_would_fit_a_register)
{
	// A word store would write a fourth byte the object does not own, and there is no three-byte
	// store - so 3 is on the indirect side of the rule, not with 1/2/4.
	constexpr std::string_view source =
		"struct Three { char a; char b; char c; };"
		"struct Three make() { struct Three t; t.a = 1; return t; }"
		"int main() { struct Three t = make(); return t.a; }";
	CHECK(functionIr(source, "make").find("function make(params=1,") != std::string::npos);
	CHECK(functionIr(source, "main").find("call make, 1") != std::string::npos);
}

TEST(ir, passing_a_struct_by_value_passes_the_address_of_a_copy_the_caller_made)
{
	// By-value semantics without a by-value register class: the caller copies into a slot of its
	// own frame and passes that slot's address, so the callee may write through it freely.
	std::string text = functionIr(
		"struct P { int x; int y; };"
		"int total(struct P p);"
		"int main() { struct P a; return total(a); }");
	CHECK_EQ(text,
		"function main(params=0, locals=2) {\n"
		"L0:\n"
		"  %0 = &local 0\n"
		"  %1 = &local 1\n"
		"  %2 = load.word [%0]\n"
		"  store.word [%1], %2\n"
		"  %3 = const 4\n"
		"  %4 = add.u %0, %3\n"
		"  %5 = load.word [%4]\n"
		"  %6 = const 4\n"
		"  %7 = add.u %1, %6\n"
		"  store.word [%7], %5\n"
		"  param %1\n"
		"  %8 = call total, 1\n"
		"  ret %8\n"
		"}\n");
}

TEST(ir, reading_a_by_value_struct_parameter_loads_the_pointer_the_caller_passed)
{
	// Inside the callee the parameter's slot holds a POINTER, so `p.x` is a load of that pointer
	// followed by the field access - not a FrameAddr of the slot itself.
	std::string text = functionIr(
		"struct P { int x; int y; };"
		"int total(struct P p) { return p.x; }", "total");
	CHECK_EQ(text,
		"function total(params=1, locals=1) {\n"
		"L0:\n"
		"  %0 = &local 0\n"
		"  %1 = load.word [%0]\n"
		"  %2 = load.word [%1]\n"
		"  ret %2\n"
		"}\n");
}

TEST(ir, a_struct_returning_calls_result_can_be_read_through_a_member_access)
{
	// `make().b` is not an lvalue (sema rejects writing it) but reading it is ordinary: the call's
	// result IS the address of the temp slot it wrote into, so the field offset applies to it
	// directly - no diagnostic, no bogus address.
	std::string text = functionIr(
		"struct P { int a; int b; };\n"
		"struct P make();\n"
		"int main() { return make().b; }\n");
	CHECK(text.find("call make, 1") != std::string::npos);
	CHECK(text.find("load.word") != std::string::npos);
}

TEST(ir, chained_struct_assignment_copies_twice_from_the_same_source)
{
	// `a = b = c` works because a struct assignment's own value is its destination's address, so
	// the outer copy reads out of the inner one's destination - C's by-value chain.
	std::string text = functionIr(
		"struct P { int x; };"
		"int main() { struct P a; struct P b; struct P c; a = b = c; return a.x; }");
	CHECK(text.find("function main(params=0, locals=3)") != std::string::npos);
	CHECK_EQ(std::count(text.begin(), text.end(), '\n'), usize(13));
}

TEST(ir, a_word_sized_byte_aligned_struct_goes_through_memory)
{
	// It fits in a register but cannot safely be loaded as a word from a byte-aligned subobject.
	constexpr std::string_view source =
		"struct Quad { char a; char b; char c; char d; };"
		"struct Wrap { char pad; struct Quad q; };"
		"int total(struct Quad q);"
		"int main() { struct Wrap w; return total(w.q); }";
	std::string text = functionIr(source, "main");
	CHECK(text.find("load.byte") != std::string::npos);
	CHECK(text.find("call total, 1") != std::string::npos);
}

// ---- variadic functions ----------------------------------------------------------------------

namespace
{
	bool contains(std::string_view haystack, std::string_view needle)
	{
		return haystack.find(needle) != std::string_view::npos;
	}

	usize countOccurrences(std::string_view haystack, std::string_view needle)
	{
		usize count = 0;
		for (usize at = haystack.find(needle); at != std::string_view::npos; at = haystack.find(needle, at + needle.size()))
			++count;
		return count;
	}
}

TEST(ir, a_variadic_function_is_marked_as_one_and_va_start_lowers_to_its_own_opcode)
{
	std::string text = functionIr("int f(int a, ...) { __builtin_va_list ap; __builtin_va_start(ap, a); return 0; }", "f");
	CHECK(contains(text, "function f(params=1, ..., locals=2)"));
	CHECK(contains(text, "= va_start"));
}

TEST(ir, va_arg_lowers_to_a_load_through_the_cursor_and_a_four_byte_advance)
{
	std::string text = functionIr("int f(int a, ...) { __builtin_va_list ap; __builtin_va_start(ap, a); return __builtin_va_arg(ap, int); }", "f");
	CHECK(contains(text, "= va_start"));
	CHECK(contains(text, "const 4"));
	CHECK(contains(text, "add"));
}

TEST(ir, a_calls_variadic_arguments_are_marked_and_its_fixed_ones_are_not)
{
	// Only the arguments past the callee's declared arity carry the marker - that is what tells
	// codegen to put them on the stack rather than in an argument register.
	std::string text = functionIr("int f(int a, int b, ...); int main() { return f(1, 2, 3, 4); }");
	CHECK_EQ(countOccurrences(text, "param.var "), usize(2));
	CHECK_EQ(countOccurrences(text, "param "), usize(2));
}

TEST(ir, an_ordinary_call_marks_nothing_as_variadic)
{
	std::string text = functionIr("int f(int a, int b); int main() { return f(1, 2); }");
	CHECK_EQ(countOccurrences(text, "param.var "), usize(0));
	CHECK_EQ(countOccurrences(text, "param "), usize(2));
}

// ---- volatile through a pointer --------------------------------------------------------------

TEST(ir, an_access_through_a_volatile_pointee_is_marked_on_the_access_itself)
{
	// `volatile int* p` qualifies the POINTEE, so there is no local slot to hang the fact on -
	// IrLocalSlot::isVolatile cannot express it and the access has to carry it.
	std::string text = functionIr("int f(volatile int* p) { *p = 1; return *p; }", "f");
	CHECK(contains(text, "store.word.v"));
	CHECK(contains(text, "load.word.v"));

	// The pointer variable itself is not volatile, so reading `p` is an ordinary load.
	CHECK(contains(text, "load.word ["));
}

TEST(ir, an_access_through_an_ordinary_pointer_is_not_marked)
{
	std::string text = functionIr("int f(int* p) { *p = 1; return *p; }", "f");
	CHECK(!contains(text, ".v"));
}

TEST(ir, a_volatile_pointer_declared_in_a_block_marks_the_same_accesses_as_the_parameter_form)
{
	// The two spellings of one type have to lower identically. They did not: a declaration applied
	// `volatile` to the FINISHED type, after the `*`, producing `int* volatile` - a volatile local
	// pointing at an ordinary int - so the two device accesses this exists for came out unmarked
	// while the store into the pointer itself got the flag nobody needed there.
	std::string text = functionIr(
		"int f(void) { volatile int* term = (volatile int*)0xFF000004; *term = 1; return *term; }", "f");
	CHECK(contains(text, "store.word.v"));
	CHECK(contains(text, "load.word.v"));
}

TEST(ir, a_volatile_pointer_is_not_the_same_type_as_a_pointer_to_volatile)
{
	// The other side of the distinction, and the exact mirror of the test above: here the POINTER
	// is volatile and what it points at is not, so reading `p` is the observable access and the
	// dereference through it is the ordinary one. Both marks appear, on the opposite instructions.
	std::string text = functionIr("int f(void) { int* volatile p = 0; return *p; }", "f");
	CHECK(contains(text, "store.word.v"));  // writing the pointer
	CHECK(contains(text, "load.word.v"));   // reading it back
	CHECK(contains(text, "load.word ["));   // ...and an ordinary load through it
}

TEST(ir, a_member_of_a_volatile_struct_is_volatile)
{
	// Only `const` used to travel from a qualified struct to its fields, so every access to a
	// member of a `volatile struct` came out unmarked - which is the memory-mapped register block
	// a program declares `volatile` for in the first place.
	std::string direct = functionIr(
		"struct R { int a; }; volatile struct R g; int f(void) { g.a = 1; return g.a; }", "f");
	CHECK(contains(direct, "store.word.v"));
	CHECK(contains(direct, "load.word.v"));

	// Through a pointer to a volatile struct, where `->` has to reach the same conclusion.
	std::string arrow = functionIr(
		"struct R { int a; }; int f(volatile struct R* p) { p->a = 1; return p->a; }", "f");
	CHECK(contains(arrow, "store.word.v"));
	CHECK(contains(arrow, "load.word.v"));
}

TEST(ir, a_member_of_an_ordinary_struct_is_not_volatile)
{
	std::string text = functionIr(
		"struct R { int a; }; int f(struct R* p) { p->a = 1; return p->a; }", "f");
	CHECK(!contains(text, ".v"));
}

// ---- function pointers ---------------------------------------------------------------------------

TEST(ir, a_call_through_a_pointer_lowers_to_an_indirect_call)
{
	// A direct call names its callee and the linker resolves it. An indirect one carries an address
	// instead, computed like any other value - so it prints the temporary it jumps through.
	std::string text = functionIr(
		"int f(int x); int main() { int (*p)(int) = f; return p(1); }", "main");
	CHECK(contains(text, "&global \"f\""));   // the address of the function, taken as a value
	CHECK(contains(text, "call %"));          // ...and jumped through
}

TEST(ir, a_call_by_name_is_still_a_direct_call)
{
	// The contrast that makes the test above about pointers rather than about every call changing
	// shape. Nothing about a direct call's lowering moved.
	std::string text = functionIr("int f(int x); int main() { return f(1); }", "main");
	CHECK(contains(text, "call f, 1"));
}

TEST(ir, the_arguments_of_an_indirect_call_stay_contiguous_with_it)
{
	// Codegen reads a call's arguments back by position, from the Param instructions immediately
	// before it - so the callee's own address has to be computed before that run begins, not
	// between the last Param and the Call.
	std::string text = functionIr(
		"int f(int a, int b); int main() { int (*p)(int, int) = f; return p(1, 2); }", "main");
	// The two instructions before the call are its two Params, with nothing in between.
	std::vector<std::string> lines;
	for (usize start = 0; start < text.size();)
	{
		usize end = text.find('\n', start);
		if (end == std::string::npos)
			break;
		lines.push_back(text.substr(start, end - start));
		start = end + 1;
	}
	usize callLine = 0;
	for (usize i = 0; i < lines.size(); ++i)
		if (lines[i].find("call %") != std::string::npos)
			callLine = i;
	CHECK(callLine >= 2);
	CHECK(lines[callLine - 1].find("param ") != std::string::npos);
	CHECK(lines[callLine - 2].find("param ") != std::string::npos);
}
