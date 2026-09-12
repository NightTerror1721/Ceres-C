#pragma once

// Symbol, SymbolKind and Scope - a chain of lexical scopes (global -> function -> block, nested
// per `{ }`), each an unordered_map<string_view, Symbol> with a pointer to its parent.
//
// See the architecture plan, §8.
//
// Implemented in Fase 4 of the phased plan (§13).

namespace ceresc::sema
{
}
