#pragma once

// Stmt hierarchy: CompoundStmt, IfStmt, WhileStmt, ForStmt, ReturnStmt, BreakStmt, ContinueStmt,
// DeclStmt, ExprStmt - the nine statement forms of the grammar in §3.
//
// See the architecture plan, §6.
//
// SwitchStmt, DoWhileStmt and GotoStmt are V1 scope too (the lexer already has KwSwitch/KwCase/
// KwDefault/KwGoto/KwDo, see token.h) - they just haven't had their phase yet, same as everything
// else here before Fase 3 lands. This file needs to grow to twelve forms when that happens; until
// then, the parser simply doesn't have a production for them - there is nothing to reject, they're
// no different from `if`/`while` not existing yet before this file itself was written.
//
// Implemented in Fase 3 of the phased plan (§13).

namespace ceresc::ast
{
}
