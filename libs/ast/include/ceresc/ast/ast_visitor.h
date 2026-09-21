#pragma once

#include "expr.h"
#include "stmt.h"
#include "decl.h"

// AstVisitor - classic double dispatch: every node implements accept(AstVisitor&), which calls
// visitor.visit(*this).
//
// Three concrete consumers use it across the pipeline: AstPrinter (--emit-ast), TypeChecker
// (libs/sema) and IrBuilder (libs/ir) - none of them needs a giant switch over a node-kind enum.
// See the architecture plan, §6.
//
// Unlike the Expr/Stmt/Decl hierarchies themselves, AstVisitor is never Arena-allocated (its
// concrete implementations are ordinary stack/heap objects owned by whoever runs a pass over the
// tree), so it carries a real virtual destructor - the trivially-destructible constraint in
// expr.h/stmt.h/decl.h has nothing to do with this class.
//
// Every accept() body lives here, not in expr.h/stmt.h/decl.h: accept() needs AstVisitor to be a
// complete type (to call visitor.visit(*this)), and AstVisitor needs every concrete node type to be
// a complete type (to declare visit(SomeNode&)) - this file is the one place all of them are
// available together. stmt.h and decl.h only forward-declare each other's pointer-only fields
// (Decl* in DeclStmt, CompoundStmt* in FunctionDecl) for exactly this reason - see their own header
// comments.
//
// Implemented in Fase 2-3 of the phased plan (§13): the Expr overloads landed in Fase 2, the
// Stmt/Decl overloads (plus TranslationUnit, which is not itself a Decl - see decl.h) in Fase 3.

namespace ceresc::ast
{
	class AstVisitor
	{
	public:
		virtual ~AstVisitor() = default;

	public:
		virtual void visit(IntLiteralExpr& node) = 0;
		virtual void visit(FloatLiteralExpr& node) = 0;
		virtual void visit(CharLiteralExpr& node) = 0;
		virtual void visit(BoolLiteralExpr& node) = 0;
		virtual void visit(StringLiteralExpr& node) = 0;
		virtual void visit(NameExpr& node) = 0;
		virtual void visit(CallExpr& node) = 0;
		virtual void visit(UnaryExpr& node) = 0;
		virtual void visit(BinaryExpr& node) = 0;
		virtual void visit(AssignExpr& node) = 0;
		virtual void visit(IndexExpr& node) = 0;
		virtual void visit(MemberExpr& node) = 0;
		virtual void visit(CastExpr& node) = 0;
		virtual void visit(SizeofExpr& node) = 0;
		virtual void visit(AlignofExpr& node) = 0;
		virtual void visit(VaExpr& node) = 0;
		virtual void visit(MachineOpExpr& node) = 0;
		virtual void visit(TernaryExpr& node) = 0;
		virtual void visit(InitListExpr& node) = 0;

		virtual void visit(EmptyStmt& node) = 0;
		virtual void visit(ExprStmt& node) = 0;
		virtual void visit(DeclStmt& node) = 0;
		virtual void visit(CompoundStmt& node) = 0;
		virtual void visit(IfStmt& node) = 0;
		virtual void visit(WhileStmt& node) = 0;
		virtual void visit(DoWhileStmt& node) = 0;
		virtual void visit(ForStmt& node) = 0;
		virtual void visit(ReturnStmt& node) = 0;
		virtual void visit(BreakStmt& node) = 0;
		virtual void visit(ContinueStmt& node) = 0;
		virtual void visit(SwitchStmt& node) = 0;
		virtual void visit(CaseStmt& node) = 0;
		virtual void visit(DefaultStmt& node) = 0;
		virtual void visit(GotoStmt& node) = 0;
		virtual void visit(LabelStmt& node) = 0;

		virtual void visit(VarDecl& node) = 0;
		virtual void visit(FunctionDecl& node) = 0;
		virtual void visit(StructDecl& node) = 0;
		virtual void visit(EnumDecl& node) = 0;
		virtual void visit(TypedefDecl& node) = 0;
		virtual void visit(InterruptVectorDecl& node) = 0;
		virtual void visit(StaticAssertDecl& node) = 0;
		virtual void visit(TranslationUnit& node) = 0;
	};

	inline void IntLiteralExpr::accept(AstVisitor& visitor) { visitor.visit(*this); }
	inline void FloatLiteralExpr::accept(AstVisitor& visitor) { visitor.visit(*this); }
	inline void CharLiteralExpr::accept(AstVisitor& visitor) { visitor.visit(*this); }
	inline void BoolLiteralExpr::accept(AstVisitor& visitor) { visitor.visit(*this); }
	inline void StringLiteralExpr::accept(AstVisitor& visitor) { visitor.visit(*this); }
	inline void NameExpr::accept(AstVisitor& visitor) { visitor.visit(*this); }
	inline void CallExpr::accept(AstVisitor& visitor) { visitor.visit(*this); }
	inline void UnaryExpr::accept(AstVisitor& visitor) { visitor.visit(*this); }
	inline void BinaryExpr::accept(AstVisitor& visitor) { visitor.visit(*this); }
	inline void AssignExpr::accept(AstVisitor& visitor) { visitor.visit(*this); }
	inline void IndexExpr::accept(AstVisitor& visitor) { visitor.visit(*this); }
	inline void MemberExpr::accept(AstVisitor& visitor) { visitor.visit(*this); }
	inline void CastExpr::accept(AstVisitor& visitor) { visitor.visit(*this); }
	inline void SizeofExpr::accept(AstVisitor& visitor) { visitor.visit(*this); }
	inline void AlignofExpr::accept(AstVisitor& visitor) { visitor.visit(*this); }
	inline void VaExpr::accept(AstVisitor& visitor) { visitor.visit(*this); }
	inline void MachineOpExpr::accept(AstVisitor& visitor) { visitor.visit(*this); }
	inline void TernaryExpr::accept(AstVisitor& visitor) { visitor.visit(*this); }
	inline void InitListExpr::accept(AstVisitor& visitor) { visitor.visit(*this); }

	inline void EmptyStmt::accept(AstVisitor& visitor) { visitor.visit(*this); }
	inline void ExprStmt::accept(AstVisitor& visitor) { visitor.visit(*this); }
	inline void DeclStmt::accept(AstVisitor& visitor) { visitor.visit(*this); }
	inline void CompoundStmt::accept(AstVisitor& visitor) { visitor.visit(*this); }
	inline void IfStmt::accept(AstVisitor& visitor) { visitor.visit(*this); }
	inline void WhileStmt::accept(AstVisitor& visitor) { visitor.visit(*this); }
	inline void DoWhileStmt::accept(AstVisitor& visitor) { visitor.visit(*this); }
	inline void ForStmt::accept(AstVisitor& visitor) { visitor.visit(*this); }
	inline void ReturnStmt::accept(AstVisitor& visitor) { visitor.visit(*this); }
	inline void BreakStmt::accept(AstVisitor& visitor) { visitor.visit(*this); }
	inline void ContinueStmt::accept(AstVisitor& visitor) { visitor.visit(*this); }
	inline void SwitchStmt::accept(AstVisitor& visitor) { visitor.visit(*this); }
	inline void CaseStmt::accept(AstVisitor& visitor) { visitor.visit(*this); }
	inline void DefaultStmt::accept(AstVisitor& visitor) { visitor.visit(*this); }
	inline void GotoStmt::accept(AstVisitor& visitor) { visitor.visit(*this); }
	inline void LabelStmt::accept(AstVisitor& visitor) { visitor.visit(*this); }

	inline void VarDecl::accept(AstVisitor& visitor) { visitor.visit(*this); }
	inline void FunctionDecl::accept(AstVisitor& visitor) { visitor.visit(*this); }
	inline void StructDecl::accept(AstVisitor& visitor) { visitor.visit(*this); }
	inline void EnumDecl::accept(AstVisitor& visitor) { visitor.visit(*this); }
	inline void TypedefDecl::accept(AstVisitor& visitor) { visitor.visit(*this); }
	inline void InterruptVectorDecl::accept(AstVisitor& visitor) { visitor.visit(*this); }
	inline void StaticAssertDecl::accept(AstVisitor& visitor) { visitor.visit(*this); }
	inline void TranslationUnit::accept(AstVisitor& visitor) { visitor.visit(*this); }
}
