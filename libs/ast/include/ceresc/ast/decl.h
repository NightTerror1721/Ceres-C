#pragma once

// Decl hierarchy: FunctionDecl, VarDecl, StructDecl, ParamDecl.
//
// A classic polymorphic hierarchy with double-dispatch (ast_visitor.h), not a std::variant - the
// AST keeps growing across phases and each node needs its own behavior, which is exactly the
// "understandable before complete" criterion of §0 applied to this library. See the architecture
// plan, §6.
//
// EnumDecl and TypedefDecl are V1 scope too (the lexer already has KwEnum/KwTypedef, see token.h) -
// ordinary phased work, not a language restriction; this file grows to six forms when their phase
// lands. `union` is different: it stays reserved for a version after v1 (overlapping storage needs
// real layout work sema/codegen don't have yet, see type.h) - once the parser exists, it must
// reject `union` with a "not implemented in this version" diagnostic instead of guessing at
// UnionDecl's shape early.
//
// Implemented in Fase 3 of the phased plan (§13).

namespace ceresc::ast
{
}
