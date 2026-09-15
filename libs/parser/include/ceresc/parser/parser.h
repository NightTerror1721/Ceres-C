#pragma once

#include <ceresc/lexer/lexer.h>
#include <ceresc/ast/ast_visitor.h>
#include <ceresc/support/arena.h>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

// Parser - Token[] -> AST (§7 of the architecture plan).
//
// Recursive descent for declarations and statements; precedence climbing for expressions (levels
// 2-11 of the precedence table share one parametrized function, only the assignment, cast, and
// prefix/postfix levels get dedicated ones). A syntax error does not abort the file: panic-mode
// recovery resynchronizes on the next `;`/`}` (inside a statement) or the next type keyword/`}`
// (at top level), so one pass reports every syntax error in a broken file, not just the first.
//
// This file covers only that first half. Fase 2 has no statements or declarations yet - no `;`/`}`
// exists to resynchronize on - so its error handling is necessarily narrower: a syntax error inside
// an expression reports a diagnostic and the failing parse* function returns nullptr, which
// propagates up through its caller instead of building on a missing piece. The one exception is a
// token that matches no production at all (parsePrimary()/parseTypeSpec()'s default case): that
// always consumes the bad token before returning nullptr, so at least one token of progress is
// guaranteed even without a real synchronization point. Real panic-mode recovery is Fase 3's job,
// once `;`/`}` exist to resynchronize on.
//
// Deliberately takes a Lexer&, not a pre-lexed Token[]: the grammar in §3 never needs more than one
// token of lookahead beyond the current one (used only to disambiguate `(type-name)` from a
// parenthesized expression, for cast-expr and sizeof - see parseCast()/parseSizeof()), so pulling
// tokens from the Lexer on demand, two at a time, is simpler than materializing the whole stream
// up front.
//
// Includes ast_visitor.h, not just expr.h: this file constructs concrete Expr subclasses directly
// (Arena::create<IntLiteralExpr>(...) and so on), and MSVC needs each class's accept() override to
// be a visible *definition* (not just expr.h's declaration) in this translation unit to emit its
// vtable correctly - GCC tolerates the declaration-only form here, MSVC does not. Verified by
// hitting the MSVC link error (unresolved accept()) with expr.h alone and fixing it this way.
//
// Also takes an Arena&, not a StringPool&: every AST node this file builds goes through
// Arena::create<T> (see libs/ast's own note on why every node type is trivially destructible), and
// nothing here needs to intern a string - NameExpr borrows the Token's own lexeme view, and
// StringLiteralExpr reuses the PooledString the Lexer already interned into the token's value.
//
// UnaryOp/BinaryOp/AssignOp translation from TokenKind happens here, not in libs/ast: ast does not
// depend on lexer (see the architecture plan's §1 dependency diagram), so this is the one place
// that is allowed to know both.
//
// Implemented in Fase 2 (expressions) and Fase 3 (declarations/statements) of the phased plan
// (§13).

namespace ceresc::parser
{
	using ast::Expr;
	using ast::Type;
	using ast::UnaryOp;
	using ast::BinaryOp;
	using ast::AssignOp;
	using lexer::Token;
	using lexer::TokenKind;

	class Parser
	{
	private:
		lexer::Lexer& _lexer;
		support::Arena& _arena;
		support::DiagnosticEngine& _diagnostics;
		Token _current;
		Token _next; // one token of lookahead beyond _current - see the header comment above

	public:
		Parser() = delete;
		Parser(const Parser&) = delete;
		Parser(Parser&&) = default;
		~Parser() = default;

		Parser& operator=(const Parser&) = delete;
		Parser& operator=(Parser&&) = delete;

	public:
		explicit Parser(lexer::Lexer& lexer, support::Arena& arena, support::DiagnosticEngine& diagnostics) noexcept;

	public:
		// Fase 2 entry points. parseTranslationUnit()/parseExternalDecl()/parseStatement() and the
		// rest of parseTypeSpec()'s grammar (struct/enum/typedef-name) are Fase 3.
		Expr* parseExpression();
		const Type* parseTypeName();

		bool isAtEnd() const noexcept { return _current.isEndOfFile(); }

	private:
		// Precedence table (§7), lowest to highest:
		//   1: parseAssignment (right-assoc)     2-11: parseBinary (left-assoc, one table)
		//  12: parseCast (right-assoc)            13: parseUnary (right-assoc, prefix + sizeof)
		//  14: parsePostfix (left-assoc, chained) -> parsePrimary
		Expr* parseAssignment();
		Expr* parseBinary(int minPrecedence);
		Expr* parseCast();
		Expr* parseUnary();
		Expr* parseSizeof(support::SourceLocation location);
		Expr* parsePostfix();
		Expr* parsePrimary();

		const Type* parseTypeSpec();

	private:
		Token advance() noexcept;
		bool check(TokenKind kind) const noexcept { return _current.is(kind); }
		bool match(TokenKind kind) noexcept;
		bool expect(TokenKind kind, std::string_view what) noexcept;

		Expr* wrapUnary(UnaryOp op, support::SourceLocation location, Expr* operand) noexcept;
		std::span<Expr* const> copyArgsToArena(const std::vector<Expr*>& args) noexcept;

	private:
		// True for the tokens that can start a type-name in Fase 2's subset of the grammar
		// (primitives + signed/unsigned/short/long combinations, no struct/enum/typedef-name yet -
		// see type.h's own note on why that's not a restriction, just work that hasn't had its
		// phase). Used to disambiguate `(type-name)` from a parenthesized expression with exactly
		// one token of lookahead past `(`, for both cast-expr and sizeof.
		static constexpr bool isTypeSpecStart(TokenKind kind) noexcept
		{
			switch (kind)
			{
				case TokenKind::KwVoid:
				case TokenKind::KwBool:
				case TokenKind::KwFloat:
				case TokenKind::KwChar:
				case TokenKind::KwShort:
				case TokenKind::KwInt:
				case TokenKind::KwLong:
				case TokenKind::KwSigned:
				case TokenKind::KwUnsigned:
					return true;
				default:
					return false;
			}
		}

		static const std::unordered_map<TokenKind, std::pair<int, BinaryOp>>& binaryOpTable();
		static const std::unordered_map<TokenKind, AssignOp>& assignOpTable();
	};
}
