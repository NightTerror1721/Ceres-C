#pragma once

// CasmEmitter - deliberately dumb: accumulates indented lines of .casm text, no builder and no
// output-side AST of its own.
//
// Every emitted instruction carries a comment citing the originating C line (`; file.c:12`),
// taken from the SourceLocation each IR node keeps from the AST - this is what makes the output
// double as teaching material, the stated goal of option A over option B in the prior audit. See
// the architecture plan, §10.
//
// Implemented in Fase 6 of the phased plan (§13).

namespace ceresc::codegen
{
}
