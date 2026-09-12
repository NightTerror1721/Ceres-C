#pragma once

// TokenKind, TokenValue and Token (§5 of the architecture plan).
//
// TokenValue is a small closed std::variant over the handful of literal value shapes a token can
// carry (u64 for IntLiteral, char for CharLiteral, string_view for StringLiteral) - same pattern
// as ceres::casm::TokenPayload in CeresASM. Token itself views into the SourceManager's buffer
// (libs/support), it never owns its lexeme.
//
// Implemented in Fase 1 of the phased plan (§13).

namespace ceresc::lexer
{
}
