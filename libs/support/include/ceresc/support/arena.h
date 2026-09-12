#pragma once

// Arena - a bump allocator over 64 KiB blocks.
//
// Every AST node and every IR instruction is allocated here, never with an individual `new`. Its
// lifetime is the whole translation unit: it is torn down all at once when a file finishes
// compiling, so no AST/IR node needs its own destructor. See the architecture plan, §4.
//
// Implemented in Fase 0 of the phased plan (§13).

namespace ceresc::support
{
}
