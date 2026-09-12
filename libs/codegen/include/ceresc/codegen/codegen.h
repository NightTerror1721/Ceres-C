#pragma once

// CodeGen - IR -> CASM text, instruction by instruction (§10 of the architecture plan).
//
// The only library that knows the Ceres ABI, the real ISA and .casm syntax - everything upstream
// of it is target-agnostic. Register allocation rule, in full: every variable lives in memory
// (a frame slot or a global address), never in a register across statements; r4-r7 and r12 are
// used only as scratch temporaries while evaluating a single expression and reloaded from memory
// afterward. Any temporary still live after a `call` is spilled to a frame slot right before the
// call and reloaded right after, since `call` clobbers r0-r7, r12, f0-f7, at and the flags (ABI).
//
// Implemented in Fase 6 (scalar expressions/functions) and Fase 7 (arrays, pointers, structs) of
// the phased plan (§13).

namespace ceresc::codegen
{
}
