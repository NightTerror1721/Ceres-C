#pragma once

#include "expr.h"

// AstVisitor - classic double dispatch: every node implements accept(AstVisitor&), which calls
// visitor.visit(*this).
//
// Three concrete consumers use it across the pipeline: AstPrinter (--emit-ast), TypeChecker
// (libs/sema) and IrBuilder (libs/ir) - none of them needs a giant switch over a node-kind enum.
// See the architecture plan, §6.
//
// Unlike the Expr hierarchy itself, AstVisitor is never Arena-allocated (its concrete
// implementations are ordinary stack/heap objects owned by whoever runs a pass over the tree), so
// it carries a real virtual destructor - the trivially-destructible constraint in expr.h has
// nothing to do with this class.
//
// Every accept() body lives here, not in expr.h: accept() needs AstVisitor to be a complete type
// (to call visitor.visit(*this)), and AstVisitor needs every Expr subclass to be a complete type
// (to declare visit(SomeExpr&)) - this file is the one place both are available together. expr.h
// only forward-declares AstVisitor for that reason.
//
// Implemented in Fase 2-3 of the phased plan (§13), growing a visit() overload as each node kind
// lands - Stmt/Decl overloads join this list in Fase 3.

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

		// Stmt/Decl overloads land here in Fase 3, one per node kind, same pattern.
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
}
