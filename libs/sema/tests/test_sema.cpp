#include <ceresc/sema/sema.h>
#include <ceresc/sema/type_layout.h>
#include <ceresc/parser/parser.h>
#include <ceresc/ast/ast_printer.h>
#include <ceresc/lexer/lexer.h>
#include <ceresc/support/arena.h>
#include <ceresc/support/diagnostics.h>
#include <ceresc/support/string_pool.h>

#include "framework.h"

using namespace ceresc;

namespace
{
	support::SourceId testSourceId() { return support::SourceId::make(1); }

	struct CheckOutcome
	{
		bool ok = false;
		std::vector<std::string> messages;
	};

	// Parses `source` as a whole translation unit and runs Sema on it. `ok` is true only when the
	// file both parses cleanly and type-checks with zero errors - `messages` collects every
	// diagnostic (parse or sema) reported along the way, for tests that need to assert a specific
	// error was the one reported.
	CheckOutcome checkSource(std::string_view source)
	{
		support::Arena arena;
		support::DiagnosticEngine diagnostics;
		support::StringPool pool;
		lexer::Lexer lexer(source, testSourceId(), diagnostics, pool);
		parser::Parser parser(lexer, arena, diagnostics);

		CheckOutcome outcome;
		ast::TranslationUnit* unit = parser.parseTranslationUnit();
		if (unit && !diagnostics.hasErrors())
		{
			sema::Sema sema(arena, diagnostics);
			outcome.ok = sema.check(*unit);
		}
		else
		{
			outcome.ok = false;
		}

		for (const support::Diagnostic& diagnostic : diagnostics.diagnostics())
			outcome.messages.push_back(diagnostic.message);
		return outcome;
	}

	bool containsMessage(const CheckOutcome& outcome, std::string_view needle)
	{
		for (const std::string& message : outcome.messages)
		{
			if (message.find(needle) != std::string::npos)
				return true;
		}
		return false;
	}

	// Parses a full program (expected to define `int main() { ...; TARGET; }` as its last
	// declaration), runs Sema on it, and returns the printed type (ast_printer.h's typeName) of
	// main's last statement - which must be an ExprStmt. Used to assert what type Sema resolved a
	// specific expression to, since Sema has no public API to query an arbitrary node after check().
	//
	// `expectOk` (default true) also asserts sema.check() succeeded: without it, a source that
	// fails to type-check for an unrelated reason would still read back the offending expression's
	// error-recovery fallback type (usually "int") and a broken test could stay green. Pass false
	// for the handful of tests that deliberately exercise that same fallback behaviour on purpose.
	std::string typeOfMainLastExpr(std::string_view source, bool expectOk = true)
	{
		support::Arena arena;
		support::DiagnosticEngine diagnostics;
		support::StringPool pool;
		lexer::Lexer lexer(source, testSourceId(), diagnostics, pool);
		parser::Parser parser(lexer, arena, diagnostics);

		ast::TranslationUnit* unit = parser.parseTranslationUnit();
		if (!unit)
			return "<parse-failed>";

		sema::Sema sema(arena, diagnostics);
		bool ok = sema.check(*unit);
		if (expectOk)
			CHECK(ok);

		for (ast::Decl* decl : unit->decls())
		{
			auto* func = dynamic_cast<ast::FunctionDecl*>(decl);
			if (!func || func->name() != "main" || !func->body())
				continue;

			std::span<ast::Stmt* const> stmts = func->body()->stmts();
			if (stmts.empty())
				return "<empty-main>";

			auto* exprStmt = dynamic_cast<ast::ExprStmt*>(stmts.back());
			if (!exprStmt)
				return "<last-stmt-not-expr>";

			return ast::AstPrinter::typeName(exprStmt->expr()->type());
		}
		return "<no-main>";
	}
}

// ---- literal / arithmetic expression types -------------------------------------------------------

TEST(sema, literal_types)
{
	CHECK_EQ(typeOfMainLastExpr("int main() { 42; }"), "int");
	CHECK_EQ(typeOfMainLastExpr("int main() { 1.5; }"), "float");
	CHECK_EQ(typeOfMainLastExpr("int main() { 'a'; }"), "char");
	CHECK_EQ(typeOfMainLastExpr("int main() { true; }"), "bool");
	CHECK_EQ(typeOfMainLastExpr("int main() { \"hi\"; }"), "char*");
}

TEST(sema, small_integer_types_promote_to_int_in_arithmetic)
{
	CHECK_EQ(typeOfMainLastExpr("int main() { (char)1 + (char)2; }"), "int");
}

TEST(sema, mixed_int_and_float_promotes_to_float)
{
	CHECK_EQ(typeOfMainLastExpr("int main() { 1 + 1.5; }"), "float");
}

TEST(sema, unsigned_beats_signed_int_at_the_same_rank)
{
	CHECK_EQ(typeOfMainLastExpr("unsigned int u; int main() { u + 1; }"), "unsigned int");
}

TEST(sema, comparisons_produce_bool)
{
	CHECK_EQ(typeOfMainLastExpr("int main() { 1 < 2; }"), "bool");
	CHECK_EQ(typeOfMainLastExpr("int main() { 1 == 2; }"), "bool");
}

TEST(sema, logical_operators_produce_bool)
{
	CHECK_EQ(typeOfMainLastExpr("int main() { 1 && 0; }"), "bool");
	CHECK_EQ(typeOfMainLastExpr("int main() { !1; }"), "bool");
}

TEST(sema, sizeof_produces_unsigned_int)
{
	CHECK_EQ(typeOfMainLastExpr("int main() { sizeof(int); }"), "unsigned int");
}

TEST(sema, cast_result_type_is_the_target_type)
{
	// This alone only exercises the parser: a CastExpr already carries its target type before
	// Sema ever sees it (CastExpr::CastExpr calls setType() at construction, see expr.h), and
	// Sema::visit(CastExpr&) never re-sets it. Sema's own share of cast handling is the
	// operand/target compatibility check below.
	CHECK_EQ(typeOfMainLastExpr("int main() { (float)1; }"), "float");

	CheckOutcome badCast = checkSource("struct P { int x; }; int main() { struct P p; (int)p; }");
	CHECK(!badCast.ok);
	CHECK(containsMessage(badCast, "cannot cast"));
}

TEST(sema, ternary_with_matching_branch_types)
{
	CHECK_EQ(typeOfMainLastExpr("int main() { 1 ? 2 : 3; }"), "int");
}

TEST(sema, ternary_with_mixed_arithmetic_branches_promotes)
{
	CHECK_EQ(typeOfMainLastExpr("int main() { 1 ? 2 : 2.5; }"), "float");
}

TEST(sema, address_of_and_deref_round_trip)
{
	CHECK_EQ(typeOfMainLastExpr("int main() { int x; *&x; }"), "int");
}

TEST(sema, pointer_plus_int_stays_a_pointer)
{
	CHECK_EQ(typeOfMainLastExpr("int main() { int x; int* p; p = &x; p + 1; }"), "int*");
}

// ---- name resolution / scoping --------------------------------------------------------------------

TEST(sema, undeclared_identifier_is_an_error)
{
	CheckOutcome outcome = checkSource("int main() { x; }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "use of undeclared identifier"));
}

TEST(sema, variable_declared_before_use_resolves_cleanly)
{
	CheckOutcome outcome = checkSource("int main() { int x; x; }");
	CHECK(outcome.ok);
}

TEST(sema, shadowing_in_a_nested_block_is_allowed)
{
	CheckOutcome outcome = checkSource("int main() { int x; { int x; x; } }");
	CHECK(outcome.ok);
}

TEST(sema, redeclaration_in_the_same_scope_is_an_error)
{
	CheckOutcome outcome = checkSource("int main() { int x; int x; }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "redefinition of 'x'"));
}

TEST(sema, for_loop_variable_is_scoped_to_the_loop)
{
	// `i` must not leak past the for-statement's own scope into the enclosing block.
	CheckOutcome outcome = checkSource("int main() { for (int i = 0; i < 10; i = i + 1) { } i; }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "use of undeclared identifier"));
}

// ---- functions -------------------------------------------------------------------------------------

TEST(sema, function_call_with_matching_arguments_is_valid)
{
	CheckOutcome outcome = checkSource("int add(int a, int b) { return a + b; } int main() { add(1, 2); }");
	CHECK(outcome.ok);
}

TEST(sema, function_call_argument_count_mismatch_is_an_error)
{
	CheckOutcome outcome = checkSource("int add(int a, int b) { return a + b; } int main() { add(1); }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "expects 2 argument"));
}

TEST(sema, function_call_argument_type_mismatch_is_an_error)
{
	CheckOutcome outcome = checkSource(
		"struct P { int x; };"
		"void take(int a) { }"
		"int main() { struct P p; take(p); }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "incompatible type"));
}

TEST(sema, calling_an_undeclared_function_is_an_error)
{
	CheckOutcome outcome = checkSource("int main() { foo(1); }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "use of undeclared identifier"));
}

TEST(sema, calling_a_non_function_is_an_error)
{
	CheckOutcome outcome = checkSource("int main() { int x; x(1); }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "is not a function"));
}

TEST(sema, prototype_then_matching_definition_is_allowed)
{
	CheckOutcome outcome = checkSource("int add(int a, int b); int add(int a, int b) { return a + b; } int main() { add(1, 2); }");
	CHECK(outcome.ok);
}

TEST(sema, conflicting_redeclaration_is_an_error)
{
	CheckOutcome outcome = checkSource("int add(int a, int b); float add(int a); int main() { }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "conflicting types"));
}

TEST(sema, redefinition_of_a_function_body_is_an_error)
{
	CheckOutcome outcome = checkSource("int add(int a) { return a; } int add(int a) { return a; } int main() { }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "redefinition of function"));
}

TEST(sema, return_type_mismatch_is_an_error)
{
	CheckOutcome outcome = checkSource("struct P { int x; }; struct P make() { return 1; } int main() { }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "incompatible result type"));
}

TEST(sema, void_function_returning_a_value_is_an_error)
{
	CheckOutcome outcome = checkSource("void f() { return 1; } int main() { }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "should not return a value"));
}

TEST(sema, non_void_function_missing_a_return_value_is_an_error)
{
	CheckOutcome outcome = checkSource("int f() { return; } int main() { }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "should return a value"));
}

// ---- structs -----------------------------------------------------------------------------------------

TEST(sema, struct_member_access_resolves_field_type)
{
	CHECK_EQ(typeOfMainLastExpr("struct P { int x; float y; }; int main() { struct P p; p.y; }"), "float");
}

TEST(sema, struct_arrow_access_resolves_field_type)
{
	CHECK_EQ(typeOfMainLastExpr("struct P { int x; }; int main() { struct P p; struct P* pp; pp = &p; pp->x; }"), "int");
}

TEST(sema, member_access_on_a_missing_field_is_an_error)
{
	CheckOutcome outcome = checkSource("struct P { int x; }; int main() { struct P p; p.y; }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "no member named"));
}

TEST(sema, dot_on_a_non_struct_is_an_error)
{
	CheckOutcome outcome = checkSource("int main() { int x; x.y; }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "is not a struct"));
}

TEST(sema, arrow_on_a_non_pointer_is_an_error)
{
	CheckOutcome outcome = checkSource("struct P { int x; }; int main() { struct P p; p->x; }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "is not a pointer to struct"));
}

TEST(sema, self_referential_struct_via_pointer_is_valid)
{
	CheckOutcome outcome = checkSource("struct Node { int value; struct Node* next; }; int main() { }");
	CHECK(outcome.ok);
}

TEST(sema, self_referential_struct_by_value_is_an_error)
{
	CheckOutcome outcome = checkSource("struct Node { int value; struct Node inner; }; int main() { }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "illegal by-value cycle"));
}

TEST(sema, struct_field_of_void_is_an_error)
{
	CheckOutcome outcome = checkSource("struct P { void x; }; int main() { }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "incomplete type 'void'"));
}

// ---- assignment / lvalues -----------------------------------------------------------------------------

TEST(sema, assignment_to_a_literal_is_an_error)
{
	CheckOutcome outcome = checkSource("int main() { 1 = 2; }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "not assignable"));
}

TEST(sema, assignment_between_compatible_types_is_valid)
{
	CheckOutcome outcome = checkSource("int main() { int x; x = 5; }");
	CHECK(outcome.ok);
}

TEST(sema, assignment_between_incompatible_types_is_an_error)
{
	CheckOutcome outcome = checkSource("struct P { int x; }; int main() { struct P p; int i; i = p; }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "incompatible type"));
}

// ---- control flow --------------------------------------------------------------------------------------

TEST(sema, break_outside_a_loop_or_switch_is_an_error)
{
	CheckOutcome outcome = checkSource("int main() { break; }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "'break' statement not in a loop or switch"));
}

TEST(sema, break_inside_a_while_loop_is_valid)
{
	CheckOutcome outcome = checkSource("int main() { while (1) { break; } }");
	CHECK(outcome.ok);
}

TEST(sema, continue_outside_a_loop_is_an_error)
{
	CheckOutcome outcome = checkSource("int main() { continue; }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "'continue' statement not in a loop"));
}

TEST(sema, case_outside_a_switch_is_an_error)
{
	CheckOutcome outcome = checkSource("int main() { case 1: ; }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "'case' statement not in a switch"));
}

TEST(sema, default_outside_a_switch_is_an_error)
{
	CheckOutcome outcome = checkSource("int main() { default: ; }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "'default' statement not in a switch"));
}

TEST(sema, case_with_a_non_constant_value_is_an_error)
{
	CheckOutcome outcome = checkSource("int main() { int x; switch (x) { case x: ; } }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "does not reduce to an integer constant"));
}

TEST(sema, case_with_an_enum_constant_is_valid)
{
	CheckOutcome outcome = checkSource(
		"enum Color { Red, Green, Blue };"
		"int main() { enum Color c; switch (c) { case Red: ; case Green: ; default: ; } }");
	CHECK(outcome.ok);
}

TEST(sema, switch_on_a_non_integer_condition_is_an_error)
{
	CheckOutcome outcome = checkSource("struct P { int x; }; int main() { struct P p; switch (p) { } }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "is not an integer"));
}

TEST(sema, goto_to_an_existing_label_is_valid)
{
	CheckOutcome outcome = checkSource("int main() { goto end; end: ; }");
	CHECK(outcome.ok);
}

TEST(sema, goto_to_a_missing_label_is_an_error)
{
	CheckOutcome outcome = checkSource("int main() { goto nowhere; }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "use of undeclared label"));
}

TEST(sema, duplicate_label_in_the_same_function_is_an_error)
{
	CheckOutcome outcome = checkSource("int main() { end: ; end: ; }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "redefinition of label"));
}

TEST(sema, if_condition_must_be_scalar)
{
	CheckOutcome outcome = checkSource("struct P { int x; }; int main() { struct P p; if (p) { } }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "arithmetic or pointer type is required"));
}

// ---- constant-expression evaluator (public, used internally for enum values / case labels) ------------

TEST(sema, eval_constant_expr_handles_arithmetic)
{
	support::Arena arena;
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	lexer::Lexer lexer("1 + 2 * 3", testSourceId(), diagnostics, pool);
	parser::Parser parser(lexer, arena, diagnostics);
	ast::Expr* expr = parser.parseExpression();

	sema::Sema sema(arena, diagnostics);
	std::optional<i64> result = sema.evalConstantExpr(expr);
	CHECK(result.has_value());
	CHECK_EQ(*result, 7);
}

TEST(sema, eval_constant_expr_handles_bitwise_and_ternary)
{
	support::Arena arena;
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	lexer::Lexer lexer("1 ? (1 << 2) | 1 : 0", testSourceId(), diagnostics, pool);
	parser::Parser parser(lexer, arena, diagnostics);
	ast::Expr* expr = parser.parseExpression();

	sema::Sema sema(arena, diagnostics);
	std::optional<i64> result = sema.evalConstantExpr(expr);
	CHECK(result.has_value());
	CHECK_EQ(*result, 5);
}

TEST(sema, eval_constant_expr_rejects_an_out_of_range_shift_count)
{
	support::Arena arena;
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	lexer::Lexer lexer("1 << 64", testSourceId(), diagnostics, pool);
	parser::Parser parser(lexer, arena, diagnostics);
	ast::Expr* expr = parser.parseExpression();

	sema::Sema sema(arena, diagnostics);
	std::optional<i64> result = sema.evalConstantExpr(expr);
	CHECK(!result.has_value());
}

TEST(sema, eval_constant_expr_rejects_a_non_constant)
{
	support::Arena arena;
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	lexer::Lexer lexer("x + 1", testSourceId(), diagnostics, pool);
	parser::Parser parser(lexer, arena, diagnostics);
	ast::Expr* expr = parser.parseExpression();

	sema::Sema sema(arena, diagnostics);
	std::optional<i64> result = sema.evalConstantExpr(expr);
	CHECK(!result.has_value());
}

// ---- lvalue-ness -------------------------------------------------------------------------------------

TEST(sema, dot_access_on_a_non_lvalue_struct_result_is_not_assignable)
{
	// `f().x` is not an lvalue when f() returns a struct by value (real C's rule) - only `->`
	// dereferences and is unconditionally an lvalue regardless of its own operand.
	CheckOutcome outcome = checkSource("struct P { int x; }; struct P make(); int main() { make().x = 1; }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "not assignable"));
}

TEST(sema, assigning_to_an_enum_constant_is_an_error)
{
	CheckOutcome outcome = checkSource("enum Color { Red }; int main() { Red = 1; }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "not assignable"));
}

TEST(sema, a_function_name_used_as_a_value_decays_to_a_pointer_to_it)
{
	// This used to be an error - there was no function-pointer type for the name to have. It now
	// has one, and `f` and `&f` mean the same thing, as in C.
	CHECK(checkSource("int f(int x); int main() { int (*p)(int) = f; return p(1); }").ok);
	CHECK(checkSource("int f(int x); int main() { int (*p)(int) = &f; return p(1); }").ok);

	// The NAME's own type is the function type; the decay happens where a value is wanted, which
	// statement position is not. That is C's model rather than an accident of where the conversion
	// is written - `sizeof f` and `&f` both need to see what it really is.
	CHECK_EQ(typeOfMainLastExpr("int f(int x); int main() { f; }"), "int (int)");
}

TEST(sema, a_function_pointer_only_converts_to_one_of_the_same_signature)
{
	// The qualifier walk that guards ordinary pointer conversions would wave any of these through,
	// because a function type is never const or volatile. Calling through a mismatched signature is
	// not a portability nicety here - it is the wrong arguments in the wrong registers.
	CHECK(checkSource("int f(int x); int main() { int (*p)(int) = f; return 0; }").ok);
	CHECK(!checkSource("int f(int x); int main() { int (*p)(void) = f; return 0; }").ok);
	CHECK(!checkSource("int f(int x); int main() { float (*p)(int) = f; return 0; }").ok);
	CHECK(!checkSource("int f(int x); int main() { int* p = f; return 0; }").ok);

	// A null pointer constant still assigns, the same way it does for any other pointer.
	CHECK(checkSource("int main() { int (*p)(int) = 0; return 0; }").ok);
}

TEST(sema, a_call_through_a_pointer_is_checked_against_the_signature_it_carries)
{
	CHECK(checkSource("int main() { int (*p)(int) = 0; return p(1); }").ok);

	CheckOutcome arity = checkSource("int main() { int (*p)(int) = 0; return p(1, 2); }");
	CHECK(!arity.ok);
	CHECK(containsMessage(arity, "expects 1 argument(s), got 2"));

	CheckOutcome argType = checkSource("struct S { int a; }; int main() { struct S s; int (*p)(int) = 0; return p(s); }");
	CHECK(!argType.ok);
	CHECK(containsMessage(argType, "incompatible type"));

	// Something that is neither a function nor a pointer to one.
	CheckOutcome notCallable = checkSource("int main() { int x = 0; return x(1); }");
	CHECK(!notCallable.ok);
	CHECK(containsMessage(notCallable, "is not a function"));
}

// ---- pointer/null comparisons -------------------------------------------------------------------------

TEST(sema, pointer_compared_against_the_integer_constant_zero_is_valid)
{
	CheckOutcome outcome = checkSource("int main() { int* p; p == 0; p != 0; 0 == p; }");
	CHECK(outcome.ok);
}

TEST(sema, pointer_compared_against_a_nonzero_integer_constant_is_still_an_error)
{
	CheckOutcome outcome = checkSource("int main() { int* p; p == 5; }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "comparison of incompatible operand types"));
}

// ---- switch label uniqueness --------------------------------------------------------------------------

TEST(sema, duplicate_case_value_is_an_error)
{
	CheckOutcome outcome = checkSource("int main() { int x; switch (x) { case 1: ; case 1: ; } }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "duplicate case value"));
}

TEST(sema, duplicate_default_label_is_an_error)
{
	CheckOutcome outcome = checkSource("int main() { int x; switch (x) { default: ; default: ; } }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "multiple default labels"));
}

// ---- declaration scoping -------------------------------------------------------------------------------

TEST(sema, self_referencing_initializer_resolves_to_the_new_declaration)
{
	// In C the declarator's own scope starts before its initializer, so `int x = x;` names the
	// new (uninitialized) `x`, not an outer one - it must not report "undeclared identifier".
	CheckOutcome outcome = checkSource("int main() { int x = x; }");
	CHECK(outcome.ok);
	CHECK(!containsMessage(outcome, "undeclared"));
}

TEST(sema, redeclaring_a_parameter_in_the_function_bodys_outer_block_is_an_error)
{
	// The parameter list and the body's outermost block share one scope in C - `int x;` inside
	// the body must conflict with parameter `x`, not merely shadow it.
	CheckOutcome outcome = checkSource("void f(int x) { int x; } int main() { }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "redefinition of 'x'"));
}

// ---- incomplete-type struct fields ----------------------------------------------------------------------

TEST(sema, struct_field_of_an_incomplete_enum_is_an_error)
{
	CheckOutcome outcome = checkSource("struct P { enum Color c; }; int main() { }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "incomplete type 'enum"));
}

TEST(sema, array_of_self_by_value_is_caught_through_the_array_element_type)
{
	// Builds the StructDecl/Type directly rather than through source text, to exercise
	// validateStructLayout() in isolation from the parser/array-declarator plumbing this same shape
	// could now also reach through `struct Node { struct Node children[2]; };` (see parser.cpp's
	// applyDeclarator()): it is exactly as illegal as the by-value (non-array) case,
	// since an array stores its elements inline, same as a plain by-value field.
	support::Arena arena;
	support::DiagnosticEngine diagnostics;
	support::SourceLocation loc{};

	ast::StructDecl* node = arena.create<ast::StructDecl>(loc, "Node");
	const ast::Type* nodeArrayType = ast::Type::makeArray(arena, ast::Type::makeStruct(arena, node), 2);
	ast::FieldDecl fields[] = { { nodeArrayType, "children", loc } };
	node->setFields(fields);

	CHECK(!sema::validateStructLayout(diagnostics, *node));

	bool foundCycleMessage = false;
	for (const support::Diagnostic& diagnostic : diagnostics.diagnostics())
	{
		if (diagnostic.message.find("illegal by-value cycle") != std::string::npos)
			foundCycleMessage = true;
	}
	CHECK(foundCycleMessage);
}

// ---- arrays/pointers, now that the parser can actually produce array declarators -------------------
//
// Array-to-pointer decay (Sema::decayArray(), sema.cpp): an array's VALUE - passed as an argument,
// assigned from, added to an integer, returned - is really the address of its first element, matching
// real C. The array's own annotated Type stays the real Array type throughout (see
// array_type_is_preserved_on_the_expressions_own_annotation_despite_decay below); only the local copy
// used by the specific check that needs an rvalue is decayed.

TEST(sema, array_variable_can_be_declared_indexed_and_assigned_through)
{
	CheckOutcome outcome = checkSource("int main() { int arr[3]; arr[0] = 1; return arr[0]; }");
	CHECK(outcome.ok);
}

TEST(sema, array_decays_to_pointer_when_passed_as_a_function_argument)
{
	CheckOutcome outcome = checkSource("void f(int* p) { } int main() { int arr[4]; f(arr); }");
	CHECK(outcome.ok);
}

TEST(sema, array_type_is_preserved_on_the_expressions_own_annotation_despite_decay)
{
	// Decay only affects the local copy isAssignable()/etc. check against - the expression itself
	// keeps annotating `arr` with its real Array type (needed by sizeof and IrBuilder alike).
	CHECK_EQ(typeOfMainLastExpr("int main() { int arr[3]; arr; }"), "int[3]");
	CHECK_EQ(typeOfMainLastExpr("int main() { int arr[3]; &arr; }"), "int[3]*");
}

TEST(sema, arrays_decay_in_conditions_and_ternary_operands)
{
	CHECK(checkSource("int main() { int arr[3]; int* p; if (arr) p = true ? arr : p; }").ok);
}

TEST(sema, assigning_a_whole_array_from_another_array_is_an_error)
{
	CheckOutcome outcome = checkSource("int main() { int a[3]; int b[3]; a = b; }");
	CHECK(!outcome.ok);
}

TEST(sema, array_plus_integer_is_pointer_arithmetic)
{
	CHECK_EQ(typeOfMainLastExpr("int main() { int arr[4]; arr + 1; }"), "int*");
}

TEST(sema, two_dimensional_array_indexes_down_to_a_row_then_an_element)
{
	CheckOutcome outcome = checkSource("int main() { int m[3][4]; m[1][2] = 5; return m[1][2]; }");
	CHECK(outcome.ok);
	CHECK_EQ(typeOfMainLastExpr("int main() { int m[3][4]; m[1]; }"), "int[4]");
}

TEST(sema, a_pointer_can_be_initialized_from_an_array)
{
	CheckOutcome outcome = checkSource("int main() { int arr[4]; int* p = arr; return *p; }");
	CHECK(outcome.ok);
}

TEST(sema, initializing_an_array_with_a_scalar_expression_is_an_error)
{
	CheckOutcome outcome = checkSource("int main() { int arr[3] = 5; }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "must be initialized with an initializer list or a string literal"));
}

TEST(sema, a_function_returning_pointer_can_return_a_decayed_local_array)
{
	// No escape/lifetime analysis in this subset (§0/§14 of the architecture plan) - only that the
	// TYPES agree, same as every other pointer-returning function.
	CheckOutcome outcome = checkSource("int* f() { int arr[4]; return arr; }");
	CHECK(outcome.ok);
}

TEST(sema, struct_field_that_is_an_array_is_indexable_through_the_real_parser)
{
	// Same shape as array_of_self_by_value_is_caught_through_the_array_element_type above, but built
	// through ordinary source text now that the parser has array-declarator support (parser.cpp's
	// applyDeclarator()) instead of constructing the StructDecl/Type by hand.
	CheckOutcome outcome = checkSource(
		"struct S { int coords[3]; }; int main() { struct S s; s.coords[0] = 1; return s.coords[0]; }");
	CHECK(outcome.ok);
}

// ---- error-recovery type annotations (regression: a failed type check must fall back to int, ---
// ---- not leak the offending operand's own type onto the node) ---------------------------------------

TEST(sema, binary_expr_with_invalid_operand_types_recovers_to_int_not_the_operand_type)
{
	CheckOutcome outcome = checkSource("struct P { int x; }; int main() { struct P p; p & p; }");
	CHECK(!outcome.ok);
	CHECK_EQ(typeOfMainLastExpr("struct P { int x; }; int main() { struct P p; p & p; }", false), "int");
}

TEST(sema, unary_negate_and_bitwise_not_with_invalid_operand_recover_to_int)
{
	CHECK_EQ(typeOfMainLastExpr("struct P { int x; }; int main() { struct P p; -p; }", false), "int");
	CHECK_EQ(typeOfMainLastExpr("struct P { int x; }; int main() { struct P p; ~p; }", false), "int");
}

TEST(sema, function_colliding_with_a_variable_name_stays_callable_after_the_conflict_error)
{
	// Regression: the mismatched-kind recovery path used to silently drop the function from the
	// symbol table, so a later call produced a second, more confusing "is not a function" error
	// on top of the correct "redefinition ... as a different kind of symbol" one.
	CheckOutcome outcome = checkSource("int foo; int foo(int x) { return x; } int main() { foo(1); }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "redefinition of 'foo' as a different kind of symbol"));
	CHECK(!containsMessage(outcome, "is not a function"));
}

// ---- whole-program integration ---------------------------------------------------------------------------

TEST(sema, a_well_formed_program_using_every_Fase4_feature_checks_cleanly)
{
	CheckOutcome outcome = checkSource(
		"struct Point { int x; int y; };"
		"enum Direction { North, East, South, West };"
		"typedef struct Point Vec2;"
		""
		"int distanceSquared(struct Point a, struct Point b)"
		"{"
		"    int dx = a.x - b.x;"
		"    int dy = a.y - b.y;"
		"    return dx * dx + dy * dy;"
		"}"
		""
		"int main()"
		"{"
		"    struct Point origin;"
		"    origin.x = 0;"
		"    origin.y = 0;"
		""
		"    enum Direction facing = North;"
		"    switch (facing)"
		"    {"
		"        case North: facing = East; break;"
		"        case East: facing = South; break;"
		"        default: goto done;"
		"    }"
		""
		"    for (int i = 0; i < 10; i = i + 1)"
		"    {"
		"        if (i == 5)"
		"            continue;"
		"        if (i == 9)"
		"            break;"
		"    }"
		""
		"done:"
		"    return distanceSquared(origin, origin);"
		"}");
	CHECK(outcome.ok);
}

// ---- initializers (Fase 7) -----------------------------------------------------------------------

TEST(sema, an_array_initializer_list_of_the_right_length_type_checks)
{
	CheckOutcome outcome = checkSource("int a[3] = { 1, 2, 3 };");
	CHECK(outcome.ok);
}

TEST(sema, an_array_initializer_list_may_be_shorter_than_the_array)
{
	// The rest zero-fills, as in C - so a short list is not an error, only a long one is.
	CheckOutcome outcome = checkSource("int a[4] = { 1 };");
	CHECK(outcome.ok);
}

TEST(sema, too_many_values_in_an_array_initializer_is_an_error)
{
	CheckOutcome outcome = checkSource("int a[2] = { 1, 2, 3 };");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "which holds 2"));
}

TEST(sema, a_nested_array_initializer_checks_each_row_against_the_row_type)
{
	CheckOutcome outcome = checkSource("int m[2][3] = { { 1, 2, 3 }, { 4, 5, 6 } };");
	CHECK(outcome.ok);
}

TEST(sema, too_many_values_in_a_row_of_a_nested_array_initializer_is_an_error)
{
	CheckOutcome outcome = checkSource("int m[2][2] = { { 1, 2, 3 }, { 4, 5 } };");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "which holds 2"));
}

TEST(sema, brace_elision_in_a_nested_array_initializer_is_reported_explicitly)
{
	// `int m[2][2] = { 1, 2, 3, 4 }` is valid C and deliberately rejected here - see
	// Sema::checkInitializer()'s own note. The diagnostic has to say WHY, not just "bad type".
	CheckOutcome outcome = checkSource("int m[2][2] = { 1, 2, 3, 4 };");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "omitting the inner braces"));
}

TEST(sema, a_struct_initializer_list_maps_values_to_fields_positionally)
{
	CheckOutcome outcome = checkSource(
		"struct P { int x; int y; };"
		"struct P p = { 1, 2 };");
	CHECK(outcome.ok);
}

TEST(sema, a_struct_initializer_list_may_leave_later_fields_out)
{
	CheckOutcome outcome = checkSource(
		"struct P { int x; int y; int z; };"
		"struct P p = { 1 };");
	CHECK(outcome.ok);
}

TEST(sema, too_many_values_in_a_struct_initializer_is_an_error)
{
	CheckOutcome outcome = checkSource(
		"struct P { int x; int y; };"
		"struct P p = { 1, 2, 3 };");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "which has 2 field(s)"));
}

TEST(sema, a_struct_field_of_the_wrong_type_in_an_initializer_is_an_error)
{
	// Checked field by field, against that field's own type - so a value that could not be assigned
	// to the field is reported here too, at the element rather than at the whole declaration.
	CheckOutcome outcome = checkSource(
		"struct Q { int a; };"
		"struct P { int x; };"
		"struct Q q;"
		"struct P p = { q };");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "incompatible type"));
}

TEST(sema, brace_elision_for_a_struct_typed_field_is_reported_explicitly)
{
	CheckOutcome outcome = checkSource(
		"struct Inner { int a; int b; };"
		"struct Outer { struct Inner in; int c; };"
		"struct Outer o = { 1, 2, 3 };");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "omitting the inner braces"));
}

TEST(sema, a_nested_struct_field_with_its_own_braces_type_checks)
{
	CheckOutcome outcome = checkSource(
		"struct Inner { int a; int b; };"
		"struct Outer { struct Inner in; int c; };"
		"struct Outer o = { { 1, 2 }, 3 };");
	CHECK(outcome.ok);
}

TEST(sema, a_string_literal_initializes_a_char_array)
{
	CheckOutcome outcome = checkSource("char s[8] = \"hola\";");
	CHECK(outcome.ok);
}

TEST(sema, a_string_literal_that_does_not_fit_its_char_array_is_an_error)
{
	// Four characters plus the terminating zero need five bytes - the message says so, because the
	// off-by-one is the whole point of getting this wrong.
	CheckOutcome outcome = checkSource("char s[4] = \"hola\";");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "5 byte(s) including its terminating zero"));
}

TEST(sema, a_string_literal_cannot_initialize_an_array_of_a_wider_element)
{
	CheckOutcome outcome = checkSource("int a[8] = \"hola\";");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "requires an array of 'char'"));
}

TEST(sema, a_string_literal_fills_one_row_of_a_two_dimensional_char_array)
{
	CheckOutcome outcome = checkSource("char names[2][8] = { \"ada\", \"grace\" };");
	CHECK(outcome.ok);
}

TEST(sema, union_members_overlay_and_alignof_is_unsigned)
{
	CHECK(checkSource("union Value { char byte; int word; }; int main(void) { union Value value; value.word = 7; return value.byte + alignof(union Value); }").ok);
	CHECK_EQ(typeOfMainLastExpr("union Value { char byte; int word; }; int main(void) { union Value value; value.word = 7; value.byte + alignof(union Value); }"), "unsigned int");
}

TEST(sema, restrict_requires_a_pointer_and_register_has_no_address)
{
	CheckOutcome restrictOutcome = checkSource("restrict int value;");
	CHECK(!restrictOutcome.ok);
	CHECK(containsMessage(restrictOutcome, "requires a pointer"));
	CheckOutcome registerOutcome = checkSource("int main(void) { register int value; int* p = &value; return 0; }");
	CHECK(!registerOutcome.ok);
	CHECK(containsMessage(registerOutcome, "address of register"));
}

TEST(sema, a_braced_string_literal_initializes_a_char_array)
{
	CheckOutcome outcome = checkSource("char s[8] = { \"hola\" };");
	CHECK(outcome.ok);
}

TEST(sema, a_nested_braced_string_literal_initializes_a_char_array_row)
{
	CheckOutcome outcome = checkSource("char names[2][8] = { { \"ada\" }, { \"grace\" } };");
	CHECK(outcome.ok);
}

TEST(sema, a_struct_value_is_valid_as_an_aggregate_element)
{
	CheckOutcome outcome = checkSource(
		"struct Inner { int a; int b; };"
		"struct Outer { struct Inner in; int c; };"
		"struct Inner inner;"
		"struct Outer o = { inner, 3 };");
	CHECK(outcome.ok);
}

TEST(sema, compound_assignment_to_a_struct_is_an_error)
{
	CheckOutcome outcome = checkSource(
		"struct P { int x; };"
		"int main() { struct P a; struct P b; a += b; return 0; }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "compound assignment is not valid for struct"));
}

TEST(sema, a_scalar_accepts_a_single_braced_value)
{
	CheckOutcome outcome = checkSource("int x = { 5 };");
	CHECK(outcome.ok);
}

TEST(sema, a_scalar_with_more_than_one_braced_value_is_an_error)
{
	CheckOutcome outcome = checkSource("int x = { 5, 6 };");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "takes exactly one value"));
}

TEST(sema, an_array_initializer_list_works_on_a_local_too)
{
	CheckOutcome outcome = checkSource("int main() { int a[2] = { 1, 2 }; return a[0]; }");
	CHECK(outcome.ok);
}

// ---- structs by value (Fase 7) -------------------------------------------------------------------

TEST(sema, assigning_one_struct_to_another_of_the_same_type_type_checks)
{
	CheckOutcome outcome = checkSource(
		"struct P { int x; int y; };"
		"int main() { struct P a; struct P b; a.x = 1; b = a; return b.x; }");
	CHECK(outcome.ok);
}

TEST(sema, assigning_between_two_different_struct_types_is_an_error)
{
	CheckOutcome outcome = checkSource(
		"struct P { int x; };"
		"struct Q { int x; };"
		"int main() { struct P p; struct Q q; p = q; return 0; }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "incompatible"));
}

TEST(sema, a_function_can_take_and_return_a_struct_by_value)
{
	CheckOutcome outcome = checkSource(
		"struct P { int x; int y; };"
		"struct P doubled(struct P p) { struct P r; r.x = p.x * 2; r.y = p.y * 2; return r; }"
		"int main() { struct P a; a.x = 1; a.y = 2; struct P b = doubled(a); return b.x + b.y; }");
	CHECK(outcome.ok);
}

TEST(sema, passing_the_wrong_struct_type_by_value_is_still_an_error)
{
	CheckOutcome outcome = checkSource(
		"struct P { int x; };"
		"struct Q { int x; };"
		"int f(struct P p);"
		"int main() { struct Q q; return f(q); }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "incompatible type"));
}

TEST(sema, a_member_access_on_a_struct_returning_call_type_checks_but_is_not_an_lvalue)
{
	CheckOutcome reading = checkSource(
		"struct P { int x; };"
		"struct P make();"
		"int main() { return make().x; }");
	CHECK(reading.ok);

	CheckOutcome writing = checkSource(
		"struct P { int x; };"
		"struct P make();"
		"int main() { make().x = 1; return 0; }");
	CHECK(!writing.ok);
}

// ---- const -------------------------------------------------------------------------------------

TEST(sema, a_const_object_cannot_be_assigned_to)
{
	CheckOutcome outcome = checkSource("int main() { const int limit = 3; limit = 4; return 0; }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "it is const"));
}

TEST(sema, a_const_object_cannot_be_incremented_or_compound_assigned)
{
	CHECK(!checkSource("int main() { const int n = 1; n++; return 0; }").ok);
	CHECK(!checkSource("int main() { const int n = 1; --n; return 0; }").ok);
	CHECK(!checkSource("int main() { const int n = 1; n += 2; return 0; }").ok);
}

TEST(sema, a_const_global_cannot_be_assigned_to_either)
{
	CheckOutcome outcome = checkSource("const int limit = 3;\nint main() { limit = 4; return 0; }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "it is const"));
}

TEST(sema, const_may_be_written_on_either_side_of_the_type)
{
	// `const int` and `int const` are the same type in C, and both have to reach the same place.
	CHECK(!checkSource("int main() { int const n = 1; n = 2; return 0; }").ok);
	CHECK(checkSource("int main() { int const n = 1; return n; }").ok);
}

TEST(sema, a_pointer_to_const_and_a_const_pointer_are_different_types)
{
	// Which side of the star the word is on is the whole difference: one forbids writing through
	// the pointer, the other forbids changing where it points.
	CHECK(!checkSource("int main() { int n = 0; const int* p = &n; *p = 1; return 0; }").ok);
	CHECK(checkSource("int main() { int n = 0; const int* p = &n; p = 0; return *p; }").ok);

	CHECK(checkSource("int main() { int n = 0; int* const p = &n; *p = 1; return 0; }").ok);
	CHECK(!checkSource("int main() { int n = 0; int* const p = &n; p = 0; return 0; }").ok);
}

TEST(sema, a_conversion_may_add_const_but_never_drop_it)
{
	CHECK(checkSource("int main() { int n = 0; int* w = &n; const int* r = w; return *r; }").ok);

	CheckOutcome dropping = checkSource("int main() { int n = 0; const int* r = &n; int* w = r; return *w; }");
	CHECK(!dropping.ok);
	CHECK(containsMessage(dropping, "incompatible type"));

	CheckOutcome nestedDropping = checkSource("int main() { int n = 0; const int* p = &n; const int** pp = &p; int** q = pp; return 0; }");
	CHECK(!nestedDropping.ok);
	CHECK(containsMessage(nestedDropping, "incompatible type"));
}

TEST(sema, a_member_of_a_const_struct_cannot_be_written)
{
	CheckOutcome outcome = checkSource("struct S { int x; }; const struct S g = { 1 }; int main() { g.x = 2; return 0; }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "const"));
}

TEST(sema, a_const_parameter_is_accepted_and_still_cannot_be_written)
{
	CHECK(checkSource("int length(const char* text) { int n = 0; while (text[n] != 0) n++; return n; }").ok);
	CHECK(!checkSource("void f(const int n) { n = 1; }").ok);
}

// ---- storage classes ---------------------------------------------------------------------------

TEST(sema, the_storage_classes_parse_and_type_check)
{
	CHECK(checkSource("static int counter = 0;\nint main() { return counter; }").ok);
	CHECK(checkSource("extern int elsewhere;\nint main() { return elsewhere; }").ok);
	CHECK(checkSource("int main() { auto int n = 1; return n; }").ok);
	CHECK(checkSource("static inline int twice(int n) { return n * 2; }\nint main() { return twice(2); }").ok);
}

TEST(sema, auto_is_only_allowed_inside_a_block)
{
	CheckOutcome outcome = checkSource("auto int n = 1;\nint main() { return n; }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "'auto'"));
}

TEST(sema, two_storage_classes_on_one_declaration_say_which_two)
{
	CheckOutcome outcome = checkSource("static extern int n;\nint main() { return 0; }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "cannot combine"));
}

TEST(sema, inline_is_only_allowed_on_a_function)
{
	CheckOutcome outcome = checkSource("inline int n = 1;\nint main() { return n; }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "'inline'"));
}

TEST(sema, a_static_local_needs_a_compile_time_initializer)
{
	// Its initial value becomes bytes in the loaded image, so there is no moment at which a
	// run-time expression could be evaluated for it.
	CheckOutcome expression = checkSource("int main() { static int n = 1 + 2; return n; }");
	CHECK(!expression.ok);
	CHECK(containsMessage(expression, "compile-time constant"));

	CheckOutcome outcome = checkSource("int seed = 4;\nint main() { static int n = seed; return n; }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "compile-time constant"));
}

TEST(sema, an_extern_declaration_in_a_block_cannot_have_an_initializer)
{
	CheckOutcome outcome = checkSource("int main() { extern int n = 1; return n; }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "'extern'"));
}

TEST(sema, an_extern_declaration_followed_by_the_definition_is_one_object)
{
	// Exactly what a header's `extern` plus the source file's definition looks like after the
	// preprocessor has run them together - and the reason headers work at all.
	CHECK(checkSource("extern int counter;\nint counter = 0;\nint main() { return counter; }").ok);
	CHECK(checkSource("int counter = 0;\nextern int counter;\nint main() { return counter; }").ok);
}

TEST(sema, two_definitions_of_the_same_global_are_still_a_redefinition)
{
	CheckOutcome outcome = checkSource("int counter = 0;\nint counter = 1;\nint main() { return counter; }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "redefinition"));
}

TEST(sema, redeclaring_a_global_with_a_different_type_is_an_error)
{
	CheckOutcome outcome = checkSource("extern int counter;\nfloat counter = 0.0;\nint main() { return 0; }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "different type"));
}

TEST(sema, redeclaring_a_global_with_conflicting_linkage_is_an_error)
{
	CheckOutcome outcome = checkSource("extern int counter;\nstatic int counter = 0;\nint main() { return counter; }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "conflicting linkage"));
}

TEST(sema, a_local_is_still_not_redeclarable)
{
	CheckOutcome outcome = checkSource("int main() { int n = 1; int n = 2; return n; }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "redefinition"));
}

// ---- variadic functions ----------------------------------------------------------------------

TEST(sema, a_variadic_call_needs_its_fixed_arguments_and_accepts_any_number_beyond_them)
{
	CHECK(checkSource("int f(int a, int b, ...); int main(void) { return f(1, 2) + f(1, 2, 3) + f(1, 2, 3, 4); }").ok);

	CheckOutcome tooFew = checkSource("int f(int a, int b, ...); int main(void) { return f(1); }");
	CHECK(!tooFew.ok);
	CHECK(containsMessage(tooFew, "expects at least 2 argument"));
}

TEST(sema, the_fixed_parameters_of_a_variadic_call_are_still_type_checked)
{
	CheckOutcome outcome = checkSource(
		"struct S { int x; };"
		"int f(int a, ...);"
		"int main(void) { struct S s; return f(s, 1); }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "incompatible type"));
}

TEST(sema, an_aggregate_cannot_travel_through_an_ellipsis)
{
	// It would be passed as a hidden pointer to a caller-owned copy, and nothing tells the callee
	// how big that copy is - so __builtin_va_arg could never read it back.
	CheckOutcome outcome = checkSource(
		"struct S { int x; int y; };"
		"int f(int a, ...);"
		"int main(void) { struct S s; return f(1, s); }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "no variadic representation"));
}

TEST(sema, va_start_is_only_allowed_in_a_variadic_function_and_must_name_the_last_parameter)
{
	CHECK(checkSource("int f(int a, ...) { __builtin_va_list ap; __builtin_va_start(ap, a); __builtin_va_end(ap); return 0; }").ok);

	CheckOutcome notVariadic = checkSource("int f(int a) { __builtin_va_list ap; __builtin_va_start(ap, a); return 0; }");
	CHECK(!notVariadic.ok);
	CHECK(containsMessage(notVariadic, "only allowed inside a function declared with"));

	CheckOutcome wrongParam = checkSource("int f(int a, int b, ...) { __builtin_va_list ap; __builtin_va_start(ap, a); return 0; }");
	CHECK(!wrongParam.ok);
	CHECK(containsMessage(wrongParam, "must name the last named parameter"));
}

TEST(sema, va_arg_reads_only_a_four_byte_scalar)
{
	CHECK(checkSource("int f(int a, ...) { __builtin_va_list ap; __builtin_va_start(ap, a); return __builtin_va_arg(ap, int); }").ok);

	// A `char` was never passed: the default argument promotions mean an `int` was, so reading one
	// back as `char` would decode a word that does not hold what was asked for.
	CheckOutcome narrow = checkSource("int f(int a, ...) { __builtin_va_list ap; __builtin_va_start(ap, a); return __builtin_va_arg(ap, char); }");
	CHECK(!narrow.ok);
	CHECK(containsMessage(narrow, "promoted to a 4-byte type"));

	CheckOutcome aggregate = checkSource(
		"struct S { int x; int y; };"
		"int f(int a, ...) { __builtin_va_list ap; __builtin_va_start(ap, a); struct S s; s = __builtin_va_arg(ap, struct S); return s.x; }");
	CHECK(!aggregate.ok);
	CHECK(containsMessage(aggregate, "only scalar types are passed through"));
}

TEST(sema, a_va_list_operand_must_actually_be_a_va_list)
{
	CheckOutcome outcome = checkSource("int f(int a, ...) { int ap; __builtin_va_start(ap, a); return 0; }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "must have type '__builtin_va_list'"));
}

TEST(sema, a_prototype_and_a_definition_must_agree_about_the_ellipsis)
{
	CheckOutcome outcome = checkSource("int f(int a); int f(int a, ...) { return a; }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "conflicting types"));
}

TEST(sema, a_va_list_may_be_passed_to_another_function)
{
	// The vprintf pattern: the worker is not itself variadic, it just consumes a cursor it was
	// handed - which works because __builtin_va_list is an ordinary pointer.
	CHECK(checkSource(
		"int worker(__builtin_va_list ap) { return __builtin_va_arg(ap, int); }"
		"int f(int a, ...) { __builtin_va_list ap; __builtin_va_start(ap, a); int r = worker(ap); __builtin_va_end(ap); return r; }").ok);
}

TEST(sema, register_is_only_allowed_on_a_variable_inside_a_block)
{
	CHECK(checkSource("int main(void) { register int cached = 1; return cached; }").ok);

	// A file-scope object has static storage duration whatever it asks for, so `register` there is
	// the same kind of error `auto` is.
	CheckOutcome atFileScope = checkSource("register int counter;");
	CHECK(!atFileScope.ok);
	CHECK(containsMessage(atFileScope, "only allowed on a variable declared inside a block"));
}

TEST(sema, a_qualified_struct_hands_each_qualifier_to_its_members_independently)
{
	// `const` reaching a member is what makes `p->a = 1` an error through a `const struct R*`, and
	// it kept working while `volatile` did not travel at all. Both now compose, so a
	// `const volatile` object hands its fields both.
	CHECK(!checkSource("struct R { int a; }; int f(const struct R* p) { p->a = 1; return 0; }").ok);
	CHECK(checkSource("struct R { int a; }; int f(volatile struct R* p) { p->a = 1; return 0; }").ok);
	CHECK(!checkSource("struct R { int a; }; int f(const volatile struct R* p) { p->a = 1; return 0; }").ok);
}

TEST(sema, a_pointer_conversion_may_not_discard_volatile)
{
	// The same rule `const` already had, for the same reason in a different currency: the alias
	// would lose the guarantee, and the optimizer is then free over accesses the program needed
	// kept. Adding a qualifier stays fine - promising more about an object than you have to never
	// is - and an explicit cast is still the deliberate way out, exactly as in C.
	CHECK(!checkSource("volatile int v; int f(void) { volatile int* q = &v; int* p = q; return *p; }").ok);
	CHECK(checkSource("int w; int f(void) { int* p = &w; volatile int* q = p; return *q; }").ok);
	CHECK(checkSource("int f(void) { volatile int* q = 0; int* p = (int*)q; return *p; }").ok);
}

TEST(sema, register_is_rejected_on_an_array)
{
	// An array decays to a pointer the moment it is used for anything but `sizeof`, and that decay
	// IS taking its address - so every use of a `register` array breaks the keyword's one promise.
	// The `&` check cannot see it, because no `&` is written anywhere.
	CheckOutcome outcome = checkSource("int f(void) { register int a[3]; a[0] = 1; return a[0]; }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "using an array takes its address"));

	CHECK(checkSource("int f(void) { register int x = 1; return x; }").ok);
}

TEST(sema, a_register_parameter_has_no_address_either)
{
	// The keyword's one promise does not depend on where the object came from. A parameter has no
	// Decl node of its own, so the answer travels on its Symbol (symbol_table.h).
	CheckOutcome outcome = checkSource("int f(register int a) { int* p = &a; return *p; }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "address of register"));

	CHECK(checkSource("int f(register int a) { return a + 1; }").ok);

	// An array parameter is a pointer - the decay happens in the declaration, so there is no array
	// left for  to be wrong about.
	CHECK(checkSource("int f(register int a[4]) { return a[0]; }").ok);
}

// ---- interrupt handlers ------------------------------------------------------------------------

TEST(sema, an_interrupt_handler_has_no_caller_and_the_rules_all_follow_from_that)
{
	CHECK(checkSource("__interrupt void h(void) { }").ok);

	// Nothing is there to receive a result...
	CheckOutcome returns = checkSource("__interrupt int h(void) { return 0; }");
	CHECK(!returns.ok);
	CHECK(containsMessage(returns, "must return 'void'"));

	// ...nor to pass an argument.
	CheckOutcome takes = checkSource("__interrupt void h(int x) { }");
	CHECK(!takes.ok);
	CHECK(containsMessage(takes, "takes no parameters"));

	// The vector is the only way in: reached by `call`, its `iret` would pop the return address as
	// a PC and whatever sat below it as flags.
	CheckOutcome called = checkSource("__interrupt void h(void) { } int main(void) { h(); return 0; }");
	CHECK(!called.ok);
	CHECK(containsMessage(called, "cannot be called"));

	// `main` is the reset vector, found by name, and does not return to anything either.
	CheckOutcome entry = checkSource("__interrupt void main(void) { }");
	CHECK(!entry.ok);
	CHECK(containsMessage(entry, "'main' cannot be an '__interrupt' handler"));
}

TEST(sema, an_interrupt_vector_binding_enforces_the_linkers_own_four_rules)
{
	std::string_view handler = "__interrupt void h(void) { } ";

	CHECK(checkSource(std::string(handler) + "__interrupt_vector(17, h);").ok);
	// An enum constant folds, so the vector can be named rather than spelled as a bare number.
	CHECK(checkSource("enum Irq { Terminal = 17 }; " + std::string(handler) + "__interrupt_vector(Terminal, h);").ok);

	// 1. The number must fold to a constant.
	CheckOutcome variable = checkSource("int n; " + std::string(handler) + "__interrupt_vector(n, h);");
	CHECK(!variable.ok);
	CHECK(containsMessage(variable, "must be a constant expression"));

	// 2. 0 is the reset vector, and 63 is the end of the table.
	CHECK(containsMessage(checkSource(std::string(handler) + "__interrupt_vector(0, h);"), "reset vector"));
	CHECK(containsMessage(checkSource(std::string(handler) + "__interrupt_vector(64, h);"), "out of range"));

	// 3. The target must be an `__interrupt` handler - an ordinary function ends in `ret` and would
	//    pop the flags the dispatcher pushed as a return address.
	CheckOutcome ordinary = checkSource("void g(void) { } __interrupt_vector(17, g);");
	CHECK(!ordinary.ok);
	CHECK(containsMessage(ordinary, "is not declared '__interrupt'"));

	// 4. One binding per number. Across objects that is the linker's to catch; inside one file the
	//    message can name the first binding.
	CheckOutcome twice = checkSource(std::string(handler) +
		"__interrupt void k(void) { } __interrupt_vector(17, h); __interrupt_vector(17, k);");
	CHECK(!twice.ok);
	CHECK(containsMessage(twice, "already bound to 'h'"));
}
