#pragma once

// AstVisitor - classic double dispatch: every node implements accept(AstVisitor&), which calls
// visitor.visit(*this).
//
// Three concrete consumers use it across the pipeline: AstPrinter (--emit-ast), TypeChecker
// (libs/sema) and IrBuilder (libs/ir) - none of them needs a giant switch over a node-kind enum.
// See the architecture plan, §6.
//
// Implemented in Fase 2-3 of the phased plan (§13), growing a visit() overload as each node kind
// lands.

namespace ceresc::ast
{
}
