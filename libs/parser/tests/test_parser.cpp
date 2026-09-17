#include <ceresc/parser/parser.h>
#include <ceresc/ast/ast_printer.h>
#include <ceresc/lexer/lexer.h>
#include <ceresc/support/arena.h>
#include <ceresc/support/diagnostics.h>
#include <ceresc/support/string_pool.h>

#include "framework.h"

using namespace ceresc;
using namespace ceresc::parser;

namespace
{
	support::SourceId testSourceId() { return support::SourceId::make(1); }

	// Parses `source` as a single expression and returns its printed form (see ast_printer.h) -
	// same trick test_lexer.cpp uses for tokens, since neither Expr nor Type has a std::formatter.
	std::string printExpr(std::string_view source)
	{
		support::Arena arena;
		support::DiagnosticEngine diagnostics;
		support::StringPool pool;
		lexer::Lexer lexer(source, testSourceId(), diagnostics, pool);
		Parser parser(lexer, arena, diagnostics);

		ast::Expr* expr = parser.parseExpression();
		ast::AstPrinter printer;
		return printer.print(expr);
	}

	// Same idea, for a single statement / a single external declaration / a whole translation unit.
	std::string printStmt(std::string_view source)
	{
		support::Arena arena;
		support::DiagnosticEngine diagnostics;
		support::StringPool pool;
		lexer::Lexer lexer(source, testSourceId(), diagnostics, pool);
		Parser parser(lexer, arena, diagnostics);

		ast::Stmt* stmt = parser.parseStatement();
		ast::AstPrinter printer;
		return printer.print(stmt);
	}

	std::string printDecl(std::string_view source)
	{
		support::Arena arena;
		support::DiagnosticEngine diagnostics;
		support::StringPool pool;
		lexer::Lexer lexer(source, testSourceId(), diagnostics, pool);
		Parser parser(lexer, arena, diagnostics);

		ast::Decl* decl = parser.parseExternalDecl();
		ast::AstPrinter printer;
		return printer.print(decl);
	}

	std::string printUnit(std::string_view source)
	{
		support::Arena arena;
		support::DiagnosticEngine diagnostics;
		support::StringPool pool;
		lexer::Lexer lexer(source, testSourceId(), diagnostics, pool);
		Parser parser(lexer, arena, diagnostics);

		ast::TranslationUnit* unit = parser.parseTranslationUnit();
		ast::AstPrinter printer;
		return unit ? printer.print(*unit) : std::string("<null>");
	}

	// True when parsing `source` reported at least one error. The parser recovers rather than
	// stopping (panic mode, see parser.h), so a rejected construct still produces a unit - what
	// makes it a rejection is the diagnostic, not a null return.
	bool parseFails(std::string_view source)
	{
		support::Arena arena;
		support::DiagnosticEngine diagnostics;
		support::StringPool pool;
		lexer::Lexer lexer(source, testSourceId(), diagnostics, pool);
		Parser parser(lexer, arena, diagnostics);

		parser.parseTranslationUnit();
		return diagnostics.hasErrors();
	}
}

TEST(parser, array_typedef_parameter_decays_to_pointer)
{
	CHECK_EQ(printUnit("typedef int A[3]; void f(A a);"), "(unit (typedef A int[3]) (func f void (params (int* a)) <null>))");
}

TEST(parser, union_and_alignof_are_parsed_as_types_and_constant_expressions)
{
	CHECK_EQ(printUnit("union Value { char byte; int word; }; int x = alignof(union Value);"),
		"(unit (union Value (fields (char byte) (int word))) (var x int (alignof union Value)))");
}

TEST(parser, volatile_restrict_and_register_are_accepted)
{
	CHECK_EQ(printUnit("volatile int device; restrict int* data; int main(void) { register int cached; return 0; }"),
		"(unit (var device volatile int <null>) (var data int* restrict <null>) (func main int (params) (block (decl-stmt (var cached int <null>)) (return 0))))");
}

TEST(parser, a_leading_volatile_qualifies_the_pointee_not_the_pointer)
{
	// The whole point of the qualifier: with `volatile int* p` the accesses THROUGH p are the
	// observable ones. Putting it on the pointer instead would protect a local nothing needed
	// protecting and leave the device register it was written for unguarded. Declaration and
	// parameter must agree - they used to disagree, because only the parameter path routed the
	// qualifier into parseTypeName() while a declaration applied it to the finished type.
	CHECK_EQ(printUnit("int f(volatile int* p) { return *p; }"),
		"(unit (func f int (params (volatile int* p)) (block (return (* p)))))");
	CHECK_EQ(printUnit("int g(void) { volatile int* p = 0; return *p; }"),
		"(unit (func g int (params) (block (decl-stmt (var p volatile int* 0)) (return (* p)))))");
	CHECK_EQ(printUnit("volatile int* global;"),
		"(unit (var global volatile int* <null>))");
}

TEST(parser, a_qualifier_after_the_star_qualifies_the_pointer_itself)
{
	// The other half of the same distinction, and the reason one leading flag could never express
	// both: `int* volatile` and `volatile int*` are different types, told apart only by which side
	// of the star the word sits on.
	CHECK_EQ(printUnit("int f(void) { int* volatile p = 0; return 0; }"),
		"(unit (func f int (params) (block (decl-stmt (var p int* volatile 0)) (return 0))))");
	CHECK_EQ(printUnit("int g(void) { const int* const p = 0; return *p; }"),
		"(unit (func g int (params) (block (decl-stmt (var p const int* const 0)) (return (* p)))))");
}

TEST(parser, a_qualifier_may_follow_the_type_spec)
{
	// `const int` and `int const` are the same type in C, and so are `volatile int` and
	// `int volatile`. Both words go through one loop, so neither spelling is the special case.
	CHECK_EQ(printUnit("int f(void) { int volatile x = 1; return x; }"),
		"(unit (func f int (params) (block (decl-stmt (var x volatile int 1)) (return x))))");
	CHECK_EQ(printUnit("int g(void) { int const volatile x = 1; return x; }"),
		"(unit (func g int (params) (block (decl-stmt (var x const volatile int 1)) (return x))))");
}

TEST(parser, a_qualifier_repeated_across_two_positions_is_still_a_duplicate)
{
	CHECK(parseFails("int f(void) { const int const x = 1; return x; }"));
	CHECK(parseFails("int f(void) { volatile int volatile x = 1; return x; }"));
}

// ---- primary expressions -----------------------------------------------------------------------

TEST(parser, literals_and_names)
{
	CHECK_EQ(printExpr("42"), "42");
	CHECK_EQ(printExpr("1.5"), "1.5");
	CHECK_EQ(printExpr("'a'"), "'a'");
	CHECK_EQ(printExpr("true"), "true");
	CHECK_EQ(printExpr("false"), "false");
	CHECK_EQ(printExpr("\"hi\""), "\"hi\"");
	CHECK_EQ(printExpr("x"), "x");
}

TEST(parser, parenthesized_expression_is_not_its_own_node)
{
	// (1 + 2) * 3 - the parens only affect precedence, they don't survive as a node of their own.
	CHECK_EQ(printExpr("(1 + 2) * 3"), "(* (+ 1 2) 3)");
	CHECK_EQ(printExpr("(x)"), "x");
}

// ---- precedence (§7's table, levels 2-11 via the single precedence-climbing function) -----------

TEST(parser, multiplicative_binds_tighter_than_additive)
{
	CHECK_EQ(printExpr("1 + 2 * 3"), "(+ 1 (* 2 3))");
	CHECK_EQ(printExpr("1 * 2 + 3"), "(+ (* 1 2) 3)");
}

TEST(parser, relational_and_equality_sit_below_arithmetic)
{
	CHECK_EQ(printExpr("a + b == c * d"), "(== (+ a b) (* c d))");
}

TEST(parser, bitwise_levels_are_distinct_from_each_other)
{
	CHECK_EQ(printExpr("a << 1 | b"), "(| (<< a 1) b)");
	CHECK_EQ(printExpr("a & b | c ^ d"), "(| (& a b) (^ c d))");
}

TEST(parser, logical_and_binds_tighter_than_logical_or)
{
	CHECK_EQ(printExpr("a || b && c"), "(|| a (&& b c))");
}

TEST(parser, binary_operators_are_left_associative)
{
	CHECK_EQ(printExpr("1 + 2 + 3"), "(+ (+ 1 2) 3)");
	CHECK_EQ(printExpr("8 - 4 - 2"), "(- (- 8 4) 2)");
}

// ---- assignment (level 1, right-associative, its own node kind) ---------------------------------

TEST(parser, assignment_is_right_associative)
{
	CHECK_EQ(printExpr("a = b = c"), "(= a (= b c))");
}

TEST(parser, compound_assignment_operators)
{
	CHECK_EQ(printExpr("a += 1"), "(+= a 1)");
	CHECK_EQ(printExpr("a <<= 1"), "(<<= a 1)");
}

// ---- ternary (right-associative, sits between assignment and the binary table) ------------------

TEST(parser, ternary_expression)
{
	CHECK_EQ(printExpr("a ? b : c"), "(?: a b c)");
}

TEST(parser, ternary_condition_is_a_full_binary_expression)
{
	CHECK_EQ(printExpr("a || b ? c : d"), "(?: (|| a b) c d)");
}

TEST(parser, ternary_chains_right_associatively)
{
	CHECK_EQ(printExpr("a ? b : c ? d : e"), "(?: a b (?: c d e))");
}

TEST(parser, ternary_branches_allow_assignment)
{
	CHECK_EQ(printExpr("a ? b = 1 : c"), "(?: a (= b 1) c)");
}

TEST(parser, ternary_sits_below_assignment)
{
	// The whole ternary is the right-hand side of the assignment, not the other way around.
	CHECK_EQ(printExpr("a = b ? c : d"), "(= a (?: b c d))");
}

// ---- unary prefix (level 13, right-associative) --------------------------------------------

TEST(parser, unary_prefix_operators)
{
	CHECK_EQ(printExpr("-x"), "(- x)");
	CHECK_EQ(printExpr("!x"), "(! x)");
	CHECK_EQ(printExpr("&x"), "(& x)");
	CHECK_EQ(printExpr("*x"), "(* x)");
	CHECK_EQ(printExpr("~x"), "(~ x)");
	CHECK_EQ(printExpr("++x"), "(++ x)");
	CHECK_EQ(printExpr("--x"), "(-- x)");
}

TEST(parser, unary_prefix_operators_chain_right_to_left)
{
	CHECK_EQ(printExpr("- - x"), "(- (- x))");
	CHECK_EQ(printExpr("!!x"), "(! (! x))");
}

// ---- postfix (level 14, left-associative, chained) -------------------------------------------

TEST(parser, postfix_increment_and_decrement_are_distinct_from_prefix)
{
	CHECK_EQ(printExpr("x++"), "(post++ x)");
	CHECK_EQ(printExpr("x--"), "(post-- x)");
}

TEST(parser, index_expression)
{
	CHECK_EQ(printExpr("a[0]"), "(index a 0)");
}

TEST(parser, member_access_dot_and_arrow)
{
	CHECK_EQ(printExpr("a.b"), "(. a b)");
	CHECK_EQ(printExpr("a->b"), "(-> a b)");
}

TEST(parser, postfix_operators_chain_left_to_right)
{
	CHECK_EQ(printExpr("a.b.c"), "(. (. a b) c)");
	CHECK_EQ(printExpr("a->b.c"), "(. (-> a b) c)");
	CHECK_EQ(printExpr("f(1)(2)"), "(call (call f 1) 2)");
}

TEST(parser, call_expression)
{
	CHECK_EQ(printExpr("f()"), "(call f)");
	CHECK_EQ(printExpr("f(1)"), "(call f 1)");
	CHECK_EQ(printExpr("f(1, 2, 3)"), "(call f 1 2 3)");
}

// ---- cast (level 12, right-associative, disambiguated from a parenthesized expr) -----------------

TEST(parser, cast_to_primitive_types)
{
	CHECK_EQ(printExpr("(int)x"), "(cast int x)");
	CHECK_EQ(printExpr("(char)x"), "(cast char x)");
	CHECK_EQ(printExpr("(unsigned char)x"), "(cast unsigned char x)");
	CHECK_EQ(printExpr("(signed char)x"), "(cast signed char x)");
	CHECK_EQ(printExpr("(short)x"), "(cast short x)");
	CHECK_EQ(printExpr("(unsigned)x"), "(cast unsigned int x)");
	CHECK_EQ(printExpr("(unsigned long)x"), "(cast unsigned long x)");
	CHECK_EQ(printExpr("(float)x"), "(cast float x)");
	CHECK_EQ(printExpr("(bool)x"), "(cast bool x)");
}

TEST(parser, cast_to_pointer_type)
{
	CHECK_EQ(printExpr("(int*)x"), "(cast int* x)");
	CHECK_EQ(printExpr("(int**)x"), "(cast int** x)");
}

TEST(parser, casts_chain_right_associatively)
{
	CHECK_EQ(printExpr("(int)(float)x"), "(cast int (cast float x))");
}

TEST(parser, parenthesized_expression_is_not_mistaken_for_a_cast)
{
	// `x` is not a type-name, so (x) here is grouping, not a cast target.
	CHECK_EQ(printExpr("(x) + 1"), "(+ x 1)");
}

// ---- sizeof (both grammar forms) -----------------------------------------------------------------

TEST(parser, sizeof_of_a_type_name)
{
	CHECK_EQ(printExpr("sizeof(int)"), "(sizeof int)");
	CHECK_EQ(printExpr("sizeof(int*)"), "(sizeof int*)");
}

TEST(parser, sizeof_of_an_expression_with_and_without_parens)
{
	CHECK_EQ(printExpr("sizeof x"), "(sizeof x)");
	CHECK_EQ(printExpr("sizeof(x)"), "(sizeof x)"); // x is not a type-name, so this is sizeof of a parenthesized expr
}

TEST(parser, sizeof_parenthesized_expression_still_allows_postfix_after_the_close_paren)
{
	// sizeof (x)[0] means sizeof applied to (x)[0], not "sizeof(x)" followed by a stray [0].
	CHECK_EQ(printExpr("sizeof (x)[0]"), "(sizeof (index x 0))");
}

// ---- error recovery (Fase 2 scope: fail-fast per expression, see parser.h's header comment) ------

TEST(parser, incomplete_binary_expression_reports_a_diagnostic_and_returns_null)
{
	support::Arena arena;
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	lexer::Lexer lexer("1 +", testSourceId(), diagnostics, pool);
	Parser parser(lexer, arena, diagnostics);

	ast::Expr* expr = parser.parseExpression();
	CHECK(expr == nullptr);
	CHECK(diagnostics.hasErrors());
}

TEST(parser, unterminated_parenthesis_reports_a_diagnostic_and_does_not_hang)
{
	support::Arena arena;
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	lexer::Lexer lexer("(", testSourceId(), diagnostics, pool);
	Parser parser(lexer, arena, diagnostics);

	ast::Expr* expr = parser.parseExpression();
	CHECK(expr == nullptr);
	CHECK(diagnostics.hasErrors());
}

TEST(parser, garbage_input_terminates_and_reports_a_diagnostic)
{
	// Fase 2 is fail-fast, not full panic-mode recovery (see parser.h): parseExpression() consumes
	// only the one bad token it fails on and returns, it does not loop draining the rest of the
	// garbage - that requires a synchronization point (`;`/`}`), which doesn't exist until Fase 3.
	// What actually matters here is that it terminates and reports a diagnostic instead of hanging
	// or crashing.
	support::Arena arena;
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	lexer::Lexer lexer("@@@", testSourceId(), diagnostics, pool);
	Parser parser(lexer, arena, diagnostics);

	ast::Expr* expr = parser.parseExpression();
	CHECK(expr == nullptr);
	CHECK(diagnostics.hasErrors());
}

// ---- statements (Fase 3) -------------------------------------------------------------------------

TEST(parser, empty_and_expr_statements)
{
	CHECK_EQ(printStmt(";"), "(empty)");
	CHECK_EQ(printStmt("x;"), "(expr-stmt x)");
	CHECK_EQ(printStmt("f(1);"), "(expr-stmt (call f 1))");
}

TEST(parser, compound_statement)
{
	CHECK_EQ(printStmt("{ }"), "(block)");
	CHECK_EQ(printStmt("{ x; y; }"), "(block (expr-stmt x) (expr-stmt y))");
	CHECK_EQ(printStmt("{ { x; } }"), "(block (block (expr-stmt x)))");
}

TEST(parser, local_variable_declaration_statement)
{
	CHECK_EQ(printStmt("int x;"), "(decl-stmt (var x int <null>))");
	CHECK_EQ(printStmt("int x = 5;"), "(decl-stmt (var x int 5))");
}

TEST(parser, if_statement_with_and_without_else)
{
	CHECK_EQ(printStmt("if (x) y;"), "(if x (expr-stmt y))");
	CHECK_EQ(printStmt("if (x) y; else z;"), "(if x (expr-stmt y) (expr-stmt z))");
}

TEST(parser, while_statement)
{
	CHECK_EQ(printStmt("while (x) y;"), "(while x (expr-stmt y))");
}

TEST(parser, do_while_statement)
{
	CHECK_EQ(printStmt("do y; while (x);"), "(do-while (expr-stmt y) x)");
}

TEST(parser, for_statement_with_expression_init)
{
	CHECK_EQ(printStmt("for (i = 0; i < 10; i = i + 1) x;"),
		"(for (expr-stmt (= i 0)) (< i 10) (= i (+ i 1)) (expr-stmt x))");
}

TEST(parser, for_statement_with_declaration_init)
{
	CHECK_EQ(printStmt("for (int i = 0; i < 10; i = i + 1) x;"),
		"(for (decl-stmt (var i int 0)) (< i 10) (= i (+ i 1)) (expr-stmt x))");
}

TEST(parser, for_statement_with_all_clauses_empty)
{
	CHECK_EQ(printStmt("for (;;) x;"), "(for <null> <null> <null> (expr-stmt x))");
}

TEST(parser, return_statement_with_and_without_value)
{
	CHECK_EQ(printStmt("return;"), "(return <null>)");
	CHECK_EQ(printStmt("return x + 1;"), "(return (+ x 1))");
}

TEST(parser, break_and_continue_statements)
{
	CHECK_EQ(printStmt("break;"), "(break)");
	CHECK_EQ(printStmt("continue;"), "(continue)");
}

TEST(parser, dangling_else_binds_to_the_nearest_if)
{
	CHECK_EQ(printStmt("if (a) if (b) x; else y;"), "(if a (if b (expr-stmt x) (expr-stmt y)))");
}

// ---- declarations (Fase 3) -----------------------------------------------------------------------

TEST(parser, global_variable_declaration)
{
	CHECK_EQ(printDecl("int x;"), "(var x int <null>)");
	CHECK_EQ(printDecl("int x = 5;"), "(var x int 5)");
}

TEST(parser, function_prototype_has_no_body)
{
	CHECK_EQ(printDecl("int foo();"), "(func foo int (params) <null>)");
}

TEST(parser, function_definition_with_parameters)
{
	CHECK_EQ(printDecl("int foo(int x, float y) { return x; }"),
		"(func foo int (params (int x) (float y)) (block (return x)))");
}

TEST(parser, function_with_void_parameter_list_means_no_parameters)
{
	CHECK_EQ(printDecl("void main(void) { }"), "(func main void (params) (block))");
}

TEST(parser, function_with_empty_parameter_list)
{
	CHECK_EQ(printDecl("void main() { }"), "(func main void (params) (block))");
}

// ---- array/pointer declarators -------------------------------------------------------------------
//
// direct-declarator's `("[" INT_LITERAL? "]")*` suffix (§7's grammar), wired into every declarator
// site: local/global VarDecl, struct field, typedef, and function parameter (which additionally
// decays its outermost dimension to a pointer, real C's own rule - see applyDeclarator()'s
// header comment in parser.h).

TEST(parser, local_array_declaration)
{
	CHECK_EQ(printStmt("int arr[10];"), "(decl-stmt (var arr int[10] <null>))");
}

TEST(parser, global_array_declaration)
{
	CHECK_EQ(printDecl("int arr[10];"), "(var arr int[10] <null>)");
}

TEST(parser, two_dimensional_array_declaration)
{
	CHECK_EQ(printDecl("int m[3][4];"), "(var m int[4][3] <null>)");
}

TEST(parser, array_of_pointers_declaration)
{
	CHECK_EQ(printDecl("int* arr[5];"), "(var arr int*[5] <null>)");
}

TEST(parser, pointer_to_array_element_type_is_still_an_ordinary_pointer_declarator)
{
	// The leading `*` belongs to parseTypeName() (the base type), not to the array suffix - so
	// `int* p;` with no brackets is unaffected by any of this.
	CHECK_EQ(printDecl("int* p;"), "(var p int* <null>)");
}

TEST(parser, struct_field_array_declaration)
{
	CHECK_EQ(printDecl("struct Point { int coords[3]; };"), "(struct Point (fields (int[3] coords)))");
}

TEST(parser, typedef_of_an_array_type)
{
	CHECK_EQ(printUnit("typedef int IntArray[4]; IntArray x;"),
		"(unit (typedef IntArray int[4]) (var x int[4] <null>))");
}

TEST(parser, parameter_array_decays_to_a_pointer)
{
	CHECK_EQ(printDecl("void f(int a[10]) { }"), "(func f void (params (int* a)) (block))");
}

TEST(parser, parameter_array_with_no_size_decays_to_a_pointer_just_like_a_sized_one)
{
	CHECK_EQ(printDecl("void f(int a[]) { }"), "(func f void (params (int* a)) (block))");
}

TEST(parser, two_dimensional_parameter_array_decays_only_its_outermost_dimension)
{
	CHECK_EQ(printDecl("void f(int m[][4]) { }"), "(func f void (params (int[4]* m)) (block))");
}

TEST(parser, array_size_is_required_outside_a_parameter_declarator)
{
	support::Arena arena;
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	lexer::Lexer lexer("int arr[];", testSourceId(), diagnostics, pool);
	Parser parser(lexer, arena, diagnostics);

	ast::Decl* decl = parser.parseExternalDecl();
	CHECK(decl != nullptr); // still recovers to *a* type (size 1) instead of losing the declarator
	CHECK(diagnostics.hasErrors());
}

TEST(parser, array_size_must_be_a_positive_integer_literal)
{
	support::Arena arena;
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	lexer::Lexer lexer("int arr[0];", testSourceId(), diagnostics, pool);
	Parser parser(lexer, arena, diagnostics);

	CHECK(parser.parseExternalDecl() != nullptr);
	CHECK(diagnostics.hasErrors());
}

TEST(parser, array_size_must_be_a_constant_not_an_arbitrary_expression)
{
	support::Arena arena;
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	// `n` is an identifier, not an INT_LITERAL - not implemented in this version (see §3: "fixed-size
	// arrays" only), and must not be confused with a syntax error that loses the rest of the file.
	lexer::Lexer lexer("int arr[n]; int y;", testSourceId(), diagnostics, pool);
	Parser parser(lexer, arena, diagnostics);

	ast::TranslationUnit* unit = parser.parseTranslationUnit();
	CHECK(unit != nullptr);
	CHECK_EQ(unit->decls().size(), usize(2));
	CHECK(diagnostics.hasErrors());
}

TEST(parser, array_of_void_is_an_error)
{
	support::Arena arena;
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	lexer::Lexer lexer("void arr[3];", testSourceId(), diagnostics, pool);
	Parser parser(lexer, arena, diagnostics);

	CHECK(parser.parseExternalDecl() != nullptr);
	CHECK(diagnostics.hasErrors());
}

// ---- brace initializers (Fase 7) -----------------------------------------------------------------
//
// The parser's whole job here is the SHAPE - whether the braces nest the way the declared type
// needs them is Sema::checkInitializer()'s question, so every well-formed list below parses
// regardless of what it is initializing.

TEST(parser, array_brace_initializer_parses_as_an_init_list)
{
	CHECK_EQ(printUnit("int arr[3] = { 1, 2, 3 };"),
		"(unit (var arr int[3] (init-list 1 2 3)))");
}

TEST(parser, nested_brace_initializers_nest_in_the_ast)
{
	CHECK_EQ(printUnit("int m[2][2] = { { 1, 2 }, { 3, 4 } };"),
		"(unit (var m int[2][2] (init-list (init-list 1 2) (init-list 3 4))))");
}

TEST(parser, a_brace_initializer_element_can_be_any_assignment_expression)
{
	CHECK_EQ(printUnit("int n; int arr[2] = { n + 1, -n };"),
		"(unit (var n int <null>) (var arr int[2] (init-list (+ n 1) (- n))))");
}

TEST(parser, a_brace_initializer_works_on_a_local_declaration_too)
{
	CHECK_EQ(printUnit("void f() { int a[2] = { 7, 8 }; }"),
		"(unit (func f void (params) (block (decl-stmt (var a int[2] (init-list 7 8))))))");
}

TEST(parser, a_string_literal_initializer_is_an_ordinary_expression_not_a_list)
{
	CHECK_EQ(printUnit("char s[4] = \"hi\";"),
		"(unit (var s char[4] \"hi\"))");
}

TEST(parser, an_empty_brace_initializer_is_rejected)
{
	support::Arena arena;
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	lexer::Lexer lexer("int arr[3] = { }; int y;", testSourceId(), diagnostics, pool);
	Parser parser(lexer, arena, diagnostics);

	ast::TranslationUnit* unit = parser.parseTranslationUnit();
	CHECK(unit != nullptr);
	CHECK(diagnostics.hasErrors());
	// Panic-mode recovery still gets to the second declaration - one bad initializer does not
	// take the rest of the file with it.
	CHECK_EQ(unit->decls().size(), usize(2));
}

TEST(parser, a_trailing_comma_in_a_brace_initializer_is_rejected_but_the_values_are_kept)
{
	support::Arena arena;
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	lexer::Lexer lexer("int arr[3] = { 1, 2, };", testSourceId(), diagnostics, pool);
	Parser parser(lexer, arena, diagnostics);

	ast::TranslationUnit* unit = parser.parseTranslationUnit();
	CHECK(unit != nullptr);
	CHECK(diagnostics.hasErrors());
	CHECK_EQ(unit->decls().size(), usize(1)); // the list itself survives - see parseInitializer()
}

TEST(parser, an_unterminated_brace_initializer_does_not_hang)
{
	support::Arena arena;
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	lexer::Lexer lexer("int arr[3] = { 1, 2", testSourceId(), diagnostics, pool);
	Parser parser(lexer, arena, diagnostics);

	ast::TranslationUnit* unit = parser.parseTranslationUnit();
	CHECK(unit != nullptr);
	CHECK(diagnostics.hasErrors());
}

TEST(parser, a_brace_is_still_a_syntax_error_in_an_ordinary_expression)
{
	support::Arena arena;
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	// Only a declarator's initializer position accepts a brace list (expr.h/parser.h) - `x = {1}`
	// as a statement must stay the error it always was.
	lexer::Lexer lexer("void f() { int x; x = { 1 }; }", testSourceId(), diagnostics, pool);
	Parser parser(lexer, arena, diagnostics);

	parser.parseTranslationUnit();
	CHECK(diagnostics.hasErrors());
}

// ---- translation unit (Fase 3) -------------------------------------------------------------------

TEST(parser, translation_unit_with_multiple_declarations)
{
	CHECK_EQ(printUnit("int x; int foo() { return x; }"),
		"(unit (var x int <null>) (func foo int (params) (block (return x))))");
}

TEST(parser, empty_translation_unit)
{
	CHECK_EQ(printUnit(""), "(unit)");
}

// ---- error recovery (Fase 3 scope: real panic-mode recovery, see parser.h's header comment) ------

TEST(parser, a_broken_statement_inside_a_block_is_skipped_and_the_rest_still_parses)
{
	support::Arena arena;
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	lexer::Lexer lexer("{ @ ; y; }", testSourceId(), diagnostics, pool);
	Parser parser(lexer, arena, diagnostics);

	ast::Stmt* stmt = parser.parseStatement();
	ast::AstPrinter printer;
	CHECK(stmt != nullptr);
	CHECK_EQ(printer.print(*stmt), "(block (expr-stmt y))");
	CHECK(diagnostics.hasErrors());
}

TEST(parser, a_broken_declaration_at_top_level_is_skipped_and_the_rest_still_parses)
{
	support::Arena arena;
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	lexer::Lexer lexer("int x; @ int y;", testSourceId(), diagnostics, pool);
	Parser parser(lexer, arena, diagnostics);

	ast::TranslationUnit* unit = parser.parseTranslationUnit();
	ast::AstPrinter printer;
	CHECK(unit != nullptr);
	CHECK_EQ(printer.print(*unit), "(unit (var x int <null>) (var y int <null>))");
	CHECK(diagnostics.hasErrors());
}

TEST(parser, a_stray_closing_brace_at_top_level_does_not_hang)
{
	support::Arena arena;
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	lexer::Lexer lexer("} int x;", testSourceId(), diagnostics, pool);
	Parser parser(lexer, arena, diagnostics);

	ast::TranslationUnit* unit = parser.parseTranslationUnit();
	ast::AstPrinter printer;
	CHECK(unit != nullptr);
	CHECK_EQ(printer.print(*unit), "(unit (var x int <null>))");
	CHECK(diagnostics.hasErrors());
}

// ---- struct (Fase 3 continued) -------------------------------------------------------------------

TEST(parser, struct_tag_declaration_with_no_variable)
{
	CHECK_EQ(printDecl("struct Point { int x; int y; };"), "(struct Point (fields (int x) (int y)))");
}

TEST(parser, struct_defined_and_instantiated_in_one_declaration)
{
	CHECK_EQ(printDecl("struct Point { int x; int y; } p;"), "(var p struct Point <null>)");
}

TEST(parser, struct_used_as_a_variable_type_after_its_own_declaration)
{
	CHECK_EQ(printUnit("struct Point { int x; int y; }; struct Point p;"),
		"(unit (struct Point (fields (int x) (int y))) (var p struct Point <null>))");
}

TEST(parser, struct_forward_declaration_then_pointer_use)
{
	CHECK_EQ(printUnit("struct Foo; struct Foo* make();"),
		"(unit (struct Foo <incomplete>) (func make struct Foo* (params) <null>))");
}

TEST(parser, self_referential_struct_via_pointer)
{
	CHECK_EQ(printDecl("struct Node { int value; struct Node* next; };"),
		"(struct Node (fields (int value) (struct Node* next)))");
}

TEST(parser, struct_redefinition_reports_a_diagnostic_but_keeps_the_latest_fields)
{
	support::Arena arena;
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	lexer::Lexer lexer("struct Foo { int a; }; struct Foo { int b; };", testSourceId(), diagnostics, pool);
	Parser parser(lexer, arena, diagnostics);

	ast::TranslationUnit* unit = parser.parseTranslationUnit();
	ast::AstPrinter printer;
	CHECK(unit != nullptr);
	// Both entries alias the SAME StructDecl (the tag table caches by pointer identity), so both
	// print whatever its final state ended up being - this is a documented consequence of that
	// design (see decl.h/parser.h), not a bug.
	CHECK_EQ(printer.print(*unit), "(unit (struct Foo (fields (int b))) (struct Foo (fields (int b))))");
	CHECK(diagnostics.hasErrors());
}

TEST(parser, local_struct_declaration_and_instantiation_as_statements)
{
	CHECK_EQ(printStmt("struct Point { int x; };"), "(decl-stmt (struct Point (fields (int x))))");
	CHECK_EQ(printStmt("struct Point { int x; int y; } p;"), "(decl-stmt (var p struct Point <null>))");
}

TEST(parser, a_broken_field_inside_a_struct_body_is_skipped)
{
	support::Arena arena;
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	lexer::Lexer lexer("struct Foo { @ int y; int z; };", testSourceId(), diagnostics, pool);
	Parser parser(lexer, arena, diagnostics);

	ast::Decl* decl = parser.parseExternalDecl();
	ast::AstPrinter printer;
	CHECK(decl != nullptr);
	CHECK_EQ(printer.print(*decl), "(struct Foo (fields (int z)))");
	CHECK(diagnostics.hasErrors());
}

// ---- enum (Fase 3 continued) --------------------------------------------------------------------

TEST(parser, enum_tag_declaration_with_no_variable)
{
	CHECK_EQ(printDecl("enum Color { RED, GREEN, BLUE };"), "(enum Color (enumerators (RED) (GREEN) (BLUE)))");
}

TEST(parser, enum_enumerators_can_have_explicit_values)
{
	CHECK_EQ(printDecl("enum E { A, B = 5, C };"), "(enum E (enumerators (A) (B 5) (C)))");
}

TEST(parser, enum_allows_a_trailing_comma)
{
	CHECK_EQ(printDecl("enum Color { RED, GREEN, };"), "(enum Color (enumerators (RED) (GREEN)))");
}

TEST(parser, enum_defined_and_instantiated_in_one_declaration)
{
	CHECK_EQ(printDecl("enum Color { RED, GREEN } c;"), "(var c enum Color <null>)");
}

TEST(parser, enum_forward_declaration)
{
	CHECK_EQ(printDecl("enum Status;"), "(enum Status <incomplete>)");
}

TEST(parser, enum_redefinition_reports_a_diagnostic)
{
	support::Arena arena;
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	lexer::Lexer lexer("enum Color { RED }; enum Color { BLUE };", testSourceId(), diagnostics, pool);
	Parser parser(lexer, arena, diagnostics);

	ast::TranslationUnit* unit = parser.parseTranslationUnit();
	CHECK(unit != nullptr);
	CHECK(diagnostics.hasErrors());
}

// ---- typedef (Fase 3 continued) -----------------------------------------------------------------

TEST(parser, typedef_declaration)
{
	CHECK_EQ(printDecl("typedef int MyInt;"), "(typedef MyInt int)");
}

TEST(parser, typedef_name_usable_as_a_type_afterward)
{
	CHECK_EQ(printUnit("typedef int MyInt; MyInt x;"), "(unit (typedef MyInt int) (var x int <null>))");
}

TEST(parser, typedef_of_a_pointer_type)
{
	CHECK_EQ(printUnit("typedef int* IntPtr; IntPtr p;"), "(unit (typedef IntPtr int*) (var p int* <null>))");
}

TEST(parser, typedef_of_a_struct_type)
{
	CHECK_EQ(printUnit("struct Point { int x; }; typedef struct Point PointT; PointT p;"),
		"(unit (struct Point (fields (int x))) (typedef PointT struct Point) (var p struct Point <null>))");
}

TEST(parser, typedef_name_can_be_used_as_a_cast_target)
{
	CHECK_EQ(printUnit("typedef int MyInt; void f() { (MyInt)0; }"),
		"(unit (typedef MyInt int) (func f void (params) (block (expr-stmt (cast int 0)))))");
}

TEST(parser, typedef_name_can_be_used_with_sizeof)
{
	CHECK_EQ(printUnit("typedef int MyInt; void f() { sizeof(MyInt); }"),
		"(unit (typedef MyInt int) (func f void (params) (block (expr-stmt (sizeof int)))))");
}

TEST(parser, local_typedef_declaration_as_a_statement)
{
	CHECK_EQ(printStmt("typedef int MyInt;"), "(decl-stmt (typedef MyInt int))");
}

// ---- switch / case / default (Fase 3 continued) --------------------------------------------------

TEST(parser, switch_with_case_and_default)
{
	CHECK_EQ(printStmt("switch (x) { case 1: y; break; default: z; }"),
		"(switch x (block (case 1 (expr-stmt y)) (break) (default (expr-stmt z))))");
}

TEST(parser, chained_case_labels_nest_as_body_of_body)
{
	// case 1: case 2: y; break; - case 1 labels (case 2 labels y;), and break is a sibling
	// statement in the enclosing block. This is what makes real C's fallthrough work: nothing
	// special is built for it here, it just falls out of the tree shape.
	CHECK_EQ(printStmt("switch (x) { case 1: case 2: y; break; }"),
		"(switch x (block (case 1 (case 2 (expr-stmt y))) (break)))");
}

TEST(parser, switch_condition_and_body_can_be_arbitrary_expressions_and_statements)
{
	CHECK_EQ(printStmt("switch (a + b) x;"), "(switch (+ a b) (expr-stmt x))");
}

// ---- goto / labels (Fase 3 continued) ------------------------------------------------------------

TEST(parser, goto_statement)
{
	CHECK_EQ(printStmt("goto end;"), "(goto end)");
}

TEST(parser, labeled_statement)
{
	CHECK_EQ(printStmt("end: x;"), "(label end (expr-stmt x))");
}

TEST(parser, goto_and_label_together_in_a_block)
{
	CHECK_EQ(printStmt("{ start: x; goto start; }"),
		"(block (label start (expr-stmt x)) (goto start))");
}

TEST(parser, a_plain_identifier_statement_is_not_mistaken_for_a_label)
{
	CHECK_EQ(printStmt("x;"), "(expr-stmt x)");
	CHECK_EQ(printStmt("f(1);"), "(expr-stmt (call f 1))");
}

// ---- integration: struct + typedef + switch in one program ---------------------------------------

TEST(parser, struct_typedef_and_switch_work_together_in_one_program)
{
	support::Arena arena;
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	lexer::Lexer lexer(
		"struct Point { int x; int y; };"
		"typedef struct Point PointT;"
		"int classify(PointT p) { switch (p.x) { case 0: return 0; default: return 1; } }",
		testSourceId(), diagnostics, pool);
	Parser parser(lexer, arena, diagnostics);

	ast::TranslationUnit* unit = parser.parseTranslationUnit();
	CHECK(unit != nullptr);
	CHECK(!diagnostics.hasErrors());
	CHECK_EQ(unit->decls().size(), (usize)3);
}

// ---- declaration specifiers ---------------------------------------------------------------------

TEST(parser, const_qualifies_the_base_type_on_either_side_of_it)
{
	// `const int` and `int const` are the same type, and both must qualify the int itself.
	CHECK_EQ(printDecl("const int x = 1;"), "(var x const int 1)");
	CHECK_EQ(printDecl("int const x = 1;"), "(var x const int 1)");
}

TEST(parser, const_before_the_star_qualifies_the_pointee_and_after_it_the_pointer)
{
	// The one place a misplaced qualifier silently produces a different, wrong type - which is why
	// the leading `const` is threaded into the type name rather than applied to the finished type.
	//
	// These three used to print as `const char*`, `const char*` and `const const char*`: the parser
	// had the distinction right, and the printer put every qualifier in front of the star, so the
	// two different types read identically and the doubled one read as nothing at all. A pointer's
	// own qualifiers belong after the star, which is where they are written in C.
	CHECK_EQ(printDecl("const char* p;"), "(var p const char* <null>)");
	CHECK_EQ(printDecl("char* const p;"), "(var p char* const <null>)");
	CHECK_EQ(printDecl("const char* const p;"), "(var p const char* const <null>)");
}

TEST(parser, a_storage_class_may_come_before_or_after_const)
{
	// C allows the specifiers in any order, so neither spelling may be the only one that parses.
	CHECK_EQ(printDecl("static const int x = 1;"), "(var x const int 1)");
	CHECK_EQ(printDecl("const static int x = 1;"), "(var x const int 1)");
}

TEST(parser, a_declaration_may_begin_with_its_storage_class_in_statement_position_too)
{
	// A statement that starts with `static` is a declaration, not an expression - reaching the
	// expression parser with that token in hand is what used to produce "expected expression".
	CHECK_EQ(printStmt("static int n = 1;"), "(decl-stmt (var n int 1))");
	CHECK_EQ(printStmt("extern int n;"), "(decl-stmt (var n int <null>))");
	CHECK_EQ(printStmt("auto int n = 1;"), "(decl-stmt (var n int 1))");
	CHECK_EQ(printStmt("const int n = 1;"), "(decl-stmt (var n const int 1))");
}

TEST(parser, a_for_loop_may_declare_its_index_with_a_specifier)
{
	CHECK(printStmt("for (const int i = 0; i < 3; ) { }").find("(for") == 0);
	CHECK(printStmt("for (static int i = 0; i < 3; ) { }").find("(for") == 0);
}

TEST(parser, a_parameter_may_be_const_qualified)
{
	CHECK_EQ(printDecl("int length(const char* text);"),
		"(func length int (params (const char* text)) <null>)");
}

TEST(parser, inline_is_accepted_on_a_function_definition)
{
	CHECK_EQ(printDecl("inline int twice(int n) { return n * 2; }"),
		"(func twice int (params (int n)) (block (return (* n 2))))");
}

// ---- variadic functions ---------------------------------------------------------------------

namespace
{
	// True when parsing the whole of `source` reported at least one error. The variadic rejections
	// below are all whole-declaration shapes, so there is nothing finer to assert than that the
	// parser refused them.
	bool unitHasErrors(std::string_view source)
	{
		support::Arena arena;
		support::DiagnosticEngine diagnostics;
		support::StringPool pool;
		lexer::Lexer lexer(source, testSourceId(), diagnostics, pool);
		Parser parser(lexer, arena, diagnostics);
		parser.parseTranslationUnit();
		return diagnostics.hasErrors();
	}
}

TEST(parser, a_variadic_parameter_list_records_only_its_fixed_parameters)
{
	CHECK_EQ(printUnit("int printf_like(int level, ...);"),
		"(unit (func printf_like int (params (int level) ...) <null>))");
}

TEST(parser, a_variadic_definition_and_the_builtins_parse)
{
	CHECK_EQ(printDecl("void trace(char* format, ...) { __builtin_va_list ap; __builtin_va_start(ap, format); __builtin_va_end(ap); }"),
		"(func trace void (params (char* format) ...) (block"
		" (decl-stmt (var ap char* <null>))"
		" (expr-stmt (__builtin_va_start ap format))"
		" (expr-stmt (__builtin_va_end ap))))");
}

TEST(parser, va_arg_takes_a_type_name_where_a_call_would_take_an_expression)
{
	CHECK_EQ(printExpr("__builtin_va_arg(ap, int)"), "(__builtin_va_arg ap int)");
}

TEST(parser, the_va_builtin_names_are_only_special_in_call_position)
{
	// Nothing reserves these names, so a program that uses one as an ordinary variable keeps
	// working - only `name(` is treated as the builtin.
	CHECK_EQ(printExpr("__builtin_va_arg + 1"), "(+ __builtin_va_arg 1)");
}

TEST(parser, an_ellipsis_needs_a_named_parameter_before_it)
{
	CHECK(unitHasErrors("int f(...);"));
}

TEST(parser, an_ellipsis_must_close_the_parameter_list)
{
	CHECK(unitHasErrors("int f(int x, ..., int y);"));
	CHECK(unitHasErrors("int f(int x, ...,);"));
}

TEST(parser, register_is_not_allowed_on_a_function)
{
	// A function has no storage of its own for `register` to ask about - the same reason `auto` is
	// rejected on one.
	CHECK(unitHasErrors("register int f(void);"));
}

TEST(parser, register_is_the_one_storage_class_a_parameter_may_carry)
{
	CHECK_EQ(printUnit("int f(register int a, int b);"),
		"(unit (func f int (params (register int a) (int b)) <null>))");

	// It is part of the DECLARATION, not of the type, so a prototype and the definition need not
	// agree about it - the same as in C.
	CHECK(!unitHasErrors("int f(int a); int f(register int a) { return a; }"));

	// Everything else answers a question a parameter does not get to ask: its storage is the
	// calling convention's to decide.
	CHECK(unitHasErrors("int f(static int a);"));
	CHECK(unitHasErrors("int f(extern int a);"));
	CHECK(unitHasErrors("int f(auto int a);"));

	// And  still means nothing in a type name, where there is no object at all.
	CHECK(unitHasErrors("int f(void) { return sizeof(register int); }"));
}

TEST(parser, the_machine_builtins_are_syntax_rather_than_calls)
{
	CHECK_EQ(printUnit("int main(void) { __builtin_sti(); return 0; }"),
		"(unit (func main int (params) (block (expr-stmt (__builtin_sti)) (return 0))))");
	CHECK_EQ(printUnit("int main(void) { __builtin_cli(); __builtin_halt(); return 0; }"),
		"(unit (func main int (params) (block (expr-stmt (__builtin_cli)) (expr-stmt (__builtin_halt)) (return 0))))");

	// None of the three takes an operand.
	CHECK(parseFails("int main(void) { __builtin_sti(1); return 0; }"));

	// Only in call position, so the names stay available to a program that has its own.
	CHECK_EQ(printUnit("int __builtin_sti; int main(void) { return __builtin_sti; }"),
		"(unit (var __builtin_sti int <null>) (func main int (params) (block (return __builtin_sti))))");
}

// ---- declarators -------------------------------------------------------------------------------

TEST(parser, a_declarator_binds_suffixes_tighter_than_the_leading_star)
{
	// The whole reason a declarator is parsed into a tree and applied afterwards. These two differ
	// by one pair of parentheses and mean opposite things, and nothing walking the tokens left to
	// right can tell them apart on its own.
	CHECK_EQ(printDecl("int *f(int x);"), "(func f int* (params (int x)) <null>)");
	CHECK_EQ(printDecl("int (*f)(int x);"), "(var f int (*)(int) <null>)");
}

TEST(parser, whether_a_declarator_declares_a_function_is_decided_by_the_declarator)
{
	// `int f(int)` is a function; `int (*f)(int)` is a variable whose type happens to be a pointer
	// to one. The parser makes that call by asking whether the derived type is a function type,
	// which is C's own rule rather than a lookahead heuristic.
	CHECK_EQ(printDecl("int f(int x);"), "(func f int (params (int x)) <null>)");
	CHECK_EQ(printDecl("int (*f)(int x);"), "(var f int (*)(int) <null>)");
	CHECK_EQ(printDecl("int (*table[3])(int x);"), "(var table int (*)(int)[3] <null>)");
}

TEST(parser, a_typedef_can_name_a_function_type_or_a_pointer_to_one)
{
	// Modelling the function type separately from the pointer is what makes these two spellings the
	// same type by construction: `Handler*` IS `int (*)(int)`.
	CHECK_EQ(printDecl("typedef int Handler(int x);"), "(typedef Handler int (int))");
	CHECK_EQ(printDecl("typedef int (*HandlerPtr)(int x);"), "(typedef HandlerPtr int (*)(int))");
}

TEST(parser, a_function_typed_parameter_decays_to_a_pointer_however_it_is_spelled)
{
	// Three ways of writing one parameter, all of which C says mean a pointer to a function -
	// there is nothing else a function could be passed as.
	CHECK_EQ(printUnit("void f(int (*g)(int y));"),
		"(unit (func f void (params (int (*)(int) g)) <null>))");
	CHECK_EQ(printUnit("void f(int g(int y));"),
		"(unit (func f void (params (int (*)(int) g)) <null>))");
	CHECK_EQ(printUnit("typedef int H(int y); void f(H* g);"),
		"(unit (typedef H int (int)) (func f void (params (int (*)(int) g)) <null>))");
}

TEST(parser, a_pointer_to_an_array_is_not_an_array_of_pointers)
{
	CHECK_EQ(printDecl("int (*p)[3];"), "(var p int[3]* <null>)");
	CHECK_EQ(printDecl("int *p[3];"), "(var p int*[3] <null>)");
}

TEST(parser, an_abstract_declarator_names_a_function_pointer_type)
{
	// A type-name is a declarator with the name left out, which is what a cast needs.
	CHECK_EQ(printExpr("(int (*)(int))p"), "(cast int (*)(int) p)");
	CHECK_EQ(printExpr("sizeof(int (*)(int))"), "(sizeof int (*)(int))");
}

TEST(parser, a_function_may_not_return_a_function_or_an_array)
{
	CHECK(parseFails("int f(int x)(int y);"));
	CHECK(parseFails("int f(int x)[3];"));
}

TEST(parser, a_struct_field_may_be_a_function_pointer_but_not_a_function)
{
	CHECK_EQ(printDecl("struct Ops { int (*run)(int x); };"),
		"(struct Ops (fields (int (*)(int) run)))");
	CHECK(parseFails("struct Ops { int run(int x); };"));
}
