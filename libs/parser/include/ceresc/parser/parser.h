#pragma once

// Parser - Token[] -> AST (§7 of the architecture plan).
//
// Recursive descent for declarations and statements; precedence climbing for expressions (levels
// 2-11 of the precedence table share one parametrized function, only the assignment, cast, and
// prefix/postfix levels get dedicated ones). A syntax error does not abort the file: panic-mode
// recovery resynchronizes on the next `;`/`}` (inside a statement) or the next type keyword/`}`
// (at top level), so one pass reports every syntax error in a broken file, not just the first.
//
// Implemented in Fase 2 (expressions) and Fase 3 (declarations/statements) of the phased plan
// (§13).

namespace ceresc::parser
{
}
