#pragma once

// Expr hierarchy: IntLiteralExpr, FloatLiteralExpr, CharLiteralExpr, StringLiteralExpr, NameExpr,
// CallExpr, UnaryExpr, BinaryExpr, AssignExpr, IndexExpr, MemberExpr, CastExpr.
//
// MemberExpr covers both `.` and `->` as two genuinely distinct forms with real C semantics (the
// parser must not desugar `a->b` into `(*a).b`) - see token.h's Dot/Arrow.
//
// Every Expr carries a Type* (type.h), resolved by sema (libs/sema) and initially null out of the
// parser. See the architecture plan, §6.
//
// sizeof is V1 scope (the lexer already has KwSizeof, see token.h): SizeofExpr just hasn't had its
// phase yet, ordinary phased work like everything else in this file. `alignof` is different and
// stays reserved for a version after v1 (KwAlignof exists in the lexer's reserved group); once the
// parser exists, it must reject `alignof` with a "not implemented in this version" diagnostic
// instead of guessing at AlignofExpr's shape early.
//
// Implemented in Fase 2 of the phased plan (§13).

namespace ceresc::ast
{
}
