#include <ceresc/ast/ast_printer.h>
#include <ceresc/support/arena.h>
#include <ceresc/support/string_pool.h>

#include "framework.h"

using namespace ceresc;
using namespace ceresc::ast;

namespace
{
	support::SourceLocation loc() { return support::SourceLocation(support::SourceId::make(1), 1, 1, 0); }
}

// ---- literals ------------------------------------------------------------------------------

TEST(ast_printer, int_literal)
{
	support::Arena arena;
	AstPrinter printer;
	Expr* e = arena.create<IntLiteralExpr>(loc(), (u64)42);
	CHECK_EQ(printer.print(*e), "42");
}

TEST(ast_printer, float_literal)
{
	support::Arena arena;
	AstPrinter printer;
	Expr* e = arena.create<FloatLiteralExpr>(loc(), (f64)1.5);
	CHECK_EQ(printer.print(*e), "1.5");
}

TEST(ast_printer, char_literal_plain)
{
	support::Arena arena;
	AstPrinter printer;
	Expr* e = arena.create<CharLiteralExpr>(loc(), 'a');
	CHECK_EQ(printer.print(*e), "'a'");
}

TEST(ast_printer, char_literal_escaped)
{
	support::Arena arena;
	AstPrinter printer;
	Expr* e = arena.create<CharLiteralExpr>(loc(), '\n');
	CHECK_EQ(printer.print(*e), "'\\n'");
}

TEST(ast_printer, bool_literal)
{
	support::Arena arena;
	AstPrinter printer;
	CHECK_EQ(printer.print(*arena.create<BoolLiteralExpr>(loc(), true)), "true");
	CHECK_EQ(printer.print(*arena.create<BoolLiteralExpr>(loc(), false)), "false");
}

TEST(ast_printer, string_literal)
{
	support::Arena arena;
	support::StringPool pool;
	AstPrinter printer;
	Expr* e = arena.create<StringLiteralExpr>(loc(), pool.intern("hi"));
	CHECK_EQ(printer.print(*e), "\"hi\"");
}

// ---- names -----------------------------------------------------------------------------------

TEST(ast_printer, name_expr)
{
	support::Arena arena;
	AstPrinter printer;
	Expr* e = arena.create<NameExpr>(loc(), std::string_view("foo"));
	CHECK_EQ(printer.print(*e), "foo");
}

// ---- unary (prefix and postfix) -----------------------------------------------------------

TEST(ast_printer, unary_prefix_ops)
{
	support::Arena arena;
	AstPrinter printer;
	Expr* x = arena.create<NameExpr>(loc(), std::string_view("x"));

	CHECK_EQ(printer.print(*arena.create<UnaryExpr>(loc(), UnaryOp::AddressOf, x)), "(& x)");
	CHECK_EQ(printer.print(*arena.create<UnaryExpr>(loc(), UnaryOp::Deref, x)), "(* x)");
	CHECK_EQ(printer.print(*arena.create<UnaryExpr>(loc(), UnaryOp::Negate, x)), "(- x)");
	CHECK_EQ(printer.print(*arena.create<UnaryExpr>(loc(), UnaryOp::LogicalNot, x)), "(! x)");
	CHECK_EQ(printer.print(*arena.create<UnaryExpr>(loc(), UnaryOp::BitwiseNot, x)), "(~ x)");
	CHECK_EQ(printer.print(*arena.create<UnaryExpr>(loc(), UnaryOp::PreIncrement, x)), "(++ x)");
	CHECK_EQ(printer.print(*arena.create<UnaryExpr>(loc(), UnaryOp::PreDecrement, x)), "(-- x)");
}

TEST(ast_printer, unary_postfix_ops_are_distinguishable_from_prefix)
{
	support::Arena arena;
	AstPrinter printer;
	Expr* x = arena.create<NameExpr>(loc(), std::string_view("x"));

	CHECK_EQ(printer.print(*arena.create<UnaryExpr>(loc(), UnaryOp::PostIncrement, x)), "(post++ x)");
	CHECK_EQ(printer.print(*arena.create<UnaryExpr>(loc(), UnaryOp::PostDecrement, x)), "(post-- x)");
}

// ---- binary ------------------------------------------------------------------------------------

TEST(ast_printer, binary_op)
{
	support::Arena arena;
	AstPrinter printer;
	Expr* a = arena.create<NameExpr>(loc(), std::string_view("a"));
	Expr* b = arena.create<NameExpr>(loc(), std::string_view("b"));
	Expr* e = arena.create<BinaryExpr>(loc(), BinaryOp::Add, a, b);
	CHECK_EQ(printer.print(*e), "(+ a b)");
}

TEST(ast_printer, binary_nesting_reflects_tree_shape)
{
	support::Arena arena;
	AstPrinter printer;
	Expr* a = arena.create<NameExpr>(loc(), std::string_view("a"));
	Expr* b = arena.create<NameExpr>(loc(), std::string_view("b"));
	Expr* c = arena.create<NameExpr>(loc(), std::string_view("c"));
	Expr* inner = arena.create<BinaryExpr>(loc(), BinaryOp::Mul, b, c);
	Expr* outer = arena.create<BinaryExpr>(loc(), BinaryOp::Add, a, inner);
	CHECK_EQ(printer.print(*outer), "(+ a (* b c))");
}

// ---- assignment --------------------------------------------------------------------------------

TEST(ast_printer, assign_op)
{
	support::Arena arena;
	AstPrinter printer;
	Expr* a = arena.create<NameExpr>(loc(), std::string_view("a"));
	Expr* b = arena.create<NameExpr>(loc(), std::string_view("b"));
	CHECK_EQ(printer.print(*arena.create<AssignExpr>(loc(), AssignOp::Assign, a, b)), "(= a b)");
	CHECK_EQ(printer.print(*arena.create<AssignExpr>(loc(), AssignOp::AddAssign, a, b)), "(+= a b)");
}

// ---- index --------------------------------------------------------------------------------------

TEST(ast_printer, index_expr)
{
	support::Arena arena;
	AstPrinter printer;
	Expr* a = arena.create<NameExpr>(loc(), std::string_view("a"));
	Expr* i = arena.create<IntLiteralExpr>(loc(), (u64)0);
	CHECK_EQ(printer.print(*arena.create<IndexExpr>(loc(), a, i)), "(index a 0)");
}

// ---- member (dot vs arrow) -----------------------------------------------------------------

TEST(ast_printer, member_dot_and_arrow_print_differently)
{
	support::Arena arena;
	AstPrinter printer;
	Expr* obj = arena.create<NameExpr>(loc(), std::string_view("obj"));

	CHECK_EQ(printer.print(*arena.create<MemberExpr>(loc(), obj, std::string_view("field"), false)), "(. obj field)");
	CHECK_EQ(printer.print(*arena.create<MemberExpr>(loc(), obj, std::string_view("field"), true)), "(-> obj field)");
}

// ---- call ---------------------------------------------------------------------------------------

TEST(ast_printer, call_with_no_arguments)
{
	support::Arena arena;
	AstPrinter printer;
	Expr* callee = arena.create<NameExpr>(loc(), std::string_view("f"));
	Expr* call = arena.create<CallExpr>(loc(), callee, std::span<Expr* const>{});
	CHECK_EQ(printer.print(*call), "(call f)");
}

TEST(ast_printer, call_with_arguments)
{
	support::Arena arena;
	AstPrinter printer;
	Expr* callee = arena.create<NameExpr>(loc(), std::string_view("f"));
	Expr* a1 = arena.create<IntLiteralExpr>(loc(), (u64)1);
	Expr* a2 = arena.create<IntLiteralExpr>(loc(), (u64)2);
	Expr* args[] = { a1, a2 };
	Expr* call = arena.create<CallExpr>(loc(), callee, std::span<Expr* const>(args, 2));
	CHECK_EQ(printer.print(*call), "(call f 1 2)");
}

// ---- cast ---------------------------------------------------------------------------------------

TEST(ast_printer, cast_to_primitive)
{
	support::Arena arena;
	AstPrinter printer;
	Expr* x = arena.create<NameExpr>(loc(), std::string_view("x"));
	Expr* cast = arena.create<CastExpr>(loc(), &Type::Int, x);
	CHECK_EQ(printer.print(*cast), "(cast int x)");
}

TEST(ast_printer, cast_to_pointer)
{
	support::Arena arena;
	AstPrinter printer;
	Expr* x = arena.create<NameExpr>(loc(), std::string_view("x"));
	const Type* intPtr = Type::makePointer(arena, &Type::Int);
	Expr* cast = arena.create<CastExpr>(loc(), intPtr, x);
	CHECK_EQ(printer.print(*cast), "(cast int* x)");
}

TEST(ast_printer, cast_sets_expr_type_to_the_target_type)
{
	support::Arena arena;
	Expr* x = arena.create<NameExpr>(loc(), std::string_view("x"));
	Expr* cast = arena.create<CastExpr>(loc(), &Type::UInt, x);
	CHECK(cast->type() == &Type::UInt);
}

// ---- sizeof --------------------------------------------------------------------------------------

TEST(ast_printer, sizeof_of_a_type)
{
	support::Arena arena;
	AstPrinter printer;
	Expr* e = arena.create<SizeofExpr>(loc(), &Type::Int);
	CHECK_EQ(printer.print(*e), "(sizeof int)");
}

TEST(ast_printer, sizeof_of_an_expression)
{
	support::Arena arena;
	AstPrinter printer;
	Expr* x = arena.create<NameExpr>(loc(), std::string_view("x"));
	Expr* e = arena.create<SizeofExpr>(loc(), x);
	CHECK_EQ(printer.print(*e), "(sizeof x)");
}

// ---- degenerate input -----------------------------------------------------------------------

TEST(ast_printer, null_child_prints_as_a_placeholder_instead_of_crashing)
{
	support::Arena arena;
	AstPrinter printer;
	Expr* e = arena.create<UnaryExpr>(loc(), UnaryOp::Negate, nullptr);
	CHECK_EQ(printer.print(*e), "(- <null>)");
}
