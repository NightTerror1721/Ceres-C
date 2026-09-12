#pragma once

// TypeChecker - walks the parser's AST (which does not yet know whether `x` exists, or what type
// anything has) and produces the same AST, annotated: every Expr with its resolved Type*, every
// VarDecl with its frame offset or global address.
//
// Never stops at the first type error - it assumes the best type it can infer (usually int) and
// keeps walking, so the rest of the file still gets checked. All diagnostics accumulate in the
// same DiagnosticEngine (libs/support). See the architecture plan, §8.
//
// Implemented in Fase 4 of the phased plan (§13).

namespace ceresc::sema
{
}
