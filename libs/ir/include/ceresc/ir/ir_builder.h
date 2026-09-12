#pragma once

// IrBuilder - an AstVisitor (libs/ast) that lowers the annotated AST from sema into IR.
//
// Every visited Expr returns an IrValue (a temporary or a constant); every visited Stmt appends
// instructions to the current basic block and, for control flow (if/while/for), creates the
// child blocks and updates what "the current block" is for what follows. && and || lower to
// control flow directly, never to a plain BinOp, so the right-hand side is never evaluated once
// the left side already decides the result. See the architecture plan, §9.
//
// Implemented in Fase 5 of the phased plan (§13). IrPrinter (--emit-ir) ships alongside it.

namespace ceresc::ir
{
}
