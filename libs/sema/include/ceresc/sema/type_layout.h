#pragma once

#include <ceresc/support/diagnostics.h>
#include <ceresc/support/types.h>

// struct layout: each field aligned to its own size, the total size rounded up to the widest
// field - the same rule CASM's own `struct` follows (documented in CeresASM's
// docs/23-Structs.md), so a Ceres-C struct and the CASM struct codegen emits for it describe
// exactly the same memory.
//
// Type::sizeInBytes()/alignment() (libs/ast/type.h) already compute a struct's overall size and
// alignment by walking StructDecl::fields() with this same rule - they do not need this library,
// since a StructDecl's fields() are already fully resolved by the time sema runs. What actually
// belongs here instead: the *per-field byte offset* (fieldOffset() - needed once sema starts
// resolving `.`/`->` on a MemberExpr, and later by codegen to emit the right ldr/str/lda), and
// completeness/cycle validation (validateStructLayout() - "does every field of this struct have a
// storable, non-recursive type" is a semantic question, not a structural one Type itself can
// answer safely without a diagnostics sink to report through).
//
// fieldOffset() recomputes by walking fields 0..index on every call rather than caching a
// per-field offset table on FieldDecl: struct field lists in this C subset are small, an
// arena-allocated FieldDecl has no mutable-annotation slot the way Expr::setType() does (see
// decl.h), and adding one would mean widening StructDecl::fields()'s span from const to
// non-const - not worth it for what is, at V1's scale, a handful of iterations per lookup.
//
// See the architecture plan, §8.
//
// Implemented in Fase 4 of the phased plan (§13).

namespace ceresc::ast
{
	class StructDecl;
}

namespace ceresc::sema
{
	// The byte offset of `decl`'s field at `fieldIndex`, following the rule described above.
	// `fieldIndex` must be < decl.fields().size() - out of range returns the raw byte count used
	// by the fields walked so far (i.e. the offset immediately after the last field's own bytes),
	// with no alignment applied for a hypothetical next field, since fieldOffset() doesn't know
	// what type that field would be.
	u32 fieldOffset(const ast::StructDecl& decl, u32 fieldIndex) noexcept;

	// Checks that every field of `decl` has a complete, storable type: not `void`, not an
	// incomplete struct/enum (a tag that was only ever forward-declared), and not a
	// direct-or-indirect by-value cycle back to `decl` itself, including through an array (always
	// illegal in C - a self-referential struct is only legal through a pointer, see decl.h's own
	// note on StructDecl). Reports one diagnostic per offending field and returns false if any was
	// found. A forward declaration (isComplete() == false) has nothing to validate yet and always
	// returns true. Only reads `decl` - never mutates it.
	bool validateStructLayout(support::DiagnosticEngine& diagnostics, const ast::StructDecl& decl) noexcept;
}
