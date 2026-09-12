#pragma once

// struct layout: each field aligned to its own size, the total size rounded up to the widest
// field - the same rule CASM's own `struct` follows (documented in CeresASM's
// docs/23-Structs.md), so a Ceres-C struct and the CASM struct codegen emits for it describe
// exactly the same memory.
//
// See the architecture plan, §8.
//
// Implemented in Fase 4 of the phased plan (§13).

namespace ceresc::sema
{
}
