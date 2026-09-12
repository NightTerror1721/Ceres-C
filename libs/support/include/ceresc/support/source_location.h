#pragma once

// SourceLocation - a trivial, non-owning {fileId, line, column, offset} value.
//
// Every Token (libs/lexer) and every AST node (libs/ast) carries one of these so diagnostics.h
// can point at exactly where they came from, and so codegen can cite the originating C line in
// its comments. See the architecture plan, §4.
//
// Implemented in Fase 0 of the phased plan (§13): a comparable (<=>) aggregate, so a
// DiagnosticEngine can sort its collected diagnostics by position.

namespace ceresc::support
{
}
