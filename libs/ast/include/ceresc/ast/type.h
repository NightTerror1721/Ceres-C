#pragma once

// Type - the C subset's type system: Void, Char, Short, Int, UInt, Pointer, Array, Struct.
//
// Unlike the AST node hierarchy in this same library, Type uses std::variant on purpose: it's a
// small, genuinely closed set that does not grow the way the AST does across phases. Carries
// sizeInBytes()/alignment()/isSigned() - alignment follows the same rule as CASM's own `struct`
// (each field aligned to its own size, see §8). See the architecture plan, §6.
//
// Implemented across Fase 2-4 of the phased plan (§13): the shape lands with the parser's
// type-name grammar, sizeInBytes()/alignment() with sema's struct layout.

namespace ceresc::ast
{
}
