#pragma once

// Expr hierarchy: IntLiteralExpr, CharLiteralExpr, StringLiteralExpr, NameExpr, CallExpr,
// UnaryExpr, BinaryExpr, AssignExpr, IndexExpr, MemberExpr, CastExpr.
//
// Every Expr carries a Type* (type.h), resolved by sema (libs/sema) and initially null out of the
// parser. See the architecture plan, §6.
//
// Implemented in Fase 2 of the phased plan (§13).

namespace ceresc::ast
{
}
