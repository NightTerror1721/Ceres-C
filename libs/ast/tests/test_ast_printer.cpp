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

// ---- ternary --------------------------------------------------------------------------------

TEST(ast_printer, ternary_expr)
{
	support::Arena arena;
	AstPrinter printer;
	Expr* cond = arena.create<NameExpr>(loc(), std::string_view("a"));
	Expr* thenExpr = arena.create<NameExpr>(loc(), std::string_view("b"));
	Expr* elseExpr = arena.create<NameExpr>(loc(), std::string_view("c"));
	Expr* e = arena.create<TernaryExpr>(loc(), cond, thenExpr, elseExpr);
	CHECK_EQ(printer.print(*e), "(?: a b c)");
}

// ---- statements -------------------------------------------------------------------------------

TEST(ast_printer, empty_stmt)
{
	support::Arena arena;
	AstPrinter printer;
	Stmt* s = arena.create<EmptyStmt>(loc());
	CHECK_EQ(printer.print(*s), "(empty)");
}

TEST(ast_printer, expr_stmt)
{
	support::Arena arena;
	AstPrinter printer;
	Expr* x = arena.create<NameExpr>(loc(), std::string_view("x"));
	Stmt* s = arena.create<ExprStmt>(loc(), x);
	CHECK_EQ(printer.print(*s), "(expr-stmt x)");
}

TEST(ast_printer, decl_stmt)
{
	support::Arena arena;
	AstPrinter printer;
	Decl* d = arena.create<VarDecl>(loc(), std::string_view("x"), &Type::Int);
	Stmt* s = arena.create<DeclStmt>(loc(), d);
	CHECK_EQ(printer.print(*s), "(decl-stmt (var x int <null>))");
}

TEST(ast_printer, compound_stmt_empty)
{
	support::Arena arena;
	AstPrinter printer;
	Stmt* s = arena.create<CompoundStmt>(loc(), std::span<Stmt* const>{});
	CHECK_EQ(printer.print(*s), "(block)");
}

TEST(ast_printer, compound_stmt_with_statements)
{
	support::Arena arena;
	AstPrinter printer;
	Stmt* s1 = arena.create<EmptyStmt>(loc());
	Stmt* s2 = arena.create<EmptyStmt>(loc());
	Stmt* stmts[] = { s1, s2 };
	Stmt* block = arena.create<CompoundStmt>(loc(), std::span<Stmt* const>(stmts, 2));
	CHECK_EQ(printer.print(*block), "(block (empty) (empty))");
}

TEST(ast_printer, if_stmt_without_else)
{
	support::Arena arena;
	AstPrinter printer;
	Expr* cond = arena.create<NameExpr>(loc(), std::string_view("a"));
	Stmt* thenStmt = arena.create<EmptyStmt>(loc());
	Stmt* s = arena.create<IfStmt>(loc(), cond, thenStmt);
	CHECK_EQ(printer.print(*s), "(if a (empty))");
}

TEST(ast_printer, if_stmt_with_else)
{
	support::Arena arena;
	AstPrinter printer;
	Expr* cond = arena.create<NameExpr>(loc(), std::string_view("a"));
	Stmt* thenStmt = arena.create<EmptyStmt>(loc());
	Stmt* elseStmt = arena.create<EmptyStmt>(loc());
	Stmt* s = arena.create<IfStmt>(loc(), cond, thenStmt, elseStmt);
	CHECK_EQ(printer.print(*s), "(if a (empty) (empty))");
}

TEST(ast_printer, while_stmt)
{
	support::Arena arena;
	AstPrinter printer;
	Expr* cond = arena.create<NameExpr>(loc(), std::string_view("a"));
	Stmt* body = arena.create<EmptyStmt>(loc());
	Stmt* s = arena.create<WhileStmt>(loc(), cond, body);
	CHECK_EQ(printer.print(*s), "(while a (empty))");
}

TEST(ast_printer, do_while_stmt)
{
	support::Arena arena;
	AstPrinter printer;
	Expr* cond = arena.create<NameExpr>(loc(), std::string_view("a"));
	Stmt* body = arena.create<EmptyStmt>(loc());
	Stmt* s = arena.create<DoWhileStmt>(loc(), body, cond);
	CHECK_EQ(printer.print(*s), "(do-while (empty) a)");
}

TEST(ast_printer, for_stmt_with_all_clauses)
{
	support::Arena arena;
	AstPrinter printer;
	Expr* i = arena.create<NameExpr>(loc(), std::string_view("i"));
	Stmt* init = arena.create<ExprStmt>(loc(), i);
	Expr* cond = arena.create<NameExpr>(loc(), std::string_view("c"));
	Expr* incr = arena.create<NameExpr>(loc(), std::string_view("n"));
	Stmt* body = arena.create<EmptyStmt>(loc());
	Stmt* s = arena.create<ForStmt>(loc(), init, cond, incr, body);
	CHECK_EQ(printer.print(*s), "(for (expr-stmt i) c n (empty))");
}

TEST(ast_printer, for_stmt_with_no_clauses)
{
	support::Arena arena;
	AstPrinter printer;
	Stmt* body = arena.create<EmptyStmt>(loc());
	Stmt* s = arena.create<ForStmt>(loc(), nullptr, nullptr, nullptr, body);
	CHECK_EQ(printer.print(*s), "(for <null> <null> <null> (empty))");
}

TEST(ast_printer, return_stmt_with_value)
{
	support::Arena arena;
	AstPrinter printer;
	Expr* x = arena.create<NameExpr>(loc(), std::string_view("x"));
	Stmt* s = arena.create<ReturnStmt>(loc(), x);
	CHECK_EQ(printer.print(*s), "(return x)");
}

TEST(ast_printer, return_stmt_without_value)
{
	support::Arena arena;
	AstPrinter printer;
	Stmt* s = arena.create<ReturnStmt>(loc());
	CHECK_EQ(printer.print(*s), "(return <null>)");
}

TEST(ast_printer, break_and_continue_stmt)
{
	support::Arena arena;
	AstPrinter printer;
	CHECK_EQ(printer.print(*arena.create<BreakStmt>(loc())), "(break)");
	CHECK_EQ(printer.print(*arena.create<ContinueStmt>(loc())), "(continue)");
}

TEST(ast_printer, switch_case_default_stmt)
{
	support::Arena arena;
	AstPrinter printer;
	Expr* cond = arena.create<NameExpr>(loc(), std::string_view("x"));
	Stmt* body = arena.create<EmptyStmt>(loc());
	CHECK_EQ(printer.print(*arena.create<SwitchStmt>(loc(), cond, body)), "(switch x (empty))");

	Expr* value = arena.create<IntLiteralExpr>(loc(), (u64)1);
	CHECK_EQ(printer.print(*arena.create<CaseStmt>(loc(), value, body)), "(case 1 (empty))");

	CHECK_EQ(printer.print(*arena.create<DefaultStmt>(loc(), body)), "(default (empty))");
}

TEST(ast_printer, goto_and_label_stmt)
{
	support::Arena arena;
	AstPrinter printer;
	CHECK_EQ(printer.print(*arena.create<GotoStmt>(loc(), std::string_view("end"))), "(goto end)");

	Stmt* body = arena.create<EmptyStmt>(loc());
	CHECK_EQ(printer.print(*arena.create<LabelStmt>(loc(), std::string_view("end"), body)), "(label end (empty))");
}

// ---- declarations -----------------------------------------------------------------------------

TEST(ast_printer, var_decl_without_initializer)
{
	support::Arena arena;
	AstPrinter printer;
	Decl* d = arena.create<VarDecl>(loc(), std::string_view("x"), &Type::Int);
	CHECK_EQ(printer.print(*d), "(var x int <null>)");
}

TEST(ast_printer, var_decl_with_initializer)
{
	support::Arena arena;
	AstPrinter printer;
	Expr* init = arena.create<IntLiteralExpr>(loc(), (u64)5);
	Decl* d = arena.create<VarDecl>(loc(), std::string_view("x"), &Type::Int, init);
	CHECK_EQ(printer.print(*d), "(var x int 5)");
}

TEST(ast_printer, function_decl_prototype_with_no_params)
{
	support::Arena arena;
	AstPrinter printer;
	Decl* d = arena.create<FunctionDecl>(loc(), std::string_view("foo"), &Type::Int, std::span<const Param>{});
	CHECK_EQ(printer.print(*d), "(func foo int (params) <null>)");
}

TEST(ast_printer, function_decl_definition_with_params)
{
	support::Arena arena;
	AstPrinter printer;
	Param params[] = {
		Param{ &Type::Int, std::string_view("x"), loc() },
		Param{ &Type::Float, std::string_view("y"), loc() },
	};
	Stmt* body = arena.create<CompoundStmt>(loc(), std::span<Stmt* const>{});
	Decl* d = arena.create<FunctionDecl>(loc(), std::string_view("foo"), &Type::Int, std::span<const Param>(params, 2), static_cast<CompoundStmt*>(body));
	CHECK_EQ(printer.print(*d), "(func foo int (params (int x) (float y)) (block))");
}

TEST(ast_printer, struct_decl_incomplete)
{
	support::Arena arena;
	AstPrinter printer;
	Decl* d = arena.create<StructDecl>(loc(), std::string_view("Node"));
	CHECK_EQ(printer.print(*d), "(struct Node <incomplete>)");
}

TEST(ast_printer, struct_decl_with_fields)
{
	support::Arena arena;
	AstPrinter printer;
	StructDecl* d = arena.create<StructDecl>(loc(), std::string_view("Point"));
	FieldDecl fields[] = {
		FieldDecl{ &Type::Int, std::string_view("x"), loc() },
		FieldDecl{ &Type::Int, std::string_view("y"), loc() },
	};
	d->setFields(std::span<const FieldDecl>(fields, 2));
	CHECK_EQ(printer.print(*static_cast<Decl*>(d)), "(struct Point (fields (int x) (int y)))");
}

TEST(ast_printer, enum_decl_incomplete)
{
	support::Arena arena;
	AstPrinter printer;
	Decl* d = arena.create<EnumDecl>(loc(), std::string_view("Color"));
	CHECK_EQ(printer.print(*d), "(enum Color <incomplete>)");
}

TEST(ast_printer, enum_decl_with_enumerators)
{
	support::Arena arena;
	AstPrinter printer;
	EnumDecl* d = arena.create<EnumDecl>(loc(), std::string_view("Color"));
	Expr* five = arena.create<IntLiteralExpr>(loc(), (u64)5);
	EnumeratorDecl enumerators[] = {
		EnumeratorDecl{ std::string_view("RED"), nullptr, loc() },
		EnumeratorDecl{ std::string_view("GREEN"), five, loc() },
	};
	d->setEnumerators(std::span<const EnumeratorDecl>(enumerators, 2));
	CHECK_EQ(printer.print(*static_cast<Decl*>(d)), "(enum Color (enumerators (RED) (GREEN 5)))");
}

TEST(ast_printer, typedef_decl)
{
	support::Arena arena;
	AstPrinter printer;
	Decl* d = arena.create<TypedefDecl>(loc(), std::string_view("MyInt"), &Type::Int);
	CHECK_EQ(printer.print(*d), "(typedef MyInt int)");
}

TEST(ast_printer, struct_and_enum_type_names_include_the_tag)
{
	support::Arena arena;
	StructDecl* structDecl = arena.create<StructDecl>(loc(), std::string_view("Point"));
	EnumDecl* enumDecl = arena.create<EnumDecl>(loc(), std::string_view("Color"));
	CHECK_EQ(AstPrinter::typeName(Type::makeStruct(arena, structDecl)), "struct Point");
	CHECK_EQ(AstPrinter::typeName(Type::makeEnum(arena, enumDecl)), "enum Color");
}

TEST(ast_printer, translation_unit)
{
	support::Arena arena;
	AstPrinter printer;
	Decl* d1 = arena.create<VarDecl>(loc(), std::string_view("x"), &Type::Int);
	Decl* d2 = arena.create<VarDecl>(loc(), std::string_view("y"), &Type::Float);
	Decl* decls[] = { d1, d2 };
	TranslationUnit* unit = arena.create<TranslationUnit>(loc(), std::span<Decl* const>(decls, 2));
	CHECK_EQ(printer.print(*unit), "(unit (var x int <null>) (var y float <null>))");
}
