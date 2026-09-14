#pragma once

// Type - the C subset's type system: Void, Char, Short, Int, UInt, Float, Bool, Pointer, Array,
// Struct.
//
// Unlike the AST node hierarchy in this same library, Type uses std::variant on purpose: it's a
// small, genuinely closed set that does not grow the way the AST does across phases. Carries
// sizeInBytes()/alignment()/isSigned() - alignment follows the same rule as CASM's own `struct`
// (each field aligned to its own size, see §8). See the architecture plan, §6.
//
// Float and Bool are V1 scope, not yet reflected in this list of members - ordinary phased work
// (the lexer already has KwFloat/KwBool/LiteralFloat/LiteralBool, see token.h), this variant just
// hasn't grown to include them yet. Float is `float` only (f32): it maps directly onto Ceres's
// native F32 register/DataType (see CeresASM's fregisters.h/data_type.h), so codegen is close to a
// straight passthrough. `double` (f64) is a different story and stays reserved for a version after
// v1: Ceres has no f64 support anywhere in the VM, so it would need real work (software emulation)
// instead of being free like float. Once the parser exists, it must reject `double` with a "not
// implemented in this version" diagnostic rather than trying to fit it in here early. `const` is a
// qualifier on a Type, not its own TypeKind member - how it attaches (a bit on Type vs. a wrapper)
// is still open, see §14.
//
// This variant is also narrower than the integer keywords the lexer already recognizes: `short`/
// `long`/`signed`/`unsigned` are meant to combine the way they do in real C (see token.h), which
// this enumeration doesn't reflect yet (no Long/ULong, and no separate signedness for Char/Short).
// It needs to grow when the parser's type-name grammar lands to cover every valid combination -
// and to reject the invalid ones (e.g. `short long`) - rather than staying stuck at today's shape.
//
// Implemented across Fase 2-4 of the phased plan (§13): the shape lands with the parser's
// type-name grammar, sizeInBytes()/alignment() with sema's struct layout.

namespace ceresc::ast
{
}
