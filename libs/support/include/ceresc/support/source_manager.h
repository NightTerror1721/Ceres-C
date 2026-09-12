#pragma once

// SourceManager - owns every source file's full buffer and hands out stable std::string_view
// slices over it.
//
// Tokens and AST nodes never copy their lexeme, they view into this buffer (same convention as
// ceres::casm::Token in CeresASM). Also translates a byte offset back to line/column for
// diagnostics.h. See the architecture plan, §4.
//
// Implemented in Fase 0 of the phased plan (§13).

namespace ceresc::support
{
}
