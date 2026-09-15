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
