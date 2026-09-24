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

TEST(sema, literal_suffixes_select_the_type)
{
	// `u`/`U` makes an integer literal unsigned; `f`/`F` makes a float (and forces a digit run to one).
	CHECK_EQ(typeOfMainLastExpr("int main() { 42u; }"), "unsigned int");
	CHECK_EQ(typeOfMainLastExpr("int main() { 42U; }"), "unsigned int");
	CHECK_EQ(typeOfMainLastExpr("int main() { 0xFFu; }"), "unsigned int");
	CHECK_EQ(typeOfMainLastExpr("int main() { 1.5f; }"), "float");
	CHECK_EQ(typeOfMainLastExpr("int main() { 1F; }"), "float");
}

TEST(sema, long_long_literal_suffix_selects_the_64_bit_type)
{
	// `ll`/`LL` names `long long`; with `u`/`U` as well it is `unsigned long long`, in either order.
	CHECK_EQ(typeOfMainLastExpr("int main() { 42ll; }"), "long long");
	CHECK_EQ(typeOfMainLastExpr("int main() { 42LL; }"), "long long");
	CHECK_EQ(typeOfMainLastExpr("int main() { 0xFFll; }"), "long long");
	CHECK_EQ(typeOfMainLastExpr("int main() { 42ull; }"), "unsigned long long");
	CHECK_EQ(typeOfMainLastExpr("int main() { 42llu; }"), "unsigned long long");
	CHECK_EQ(typeOfMainLastExpr("int main() { 42ULL; }"), "unsigned long long");
}

TEST(sema, long_long_outranks_the_32_bit_integers)
{
	// The usual arithmetic conversions: a 64-bit integer beats every 32-bit one, and
	// `unsigned long long` beats `long long` - so `a + b` picks the wider type.
	CHECK_EQ(typeOfMainLastExpr("long long a; int main() { a + 1; }"), "long long");
	CHECK_EQ(typeOfMainLastExpr("long long a; unsigned int u; int main() { a + u; }"), "long long");
	CHECK_EQ(typeOfMainLastExpr("unsigned long long a; long long b; int main() { a + b; }"), "unsigned long long");
	CHECK_EQ(typeOfMainLastExpr("long long a; int main() { a < 1; }"), "bool");
	CHECK_EQ(typeOfMainLastExpr("long long a; int main() { a ? a : 0; }"), "long long");
}

TEST(sema, sizeof_a_wide_integer_is_eight)
{
	CHECK(checkSource("_Static_assert(sizeof(long long) == 8, \"\"); int main() { }").ok);
	CHECK(checkSource("_Static_assert(sizeof(unsigned long long) == 8, \"\"); int main() { }").ok);
	// Alignment 8 shows up through sizeof: `long long a; char c;` is 9 raw bytes rounded up to 16.
	CHECK(checkSource("struct S { long long a; char c; };"
		"_Static_assert(sizeof(struct S) == 16, \"\"); int main() { }").ok);
}

TEST(sema, struct_layout_pads_a_wide_integer_field)
{
	// `char c; long long w; int i;` = 24 bytes (w pads to offset 8, the total rounds up to 8).
	CHECK(checkSource("struct S { char c; long long w; int i; };"
		"_Static_assert(sizeof(struct S) == 24, \"\"); int main() { }").ok);
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

TEST(sema, anonymous_struct_union_and_enum_types_work_like_tagged_ones)
{
	CheckOutcome outcome = checkSource(
		"typedef struct { int a; char b; } Pair;"
		"typedef union { int i; float f; } Word;"
		"enum { Off, On = 5, Auto };"
		"typedef enum { Red, Green } Color;"
		"struct { int x; int y; } origin;"
		"int main() {"
		"    Pair p; Word w; Color c;"
		"    p.a = On; p.b = 1; w.i = Auto; c = Green;"
		"    origin.x = p.a + w.i + c;"
		"    return sizeof(Pair) + sizeof(Word) + origin.x;"
		"}");
	CHECK(outcome.ok);

	// Two anonymous structs are two different types, exactly as two differently tagged ones are.
	CheckOutcome distinct = checkSource(
		"struct { int a; } first; struct { int a; } second;"
		"int main() { first = second; return 0; }");
	CHECK(!distinct.ok);
}

TEST(sema, enumerators_defined_inside_a_typedef_or_a_variable_declaration_are_visible)
{
	// The tag was only emitted for a bare `enum E { ... };`, so these enumerators never reached the
	// symbol table and every use was "undeclared identifier".
	CHECK(checkSource("typedef enum Tag { A, B } T; int main() { return B; }").ok);
	CHECK(checkSource("enum Mode { Off, On = 4 } mode; int main() { return On + mode; }").ok);
	CHECK(checkSource("int main() { enum { X, Y } v = Y; return v + X; }").ok);
	CHECK(checkSource("typedef struct { enum { Small = 1, Big = 2 } size; } Box; int main() { Box b; b.size = Big; return b.size; }").ok);
	// ... and the layout of a struct defined inside a declaration is now checked like any other.
	CheckOutcome selfContaining = checkSource("struct Node { struct Node inner; } n;");
	CHECK(!selfContaining.ok);
}

TEST(sema, a_const_pointer_converts_to_a_pointer_to_const_void)
{
	// memcpy(dst, src, n) with a `const char* src`: the parameter is `const void*`.
	CheckOutcome outcome = checkSource(
		"int f(const void* p); int g(const char* s) { return f(s); }"
		"int h(const int* s) { const void* q = s; return f(q); }"
		"int k(char* s) { return f(s); }");
	CHECK(outcome.ok);
}

TEST(sema, a_const_pointer_does_not_convert_to_a_plain_void_pointer)
{
	// The other half of the rule: the qualifier must not be forgotten on the way.
	CheckOutcome outcome = checkSource("int f(void* p); int g(const char* s) { return f(s); }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "incompatible type"));
	CheckOutcome dropped = checkSource("int g(const void* s) { void* p = s; return p != 0; }");
	CHECK(!dropped.ok);
}

TEST(sema, a_const_struct_can_be_copied_into_a_plain_one)
{
	// `struct T copy = *p` with `const struct T* p` is a copy, not an alias: it must type-check.
	CheckOutcome outcome = checkSource(
		"struct T { int a; int b; }; union U { int i; char c; };"
		"int f(const struct T* p) { struct T copy = *p; struct T other; other = *p; return copy.a + other.b; }"
		"int g(const union U* p) { union U copy = *p; return copy.i; }"
		"struct T h(const struct T* p) { return *p; }");
	CHECK(outcome.ok);
	// Two different tags are still two different types, and a const pointer still cannot lose its const.
	CheckOutcome other = checkSource("struct A { int a; }; struct B { int a; };"
		"int f(const struct A* p) { struct B b = *p; return b.a; }");
	CHECK(!other.ok);
	CHECK(containsMessage(other, "incompatible type"));
	CheckOutcome dropped = checkSource("struct T { int a; }; int f(const struct T* p) { struct T* q = p; return q->a; }");
	CHECK(!dropped.ok);
}

TEST(sema, a_ternary_may_pair_a_pointer_with_zero_or_another_pointer)
{
	CheckOutcome outcome = checkSource(
		"char* a(int c, char* p) { return c ? p : 0; }"
		"char* b(int c, char* p) { return c ? 0 : p; }"
		"const char* d(int c, const char* p, char* q) { return c ? p : q; }"
		"void* e(int c, char* p, void* q) { return c ? p : q; }"
		"int* f(int c, int* p) { int* r = c ? p : 0; return r; }");
	CHECK(outcome.ok);
	// Only the literal 0 counts, and a pointer cannot be paired with a struct.
	CheckOutcome nonzero = checkSource("char* a(int c, char* p) { return c ? p : 1; }");
	CHECK(!nonzero.ok);
	CheckOutcome variable = checkSource("char* a(int c, char* p, int n) { return c ? p : n; }");
	CHECK(!variable.ok);
	CheckOutcome aggregate = checkSource("struct S { int x; }; char* a(int c, char* p, struct S s) { return c ? p : s; }");
	CHECK(!aggregate.ok);
}

TEST(sema, an_array_takes_its_size_from_its_initializer)
{
	CheckOutcome outcome = checkSource(
		"int a[] = { 1, 2, 3 };"
		"char s[] = \"hello\";"
		"struct P { int x; int y; }; struct P ps[] = { {1, 2}, {3, 4} };"
		"int m[][2] = { {1, 2}, {3, 4}, {5, 6} };"
		"int total() { int local[] = { 4, 5 }; char word[] = \"hi\"; "
		"  return sizeof(a) + sizeof(s) + sizeof(ps) + sizeof(m) + sizeof(local) + sizeof(word); }");
	CHECK(outcome.ok);
}

TEST(sema, an_array_size_may_be_arithmetic_on_literals)
{
	CheckOutcome outcome = checkSource(
		"int a[64]; int b[4 * 512]; int c[1 << 6]; int d[(2 + 3) * 4]; int e[4 + 8]; char f[100 - 1]; int g[7 % 4]; int h[2 * 3][2 + 2];"
		"int total() { return sizeof(a) + sizeof(b) + sizeof(c) + sizeof(d) + sizeof(e) + sizeof(f) + sizeof(g) + sizeof(h); }");
	CHECK(outcome.ok);
	CheckOutcome zero = checkSource("int a[2 - 2];");
	CHECK(!zero.ok);
	CheckOutcome negative = checkSource("int a[1 - 5];");
	CHECK(!negative.ok);
	CheckOutcome divide = checkSource("int a[4 / 0];");
	CHECK(!divide.ok);
	CheckOutcome variable = checkSource("int n = 4; int a[n];");
	CHECK(!variable.ok);
	CHECK(containsMessage(variable, "integer constant"));
	CheckOutcome sized = checkSource("int a[sizeof(int)];");
	CHECK(!sized.ok);
}

TEST(sema, an_array_without_a_size_or_an_initializer_is_still_an_error)
{
	CheckOutcome none = checkSource("int a[];");
	CHECK(!none.ok);
	CHECK(containsMessage(none, "array size is required"));
	CheckOutcome scalar = checkSource("int a[] = 5;");
	CHECK(!scalar.ok);
	// A struct field is the one other place an omitted size is legal - a flexible array member - so
	// it is deliberately NOT an error here. See the flexible-array-member tests below.
	CheckOutcome pointer = checkSource("int f() { int (*p)[] = 0; return 0; }");
	CHECK(!pointer.ok);
	CheckOutcome flatRows = checkSource("int m[][2] = { 1, 2, 3, 4 };");
	CHECK(!flatRows.ok);
	CHECK(containsMessage(flatRows, "cannot infer"));
	CheckOutcome wrongString = checkSource("int a[] = \"text\";");
	CHECK(!wrongString.ok);
	CheckOutcome staticNone = checkSource("static int a[];");
	CHECK(!staticNone.ok);
	CheckOutcome externInitializer = checkSource("extern int a[] = { 1, 2 };\nint n = sizeof(a);");
	CHECK(externInitializer.ok);   // that one is a definition, and the initializer gives the size
}

// ---- extern arrays of unknown size ---------------------------------------------------------------------

TEST(sema, an_extern_array_may_leave_its_size_to_the_unit_that_defines_it)
{
	CHECK(checkSource("extern int table[];\nint main() { return table[2]; }").ok);
	CHECK(checkSource("extern char names[][4];\nint main() { return names[1][2]; }").ok);
	CHECK(checkSource("extern int table[];\nint main() { int* p = table; return p[0]; }").ok);
	CHECK(checkSource("extern int table[];\nint sum(int* v) { return v[0]; }\nint main() { return sum(table); }").ok);
	CHECK(checkSource("int main() { extern int table[]; return table[0]; }").ok);
	CHECK(checkSource("extern const char words[];\nint main() { return words[0]; }").ok);
}

TEST(sema, the_size_of_an_extern_array_is_not_known_and_sizeof_says_so)
{
	CheckOutcome whole = checkSource("extern int table[];\nint n = sizeof(table);");
	CHECK(!whole.ok);
	CHECK(containsMessage(whole, "size is not known"));
	CHECK(containsMessage(whole, "int[]"));
	CHECK(!checkSource("extern int table[];\nint main() { return sizeof table; }").ok);
	// What is known is still there to ask about
	CHECK(checkSource("extern int table[];\nint n = sizeof(table[0]);").ok);
	CHECK(checkSource("extern char names[][4];\nint n = sizeof(names[0]);").ok);
	CHECK(!checkSource("extern char names[][4];\nint n = sizeof(names);").ok);
}

TEST(sema, a_definition_gives_an_extern_array_its_size_whichever_comes_first)
{
	CHECK(checkSource("extern int t[];\nint t[4] = { 1, 2, 3, 4 };\nint n = sizeof(t);").ok);
	CHECK(checkSource("int t[4] = { 1, 2, 3, 4 };\nextern int t[];\nint n = sizeof(t);").ok);
	CHECK(checkSource("extern int t[];\nextern int t[];\nint t[4];\nextern int t[4];").ok);
	// Until the definition, the size is unknown
	CHECK(!checkSource("extern int t[];\nint n = sizeof(t);\nint t[4];").ok);
}

TEST(sema, an_extern_array_and_its_definition_must_agree_apart_from_the_size)
{
	CheckOutcome size = checkSource("extern int t[];\nint t[4];\nint t[5];");
	CHECK(!size.ok);
	CHECK(containsMessage(size, "different type"));
	CHECK(!checkSource("extern int t[4];\nextern int t[5];").ok);
	CHECK(!checkSource("extern int t[];\nchar t[4];").ok);
	CHECK(!checkSource("extern int t[];\nconst int t[4] = { 1, 2, 3, 4 };").ok);
	CHECK(!checkSource("extern char m[][4];\nchar m[2][5];").ok);
	CHECK(!checkSource("extern int t[];\nint t;").ok);
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

TEST(sema, arithmetic_on_constants_is_a_valid_static_initializer)
{
	CHECK(checkSource("static int y = 2 + 3; int main() { return y; }").ok);
	CHECK(checkSource("enum { N = 4 }; static int y = N * 2 + 1; int main() { return y; }").ok);
	CHECK(checkSource("static int a[4]; static int n = sizeof(a) / sizeof(a[0]); int main() { return n; }").ok);
	CHECK(checkSource("static int y = 1 ? 10 : 20; int main() { return y; }").ok);
	CHECK(checkSource("static unsigned int y = 1u << 31; int main() { return 0; }").ok);
	CHECK(checkSource("struct S { int a; int b; }; static struct S s = { 1 + 1, 2 * 3 }; int main() { return s.a; }").ok);
	CHECK(checkSource("static const int t[] = { 1 + 1, -(2 * 3), 8 >> 1 }; int main() { return t[0]; }").ok);
	CHECK(checkSource("static float f = 2 * 3; int main() { return 0; }").ok);
	CHECK(checkSource("int g = 10 * 10 + 1; int main() { return g; }").ok);

	// What is not a constant is still refused: a variable, a division by zero, a comma, a call.
	CHECK(!checkSource("int x = 3; static int y = x + 1; int main() { return y; }").ok);
	CHECK(!checkSource("static int y = 1 / 0; int main() { return y; }").ok);
	CHECK(!checkSource("static int y = (1, 2); int main() { return y; }").ok);
	CHECK(!checkSource("int f(void); static int y = f() + 1; int main() { return y; }").ok);
}

TEST(sema, a_null_pointer_constant_converts_to_a_function_pointer_too)
{
	// NULL is ((void*)0): a void*, which the types alone would refuse for a function pointer.
	CHECK(checkSource("int main() { void (*h)(int) = ((void*)0); return 0; }").ok);
	CHECK(checkSource("void (*g)(int); int main() { g = (void*)0; return 0; }").ok);
	CHECK(checkSource("void f(void (*cb)(int)); int main() { f((void*)0); return 0; }").ok);
	CHECK(checkSource("typedef void (*Handler)(int); Handler get(void) { return (void*)0; } int main() { return 0; }").ok);
	CHECK(checkSource("static void (*table[2])(int) = { 0, ((void*)0) }; int main() { return 0; }").ok);
	CHECK(checkSource("void a(int); int main(int c) { void (*h)(int) = c ? a : (void*)0; return 0; }").ok);
	CHECK(checkSource("void a(int); int main(int c) { void (*h)(int) = c ? (void*)0 : a; return 0; }").ok);

	// Only the constant: a void* variable, or a non-zero address, still does not become a function.
	CHECK(!checkSource("int main() { void* p = 0; void (*h)(int) = p; return 0; }").ok);
	CHECK(!checkSource("int main() { void (*h)(int) = (void*)1; return 0; }").ok);
	CHECK(!checkSource("int main() { int* p = 0; void (*h)(int) = (int*)0; return 0; }").ok);
}

TEST(sema, the_comma_operator_takes_the_type_of_its_right_side)
{
	CHECK(checkSource("int main() { int a; float f; a = (f = 1.5f, 3); return a; }").ok);
	CHECK_EQ(typeOfMainLastExpr("int main() { int a; float f; (a = 1, f = 2.0f); }"), "float");
	CHECK_EQ(typeOfMainLastExpr("int main() { char* p; int a; (a = 1, p); }"), "char*");

	// The result is an rvalue, and the left side may be anything, even a value nobody uses.
	CHECK(!checkSource("int main() { int a; int b; (a, b) = 1; return 0; }").ok);
	CHECK(checkSource("int main() { int a = 0; int b = 0; for (a = 1, b = 2; a < b; a++, b--) { } return 0; }").ok);

	// Never a constant expression, so it cannot size an array or label a case.
	CHECK(!checkSource("int a[(1, 2)]; int main() { return 0; }").ok);
	CHECK(!checkSource("int main() { int x = 1; switch (x) { case (1, 2): return 0; } return 1; }").ok);
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

// ---- flexible array members ---------------------------------------------------------------------

TEST(sema, a_flexible_array_member_is_allowed_as_the_last_field)
{
	CHECK(checkSource("struct S { int n; int a[]; }; int main() { }").ok);
	// The element type may itself be an array: `int a[][3]` is an unsized array of `int[3]`.
	CHECK(checkSource("struct S { int n; int a[][3]; }; int main() { }").ok);
	// A pointer to the element type still works, and so does indexing the member.
	CHECK(checkSource("struct S { int n; int a[]; }; int main() { struct S* p; p->a[2] = 7; return p->a[2]; }").ok);
}

TEST(sema, sizeof_a_struct_with_a_flexible_array_member_excludes_it)
{
	// The member contributes no bytes to the struct's own size, but its alignment still applies -
	// so a trailing `char` still pads the struct out to four bytes.
	CHECK(checkSource("struct S { int n; int a[]; };\n_Static_assert(sizeof(struct S) == 4, \"4\");").ok);
	CHECK(checkSource("struct T { char c; int a[]; };\n_Static_assert(sizeof(struct T) == 4, \"4\");").ok);
}

TEST(sema, a_flexible_array_member_that_is_not_last_is_an_error)
{
	CheckOutcome outcome = checkSource("struct S { int a[]; int n; }; int main() { }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "must be the last member"));
}

TEST(sema, a_struct_with_a_flexible_array_member_cannot_be_embedded_by_value)
{
	CheckOutcome direct = checkSource("struct S { int n; int a[]; }; struct T { struct S s; }; int main() { }");
	CHECK(!direct.ok);
	CHECK(containsMessage(direct, "cannot be held by value"));

	CheckOutcome array = checkSource("struct S { int n; int a[]; }; struct T { struct S arr[3]; }; int main() { }");
	CHECK(!array.ok);
	CHECK(containsMessage(array, "cannot be held by value"));

	// Through a pointer is fine: that is exactly what the member is for.
	CHECK(checkSource("struct S { int n; int a[]; }; struct T { struct S* s; }; int main() { }").ok);
}

TEST(sema, sizeof_a_flexible_array_member_is_an_error)
{
	// The member's own type is an incomplete array, so `sizeof(s.a)` cannot be answered - the same
	// rule that already applies to `extern int table[]`.
	CheckOutcome outcome = checkSource("struct S { int n; int a[]; }; int main() { struct S s; return sizeof(s.a); }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "size is not known"));
}

TEST(sema, a_flexible_array_member_is_rejected_in_a_union)
{
	CheckOutcome outcome = checkSource("union U { int n; int a[]; }; int main() { }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "not allowed in a union"));
}

TEST(sema, an_array_of_a_flexible_array_struct_is_rejected)
{
	// A struct with a FAM cannot be an array's element, or one element's member would run into the
	// next. A standalone object of the type (or a pointer to one) is allowed.
	CheckOutcome outcome = checkSource("struct S { int n; int a[]; }; struct S arr[2]; int main() { }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "array of a struct with a flexible array member"));

	CHECK(checkSource("struct S { int n; int a[]; }; struct S one; struct S* p; int main() { }").ok);
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
	// Arithmetic on constants is one (see arithmetic_on_constants_is_a_valid_static_initializer): it is the
	// run-time expressions that are not.
	CHECK(checkSource("int main() { static int n = 1 + 2; return n; }").ok);
	CheckOutcome expression = checkSource("int seed(void);\nint main() { static int n = 1 + seed(); return n; }");
	CHECK(!expression.ok);
	CHECK(containsMessage(expression, "compile-time constant"));

	CheckOutcome outcome = checkSource("int seed = 4;\nint main() { static int n = seed; return n; }");
	CHECK(!outcome.ok);
	CHECK(containsMessage(outcome, "compile-time constant"));
}

TEST(sema, a_static_initializer_may_be_the_address_of_an_object_with_static_storage)
{
	// The link fills the address in, so it is as constant as a number: an array's name, `&x`, a
	// function's name - for a `static` object or a global, at file scope or in a block.
	CHECK(checkSource("static int cells[3];\nstatic int g;\nstatic int* p = cells;\nstatic int* q = &g;\n"
		"int main() { return 0; }").ok);
	CHECK(checkSource("struct S { int w; const int* px; };\nstatic const int cells[3] = { 1, 2, 3 };\n"
		"static const struct S s = { 3, cells };\nstatic const struct S* table[2] = { &s, &s };\n"
		"int main() { return s.w; }").ok);
	CHECK(checkSource("int f(int a) { return a; }\nint main() { static int local[2]; static int* p = local; static int x; "
		"static int* q = &x; return f(1) + (p == q); }").ok);

	// A cast of a constant is a constant: NULL is `((void*)0)`, and a table is often reached as `(char*)table`.
	CHECK(checkSource("static const char* names[3] = { \"a\", ((void*)0), \"c\" };\nstatic int* none = (int*)0;\n"
		"static char big[8];\nstatic char* bytes = (char*)big;\nstatic float f = (float)3;\nint main() { return 0; }").ok);

	// An automatic local has no fixed address, and the value of a plain variable is not known until run time.
	CheckOutcome automatic = checkSource("int main() { int a[2]; static int* p = a; return 0; }");
	CHECK(!automatic.ok);
	CHECK(containsMessage(automatic, "compile-time constant"));
	CheckOutcome address = checkSource("int main() { int x; static int* p = &x; return 0; }");
	CHECK(!address.ok);
	CHECK(containsMessage(address, "compile-time constant"));
	CheckOutcome parameter = checkSource("int f(int* a) { static int* p = a; return 0; }\nint main() { return 0; }");
	CHECK(!parameter.ok);
	CheckOutcome dereference = checkSource("int g;\nint main() { static int* p = &*&g; return 0; }");
	CHECK(!dereference.ok);
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

TEST(sema, adjacent_string_literals_are_one_object_of_one_size)
{
	// Joined by the lexer, before sema ever sees them - so the size, the array it fits and the
	// array it does not are all decided on the joined text.
	CHECK(checkSource("int f(void) { return (int)sizeof(\"a\" \"bc\"); }").ok);
	CHECK(checkSource("char s[4] = \"ab\" \"c\";").ok);

	CheckOutcome tooLong = checkSource("char s[3] = \"ab\" \"cd\";");
	CHECK(!tooLong.ok);
	CHECK(containsMessage(tooLong, "5 byte(s)"));
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

// ---- _Static_assert and __func__ ---------------------------------------------------------------------------------

TEST(sema, a_true_static_assertion_costs_nothing_wherever_it_stands)
{
	CHECK(checkSource("_Static_assert(sizeof(int) == 4, \"a word\");\nint main() { return 0; }").ok);
	CHECK(checkSource("int main() { _Static_assert(sizeof(char) == 1, \"a byte\"); return 0; }").ok);
	CHECK(checkSource("struct H { char k; int n; _Static_assert(sizeof(int) == 4, \"a word\"); };").ok);
	CHECK(checkSource("_Static_assert(1);").ok);
	CHECK(checkSource("enum { N = 3 };\n_Static_assert(N * 2 == 6 && N != 4, \"enumerators fold\");").ok);
	CHECK(checkSource("struct P { int x, y; };\n_Static_assert(sizeof(struct P) == 8, \"layout\");").ok);
	CHECK(checkSource("_Static_assert(sizeof(int) == 4 ? 1 : 0, \"a ternary\");").ok);
}

TEST(sema, a_false_static_assertion_is_an_error_that_carries_its_message)
{
	CheckOutcome withMessage = checkSource("_Static_assert(sizeof(int) == 2, \"an int is two bytes\");");
	CHECK(!withMessage.ok);
	CHECK(containsMessage(withMessage, "static assertion failed: an int is two bytes"));
	CheckOutcome bare = checkSource("_Static_assert(0);");
	CHECK(!bare.ok);
	CHECK(containsMessage(bare, "static assertion failed"));
	CHECK(!checkSource("int main() { _Static_assert(1 == 2, \"in a block\"); return 0; }").ok);
	CHECK(!checkSource("struct H { char k; _Static_assert(sizeof(struct H) == 2, \"no\"); };").ok);   // checked once the body has made H complete
	CHECK(!checkSource("struct P { int x, y; };\n_Static_assert(sizeof(struct P) == 4, \"layout\");").ok);
}

TEST(sema, the_condition_of_a_static_assertion_must_be_a_constant)
{
	CheckOutcome variable = checkSource("int n = 3;\n_Static_assert(n == 3, \"n\");");
	CHECK(!variable.ok);
	CHECK(containsMessage(variable, "must be a constant expression"));
	CHECK(!checkSource("int f(void);\n_Static_assert(f(), \"call\");").ok);
	CHECK(!checkSource("_Static_assert(undeclared_name, \"x\");").ok);
}

TEST(sema, func_names_the_function_it_is_used_in)
{
	CHECK(checkSource("const char* who(void) { return __func__; }").ok);
	CHECK(checkSource("int first(void) { return __FUNCTION__[0]; }").ok);
	CheckOutcome outside = checkSource("const char* name = __func__;");
	CHECK(!outside.ok);
	CHECK(containsMessage(outside, "undeclared identifier '__func__'"));
}

// ---- _Generic ------------------------------------------------------------------------------------------------

TEST(sema, a_generic_selection_has_the_type_and_value_of_the_matching_association)
{
	CHECK_EQ(typeOfMainLastExpr("int main() { _Generic(1, int: 1, float: 2.0f); }"), "int");
	CHECK_EQ(typeOfMainLastExpr("int main() { _Generic(1.0f, int: 1, float: 2.0f); }"), "float");
	// The controlling expression's own type decides, regardless of position in the list
	CHECK_EQ(typeOfMainLastExpr("int main() { _Generic(1.0f, default: 1, float: 2.0f); }"), "float");
	// `default` wins when nothing else matches
	CHECK_EQ(typeOfMainLastExpr("int main() { _Generic('c', int: 1, default: 2.0f); }"), "float");
	// Pointer types match by full type, not just by being pointers
	CHECK_EQ(typeOfMainLastExpr("int main() { int* p = 0; _Generic(p, int*: 1, char*: 2); }"), "int");
	CHECK_EQ(typeOfMainLastExpr("int main() { char* p = 0; _Generic(p, int*: 1, char*: 2); }"), "int");
}

TEST(sema, a_generic_selections_controlling_expression_is_never_evaluated)
{
	// x++ must type-check (it does - int) but must never actually run: if it ran, x would be 1 and
	// the program would return 1, not the 0 the still-zero x asserts below.
	CHECK(checkSource(
		"int main() { int x = 0; int r = _Generic(x++, int: 5, default: 6); "
		"if (x != 0) return 1; return r == 5 ? 0 : 2; }"
	).ok);
}

TEST(sema, a_generic_selection_needs_a_matching_association_or_a_default)
{
	CheckOutcome noMatch = checkSource("int main() { _Generic(1, float: 1.0f); return 0; }");
	CHECK(!noMatch.ok);
	CHECK(containsMessage(noMatch, "no association for type 'int'"));
	CHECK(checkSource("int main() { _Generic(1, float: 1.0f, default: 0); return 0; }").ok);
}

TEST(sema, a_generic_selection_rejects_a_duplicate_type_or_more_than_one_default)
{
	CheckOutcome dup = checkSource("int main() { _Generic(1, int: 1, int: 2); return 0; }");
	CHECK(!dup.ok);
	CHECK(containsMessage(dup, "more than one association"));

	CheckOutcome twoDefaults = checkSource("int main() { _Generic(1, default: 1, default: 2); return 0; }");
	CHECK(!twoDefaults.ok);
	CHECK(containsMessage(twoDefaults, "more than one 'default'"));
}

TEST(sema, every_association_of_a_generic_selection_is_type_checked_even_when_not_selected)
{
	// The int: branch is picked, but the float: branch still has to name a real identifier - this
	// is what "non-evaluated but still checked" means, the same as sizeof(x++)'s operand.
	CheckOutcome bad = checkSource("int main() { _Generic(1, int: 1, float: undeclared_name); return 0; }");
	CHECK(!bad.ok);
	CHECK(containsMessage(bad, "undeclared identifier 'undeclared_name'"));
}

// ---- __asm__("label") --------------------------------------------------------------------------------------------

TEST(sema, an_asm_label_may_follow_a_declarator_at_file_scope)
{
	CHECK(checkSource("int f(int) __asm__(\"g\");\nint main() { return f(1); }").ok);
	CHECK(checkSource("extern int v __asm__(\"the_v\");\nint main() { return v; }").ok);
	CHECK(checkSource("int v __asm__(\"the_v\") = 3;").ok);
	CHECK(checkSource("static int s __asm__(\"the_s\");").ok);
	CHECK(checkSource("int f(void) __asm__(\"g\") { return 1; }").ok);
	CHECK(checkSource("int f(int) __asm(\"g\");").ok);                  // the two older spellings
}

TEST(sema, every_declaration_of_a_name_must_give_it_the_same_label)
{
	CHECK(checkSource("int f(int) __asm__(\"g\");\nint f(int) __asm__(\"g\");\nint f(int x) { return x; }").ok);
	CheckOutcome different = checkSource("int f(int) __asm__(\"g\");\nint f(int) __asm__(\"h\");");
	CHECK(!different.ok);
	CHECK(containsMessage(different, "asm label"));
	CHECK(!checkSource("int v __asm__(\"a\");\nextern int v __asm__(\"b\");").ok);
}

TEST(sema, an_asm_label_is_a_plain_identifier_at_file_scope)
{
	CheckOutcome dash = checkSource("int f(void) __asm__(\"a-b\");");
	CHECK(!dash.ok);
	CHECK(containsMessage(dash, "plain identifier"));
	CHECK(!checkSource("int f(void) __asm__(\"\");").ok);
	CHECK(!checkSource("int f(void) __asm__(\"1abc\");").ok);
	CHECK(!checkSource("int f(void) __asm__(\"a b\");").ok);
	CheckOutcome local = checkSource("int main() { extern int v __asm__(\"the_v\"); return v; }");
	CHECK(!local.ok);
	CHECK(containsMessage(local, "file scope"));
	CHECK(!checkSource("int main() { static int n __asm__(\"the_n\"); return n; }").ok);
}

// ---- designated initializers -------------------------------------------------------------------------------------

TEST(sema, designated_initializers_are_accepted_for_structs_arrays_and_both_nested)
{
	const char* declarations =
		"struct P { int x; int y; };\n"
		"struct L { struct P a; struct P b; int tag; char name[4]; float f; };\n"
		"union U { int i; char c[4]; };\n";
	auto ok = [&](const char* text) { return checkSource(std::string(declarations) + text).ok; };
	CHECK(ok("struct P p = { .y = 2 };"));
	CHECK(ok("struct P p = { .y = 2, .x = 1 };"));
	CHECK(ok("struct P p = { 1, .y = 2 };"));                                  // positional then designated
	CHECK(ok("struct P p = { .x = 1, 2 };"));                                  // and designated then positional: the next field
	CHECK(ok("struct L l = { .b.y = 3, .a = { 1, 2 }, .tag = 5, .name = { 'h', 'i' }, .f = 1 };"));
	CHECK(ok("struct L l = { .a.x = 1, .a.y = 2 };"));
	CHECK(ok("int a[6] = { [4] = 9, 1 };"));
	CHECK(ok("int a[] = { [5] = 1, 2 };"));
	CHECK(ok("struct P v[3] = { [2] = { .x = 8 }, [0].y = 4 };"));
	CHECK(ok("struct P v[3] = { [1] = { 1, 2 }, [1].y = 4 };"));               // a designation merged into an element
	CHECK(ok("union U u = { .i = 1 };"));
	CHECK(ok("int m[2][3] = { [1][2] = 5 };"));                                 // a path of two indices
	CHECK(ok("enum { N = 2 };\nint a[4] = { [N] = 1, [N + 1] = 2 };"));
	CHECK(ok("int f(void) { int a[4] = { [1] = 2, [3] = 4 }; struct P p = { .y = 1 }; return a[1] + p.y; }"));
}

TEST(sema, a_designator_has_to_fit_what_it_initializes)
{
	const char* declarations =
		"struct P { int x; int y; };\n"
		"union U { int i; char c[4]; };\n";
	auto check = [&](const char* text) { return checkSource(std::string(declarations) + text); };

	CheckOutcome noSuchField = check("struct P p = { .z = 1 };");
	CHECK(!noSuchField.ok);
	CHECK(containsMessage(noSuchField, "no member named 'z'"));

	CheckOutcome fieldForArray = check("int a[2] = { .x = 1 };");
	CHECK(!fieldForArray.ok);
	CHECK(containsMessage(fieldForArray, "cannot initialize the array"));

	CheckOutcome indexForStruct = check("struct P p = { [0] = 1 };");
	CHECK(!indexForStruct.ok);
	CHECK(containsMessage(indexForStruct, "index designator"));

	CheckOutcome scalar = check("int n = { .x = 1 };");
	CHECK(!scalar.ok);
	CHECK(containsMessage(scalar, "needs an array, struct or union"));

	CheckOutcome outOfRange = check("int a[3] = { [3] = 1 };");
	CHECK(!outOfRange.ok);
	CHECK(containsMessage(outOfRange, "outside"));
	CHECK(!check("int a[3] = { [-1] = 1 };").ok);

	CheckOutcome notConstant = check("int n = 1;\nint a[3] = { [n] = 1 };");
	CHECK(!notConstant.ok);

	CheckOutcome secondMember = check("union U u = { .c = { 1 } };");
	CHECK(!secondMember.ok);
	CHECK(containsMessage(secondMember, "first member of a union"));

	CHECK(!check("struct P p = { .x = 1, .x.y = 2 };").ok);                    // an int has no member y
	CHECK(!check("int f(void) { return .x = 1; }").ok);                        // not a list at all: parse error
}

TEST(sema, an_unnamed_place_is_zero_and_the_positions_after_a_designator_follow_it)
{
	// The list means: p.x = 0, p.y = 7 - and the size of an array takes in the designated position
	CHECK(checkSource("struct P { int x; int y; };\nstruct P p = { .y = 7 };\n_Static_assert(sizeof(p) == 8);").ok);
	CHECK(checkSource("int a[] = { [5] = 1, 2 };\n_Static_assert(sizeof(a) == 7 * sizeof(int));").ok);
	CHECK(checkSource("int a[] = { 1, [3] = 2 };\n_Static_assert(sizeof(a) == 4 * sizeof(int));").ok);
	// Too many positional values after a designator still trip the size check
	CHECK(!checkSource("int a[3] = { [2] = 1, 2 };").ok);
	CHECK(!checkSource("struct P { int x; int y; };\nstruct P p = { .y = 1, 2 };").ok);
}

// ---- compound literals -------------------------------------------------------------------------------------------

TEST(sema, a_compound_literal_is_an_unnamed_object_that_can_go_wherever_an_object_can)
{
	const char* declarations =
		"struct P { int x; int y; };\n"
		"int sum(const int* v, int n);\n"
		"struct P twice(struct P p);\n";
	auto ok = [&](const char* body) { return checkSource(std::string(declarations) + "int f(void) { " + body + " }").ok; };
	CHECK(ok("struct P p = (struct P){ 1, 2 }; return p.x;"));
	CHECK(ok("return (struct P){ 1, 2 }.y;"));
	CHECK(ok("struct P* q = &(struct P){ 1, 2 }; return q->x;"));
	CHECK(ok("return sum((int[]){ 1, 2, 3 }, 3);"));
	CHECK(ok("return twice((struct P){ 1, 2 }).x;"));
	CHECK(ok("int* a = (int[]){ 1, 2, 3 }; return a[1];"));
	CHECK(ok("return (int[]){ 1, 2, 3 }[2];"));
	CHECK(ok("return (int){ 4 };"));
	CHECK(ok("return (struct P){ .y = 2 }.y;"));                         // designators inside a literal
	CHECK(ok("return (struct P[]){ { 1, 2 }, { 3, 4 } }[1].y;"));         // nested lists
	CHECK(ok("(struct P){ 1, 2 }.x = 5; return 0;"));                     // an lvalue: it can be assigned to
	CHECK(ok("return sizeof((int[]){ 1, 2, 3 });"));
	CHECK(ok("const int* a = (const int[]){ 1, 2 }; return a[0];"));
}

TEST(sema, a_compound_literal_is_checked_like_the_initializer_it_is)
{
	const char* declarations = "struct P { int x; int y; };\n";
	auto check = [&](const char* body) { return checkSource(std::string(declarations) + "int f(void) { " + body + " }"); };

	CheckOutcome tooMany = check("return (struct P){ 1, 2, 3 }.x;");
	CHECK(!tooMany.ok);
	CHECK(containsMessage(tooMany, "3 value(s)"));
	CHECK(!check("return (int[2]){ 1, 2, 3 }[0];").ok);
	CHECK(!check("return (struct P){ 1, 2 }.z;").ok);                     // no such member
	CHECK(!check("(const struct P){ 1, 2 }.x = 5; return 0;").ok);        // a const object
	CHECK(!check("return (struct P){ .z = 1 }.x;").ok);
	CHECK(!check("return (struct Q){ 1 }.x;").ok);                        // an incomplete type
	CheckOutcome voidType = check("(void){ 0 }; return 0;");
	CHECK(!voidType.ok);
	CHECK(!check("return (int){ 1, 2 };").ok);                            // a scalar takes one value
}

TEST(sema, a_compound_literal_at_file_scope_is_a_static_variable_so_its_values_must_be_constants)
{
	CHECK(checkSource("struct P { int x; int y; };\nstruct P* p = &(struct P){ 3, 4 };\nint* a = (int[]){ 1, 2, 3 };").ok);
	CHECK(checkSource("const char** names = (const char*[]){ \"ab\", \"cd\" };").ok);
	CHECK(checkSource("enum { N = 4 };\nint* a = (int[]){ N, N * 2 };").ok);
	CheckOutcome notConstant = checkSource("int n = 3;\nint* a = (int[]){ n };");
	CHECK(!notConstant.ok);
	CHECK(containsMessage(notConstant, "compile-time constant"));
}

// ---- __attribute__((noreturn)) -----------------------------------------------------------------------------------

TEST(sema, a_noreturn_function_that_contains_a_return_is_warned_about)
{
	CheckOutcome returns = checkSource("void stop(void) __attribute__((noreturn));\nvoid stop(void) { return; }");
	CHECK(returns.ok);     // a warning, not an error
	CHECK(containsMessage(returns, "declared 'noreturn'"));

	CheckOutcome loops = checkSource("void stop(void) __attribute__((noreturn));\nvoid stop(void) { for (;;) { } }");
	CHECK(loops.ok);
	CHECK(!containsMessage(loops, "declared 'noreturn'"));

	CHECK(checkSource("void bail(int code) __attribute__((noreturn));\nint main() { bail(1); return 0; }").ok);
	CHECK(!containsMessage(checkSource("void f(void) { return; }"), "declared 'noreturn'"));
}

// ---- the other __attribute__s that do something --------------------------------------------------------------

namespace
{
	const std::string kFormatHeader =
		"int pf(const char* f, ...) __attribute__((format(printf, 1, 2)));\n"
		"int sf(const char* f, ...) __attribute__((__format__(__scanf__, 1, 2)));\n"
		"int vpf(const char* f, char* ap) __attribute__((format(printf, 1, 0)));\n";

	// The diagnostics of one statement placed in main, after the declarations above.
	CheckOutcome formatCall(std::string_view statement)
	{
		return checkSource(kFormatHeader +
			"int main() { int i = 0; short h = 0; char c = 0; long long ll = 0; float f = 0; char buf[8]; int n;\n" +
			std::string(statement) + "\n return 0; }");
	}
}

TEST(sema, a_format_that_matches_its_arguments_is_not_warned_about)
{
	CheckOutcome clean = formatCall(
		"pf(\"%d %i %u %x %c %s %f %g %lld %llu %p %n %% %5.2f %-8s %hhd %hd %ld %zu\\n\", "
		"i, c, 1u, i, 'a', \"s\", f, 2.0f, ll, 3ULL, (void*)0, &n, f, buf, i, h, 4, 5u);"
		"pf(\"%*d|%-.*f\", 3, i, 2, f);"
		"sf(\"%d %hd %hhd %lld %f %lf %s %c %[^,] %*d %n\", &i, &h, &c, &ll, &f, &f, buf, buf, buf, &n);");
	CHECK(clean.ok);
	CHECK(clean.messages.empty());
}

TEST(sema, a_format_argument_of_the_wrong_kind_is_warned_about)
{
	CHECK(containsMessage(formatCall("pf(\"%lld\", i);"), "format '%lld' expects a 'long long', but argument 2 has type 'int'"));
	CHECK(containsMessage(formatCall("pf(\"%d\", ll);"), "expects an 'int'"));
	CHECK(containsMessage(formatCall("pf(\"%d\", f);"), "expects an 'int'"));
	CHECK(containsMessage(formatCall("pf(\"%f\", i);"), "expects a 'float'"));
	CHECK(containsMessage(formatCall("pf(\"%s\", i);"), "expects a 'char *'"));
	CHECK(containsMessage(formatCall("pf(\"%x %s\", i, (void*)0);"), "argument 3 has type"));
	CHECK(containsMessage(formatCall("pf(\"%*d\", f, i);"), "format '%*' expects an 'int'"));
	CHECK(containsMessage(formatCall("pf(\"%jd\", i);"), "expects a 'long long'"));   // intmax_t is 64 bits
	CHECK(containsMessage(formatCall("sf(\"%d\", i);"), "expects an 'int *'"));
	CHECK(containsMessage(formatCall("sf(\"%lld\", &i);"), "expects a 'long long *'"));
	CHECK(containsMessage(formatCall("sf(\"%hd\", &i);"), "expects a 'short *'"));
	CHECK(containsMessage(formatCall("sf(\"%f\", &i);"), "expects a 'float *'"));
	CHECK(formatCall("pf(\"%lld\", i);").ok);                                     // warnings, not errors
}

TEST(sema, a_format_and_a_call_that_disagree_on_the_count_are_warned_about)
{
	CHECK(containsMessage(formatCall("pf(\"%d %d\", i);"), "no argument left"));
	CHECK(containsMessage(formatCall("pf(\"%d\", i, i);"), "argument 3 is not used"));
	CHECK(containsMessage(formatCall("sf(\"%*d\", &i);"), "argument 2 is not used"));  // %*d stores nothing
	CHECK(containsMessage(formatCall("pf(\"%y\");"), "unknown conversion '%y'"));
	CHECK(containsMessage(formatCall("pf(\"%5\");"), "ends in the middle"));
	CHECK(containsMessage(formatCall("sf(\"%[abc\", buf);"), "no closing ']'"));
	// A va_list function's string is still read, but it has no arguments to count.
	CHECK(containsMessage(formatCall("vpf(\"%y\", 0);"), "unknown conversion"));
	CHECK(formatCall("vpf(\"%d %s\", 0);").messages.empty());
}

TEST(sema, a_format_made_at_run_time_is_not_checked)
{
	CHECK(formatCall("const char* fmt = \"%d\"; pf(fmt, f);").messages.empty());
}

TEST(sema, a_format_attribute_that_does_not_fit_its_function_is_ignored_with_a_warning)
{
	CHECK(containsMessage(checkSource("int bad(int x, ...) __attribute__((format(printf, 1, 2)));"), "attribute 'format' on 'bad' ignored"));
	CHECK(containsMessage(checkSource("int bad(const char* f, int a, ...) __attribute__((format(printf, 1, 2)));"), "must be the '...'"));
	CHECK(containsMessage(checkSource("int bad(const char* f, ...) __attribute__((format(printf, 2)));"), "attribute 'format' ignored"));
	CHECK(containsMessage(checkSource("int bad(const char* f, ...) __attribute__((format(printf, 1, 1)));"), "attribute 'format' ignored"));
	// Another archetype is accepted and not checked.
	CheckOutcome other = checkSource("int st(const char* f, ...) __attribute__((format(strftime, 1, 0)));\nint main() { return st(\"%Q\"); }");
	CHECK(other.ok && other.messages.empty());
}

TEST(sema, a_definition_keeps_its_prototypes_format)
{
	CheckOutcome outcome = checkSource(
		"int pf(const char* f, ...) __attribute__((format(printf, 1, 2)));\n"
		"int pf(const char* f, ...) { return 0; }\n"
		"int main() { return pf(\"%s\", 1); }");
	CHECK(containsMessage(outcome, "expects a 'char *'"));
}

TEST(sema, calling_a_deprecated_function_is_warned_about_wherever_it_is_called)
{
	CheckOutcome warned = checkSource(
		"int old(void) __attribute__((deprecated));\n"
		"int main() { return old(); }");
	CHECK(warned.ok);
	CHECK(containsMessage(warned, "deprecated"));

	CHECK(!containsMessage(checkSource("int fresh(void);\nint main() { return fresh(); }"), "deprecated"));
}

TEST(sema, discarding_the_result_of_a_warn_unused_result_function_is_warned_about)
{
	std::string_view header = "int careful(void) __attribute__((warn_unused_result));\n";

	CheckOutcome discarded = checkSource(std::string(header) + "int main() { careful(); return 0; }");
	CHECK(discarded.ok);
	CHECK(containsMessage(discarded, "warn_unused_result"));

	// The warning is about the discarded result, not about the call: using it is silent.
	CheckOutcome used = checkSource(std::string(header) + "int main() { return careful(); }");
	CHECK(used.ok);
	CHECK(!containsMessage(used, "warn_unused_result"));
}

TEST(sema, warn_unused_result_on_a_void_function_does_not_warn)
{
	// There is no result to discard, so the attribute has nothing to say - warning "the result of
	// this call is ignored" here would be misleading, and under -Werror a build breaker.
	CheckOutcome outcome = checkSource("void f(void) __attribute__((warn_unused_result));\nint main() { f(); return 0; }");
	CHECK(outcome.ok);
	CHECK(!containsMessage(outcome, "warn_unused_result"));
}

TEST(sema, a_function_attribute_on_a_prototype_holds_for_the_definition)
{
	// The prototype carries `warn_unused_result`, the definition does not repeat it, and the call
	// still warns - the same rule `noreturn` already follows.
	CheckOutcome outcome = checkSource(
		"int careful(void) __attribute__((warn_unused_result));\n"
		"int careful(void) { return 1; }\n"
		"int main() { careful(); return 0; }");
	CHECK(outcome.ok);
	CHECK(containsMessage(outcome, "warn_unused_result"));
}

TEST(sema, noinline_and_purity_attributes_are_accepted_without_a_word)
{
	CheckOutcome outcome = checkSource(
		"int pure_fn(int a) __attribute__((pure));\n"
		"int const_fn(int b) __attribute__((const));\n"
		"int never(int c) __attribute__((noinline));\n"
		"int force(int d) __attribute__((always_inline)) { return d + 3; }\n"
		"int main() { return pure_fn(1) + const_fn(2) + never(3) + force(4); }");
	CHECK(outcome.ok);
	CHECK(!containsMessage(outcome, "ignored"));
}

// ---- case ranges ----------------------------------------------------------------------------------------

TEST(sema, a_case_range_is_accepted_and_its_bounds_are_checked)
{
	CHECK(checkSource("int f(int x) { switch (x) { case 1 ... 5: return 1; } return 0; }").ok);

	CheckOutcome empty = checkSource("int f(int x) { switch (x) { case 5 ... 1: return 1; } return 0; }");
	CHECK(!empty.ok);
	CHECK(containsMessage(empty, "empty case range"));

	CheckOutcome huge = checkSource("int f(int x) { switch (x) { case 0 ... 100000: return 1; } return 0; }");
	CHECK(!huge.ok);
	CHECK(containsMessage(huge, "spans more than"));

	// A later single case overlapping the range is still a duplicate.
	CheckOutcome overlap = checkSource("int f(int x) { switch (x) { case 1 ... 5: return 1; case 3: return 2; } return 0; }");
	CHECK(!overlap.ok);
	CHECK(containsMessage(overlap, "duplicate case value"));
}

// ---- the one-instruction machine builtins ---------------------------------------------------------------

TEST(sema, the_machine_builtins_type_their_result_and_their_arguments)
{
	CHECK(checkSource("unsigned int f(unsigned int x) { return __builtin_clz(x); }").ok);
	CHECK(checkSource("int f(int x) { return __builtin_abs(x); }").ok);
	CHECK(checkSource("float f(float x) { return __builtin_fabs(x); }").ok);
	CHECK(checkSource("float f(float x, float y) { return __builtin_fmin(x, y); }").ok);
	CHECK(checkSource("int f(float x) { return __builtin_fclass(x); }").ok);
	CHECK(checkSource("unsigned int f(float x) { return __builtin_float_bits(x); }").ok);
	CHECK(checkSource("float f(unsigned int b) { return __builtin_float_from_bits(b); }").ok);

	// `clz` wants an integer and `sqrt` a float - the wrong bank is refused rather than converted.
	CHECK(!checkSource("int f(float x) { return __builtin_clz(x); }").ok);
	CHECK(!checkSource("float f(int x) { return __builtin_sqrt(x); }").ok);
}

TEST(sema, the_compiler_builtins_type_their_result)
{
	CHECK(checkSource("int f(void) { return __builtin_constant_p(1 + 2); }").ok);
	// `expect` has the first operand's own type - here a float.
	CHECK(checkSource("float f(float x) { return __builtin_expect(x, 1.0f); }").ok);
	CHECK(checkSource("void f(void) { __builtin_trap(); }").ok);
	CHECK(checkSource("void f(void) { __builtin_unreachable(); }").ok);
	CHECK(checkSource("unsigned int f(void) { return __builtin_stack_pointer(); }").ok);
	CHECK(checkSource("unsigned int f(void) { return __builtin_flags() & 16u; }").ok);
	CHECK(checkSource("int f(int a, int b) { return __builtin_imin(a, b); }").ok);
	CHECK(checkSource("unsigned int f(unsigned a, unsigned b) { return __builtin_umax(a, b); }").ok);

	// The integer min/max builtins take integers, not floats.
	CHECK(!checkSource("float f(float a, float b) { return __builtin_imin(a, b); }").ok);
}

TEST(sema, overflow_builtins_check_their_operands)
{
	CHECK(checkSource("int f(int a, int b) { int r; return __builtin_add_overflow(a, b, &r); }").ok);

	// The third operand must be a pointer to a 4-byte integer.
	CheckOutcome notPointer = checkSource("int f(int a, int b) { return __builtin_add_overflow(a, b, a); }");
	CHECK(!notPointer.ok);
	CHECK(containsMessage(notPointer, "pointer to a 4-byte integer"));

	// 4-byte operands only.
	CheckOutcome narrow = checkSource("int f(short a, short b) { short r; return __builtin_add_overflow(a, b, &r); }");
	CHECK(!narrow.ok);
	CHECK(containsMessage(narrow, "two 4-byte integer operands"));
}

TEST(sema, an_asm_statement_is_accepted_wherever_a_statement_is)
{
	CHECK(checkSource("int f(void) { __asm__(\"nop\"); return 1; }").ok);
	CHECK(checkSource("void f(int n) { while (n) { __asm__ volatile (\"nop\"); n--; } }").ok);
	CHECK(checkSource("void f(int n) { if (n) __asm__(\"nop\"); else __asm__(\"nop\"); }").ok);
}
