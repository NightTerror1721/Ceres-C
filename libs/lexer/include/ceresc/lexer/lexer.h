#pragma once

// Lexer - a cursor over std::string_view that produces one Token per call to next(), with a
// single character of pushback via peek() (the grammar in §3 never needs more).
//
// Does not know the grammar: it knows that `if` is the keyword If, not that `if` needs
// parentheses. An unrecognized character does not stop the lexer - it reports a diagnostic,
// yields Token::Invalid and advances one character, so the parser can keep going. See the
// architecture plan, §5.
//
// Implemented in Fase 1 of the phased plan (§13).

namespace ceresc::lexer
{
}
