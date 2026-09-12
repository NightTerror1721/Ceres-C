#pragma once

// Decl hierarchy: FunctionDecl, VarDecl, StructDecl, ParamDecl.
//
// A classic polymorphic hierarchy with double-dispatch (ast_visitor.h), not a std::variant - the
// AST keeps growing across phases and each node needs its own behavior, which is exactly the
// "understandable before complete" criterion of §0 applied to this library. See the architecture
// plan, §6.
//
// Implemented in Fase 3 of the phased plan (§13).

namespace ceresc::ast
{
}
