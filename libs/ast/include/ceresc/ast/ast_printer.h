#pragma once

#include "ast_visitor.h"
#include <string>

// AstPrinter - renders an Expr/Stmt/Decl tree as a flat, parenthesized text form (a small
// S-expression dialect), e.g. `1 + 2 * 3` prints as `(+ 1 (* 2 3))`.
//
// Two reasons this exists this early, before sema or codegen: it's the compiler's own
// --emit-ast debugging output (§6), and it's what libs/parser's tests compare against - neither
// Expr nor Type has a std::formatter, so a parser test can't CHECK_EQ two trees directly. Printing
// both to text and comparing strings is the same trick test_lexer.cpp already uses for tokens.
//
// Not constexpr/noexcept: it builds a std::string, and StringLiteralExpr's PooledString read alone
// already rules out constexpr - this is a runtime debugging/testing tool, not a hot path.
//
// Implemented in Fase 2-3 of the phased plan (§13): Expr coverage landed in Fase 2, Stmt/Decl/
// TranslationUnit coverage in Fase 3.

namespace ceresc::ast
{
	class AstPrinter final : public AstVisitor
	{
	private:
		std::string _output;

	public:
		AstPrinter() = default;
		AstPrinter(const AstPrinter&) = delete;
		AstPrinter(AstPrinter&&) = delete;
		~AstPrinter() override = default;

		AstPrinter& operator=(const AstPrinter&) = delete;
		AstPrinter& operator=(AstPrinter&&) = delete;

	public:
		// Resets internal state and returns the printed form of `root`.
		std::string print(Expr& root);
		std::string print(Stmt& root);
		std::string print(Decl& root);
		std::string print(TranslationUnit& root);
		// Same, but for a possibly-null child - prints `<null>` instead of dereferencing.
		std::string print(Expr* root);
		std::string print(Stmt* root);
		std::string print(Decl* root);

	public:
		void visit(IntLiteralExpr& node) override;
		void visit(FloatLiteralExpr& node) override;
		void visit(CharLiteralExpr& node) override;
		void visit(BoolLiteralExpr& node) override;
		void visit(StringLiteralExpr& node) override;
		void visit(NameExpr& node) override;
		void visit(CallExpr& node) override;
		void visit(UnaryExpr& node) override;
		void visit(BinaryExpr& node) override;
		void visit(AssignExpr& node) override;
		void visit(IndexExpr& node) override;
		void visit(MemberExpr& node) override;
		void visit(CastExpr& node) override;
		void visit(SizeofExpr& node) override;
		void visit(TernaryExpr& node) override;

		void visit(EmptyStmt& node) override;
		void visit(ExprStmt& node) override;
		void visit(DeclStmt& node) override;
		void visit(CompoundStmt& node) override;
		void visit(IfStmt& node) override;
		void visit(WhileStmt& node) override;
		void visit(DoWhileStmt& node) override;
		void visit(ForStmt& node) override;
		void visit(ReturnStmt& node) override;
		void visit(BreakStmt& node) override;
		void visit(ContinueStmt& node) override;
		void visit(SwitchStmt& node) override;
		void visit(CaseStmt& node) override;
		void visit(DefaultStmt& node) override;
		void visit(GotoStmt& node) override;
		void visit(LabelStmt& node) override;

		void visit(VarDecl& node) override;
		void visit(FunctionDecl& node) override;
		void visit(StructDecl& node) override;
		void visit(EnumDecl& node) override;
		void visit(TypedefDecl& node) override;
		void visit(TranslationUnit& node) override;

	private:
		void printChild(Expr* child);
		void printChild(Stmt* child);
		void printChild(Decl* child);
		void appendTypeName(const Type* type);

	public:
		static std::string typeName(const Type* type);
	};
}
