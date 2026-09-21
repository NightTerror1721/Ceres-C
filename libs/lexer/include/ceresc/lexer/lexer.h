#pragma once

#include "token.h"
#include <ceresc/support/diagnostics.h>
#include <ceresc/support/string_pool.h>
#include <functional>
#include <unordered_map>

// Lexer - a cursor over std::string_view that produces one Token per call to next(), with a
// single character of pushback via peek() (the grammar in §3 never needs more).
//
// Does not know the grammar: it knows that `if` is the keyword If, not that `if` needs
// parentheses. An unrecognized character does not stop the lexer - it reports a diagnostic,
// yields Token::Invalid and advances one character, so the parser can keep going. See the
// architecture plan, §5.
//
// This extends to every keyword regardless of where it stands in token.h's V1/reserved split
// (switch/enum/typedef/sizeof, or double/union/volatile/... - see token.h): the lexer emits the
// real TokenKind exactly like any other keyword, no differently. It has no notion of "not built
// yet" or "not implemented in this version" - both of those checks belong to the parser, the first
// stage that actually knows which forms it can build an AST for.
//
// Implemented in Fase 1 of the phased plan (§13).

namespace ceresc::lexer
{
	using support::Offset;
	using support::Result;
	using support::Diagnostic;

	// LexerCursor - a position over std::string_view that also tracks line/column, kept in sync
	// through every method that moves it (advance/back and the match()/skipUntil() family alike -
	// there is no back door that can desync them from position()). Forward moves update line/column
	// incrementally, one character at a time, since that is the only correct way to notice a '\n'
	// in the range being crossed. Backward moves (back()/back(uoffset)) can't do the equivalent
	// incrementally - undoing past a '\n' needs the length of the *previous* line, which isn't
	// known without looking - so they recompute line/column from scratch by rescanning the buffer
	// up to the new position; back() itself still takes the O(1) path when the character it is
	// undoing isn't a newline, which is the common case. This asymmetry is fine in practice: the
	// grammar in §3 never needs more than a character of pushback, so back() is rarely called at
	// all, let alone across a line boundary.
	class LexerCursor
	{
	private:
		std::string_view _buffer;
		uoffset _position = 0;
		support::LineNumber _line = 1;
		support::ColumnNumber _column = 1;

	public:
		constexpr LexerCursor() = default;
		constexpr LexerCursor(const LexerCursor&) = default;
		constexpr LexerCursor(LexerCursor&&) = default;
		constexpr ~LexerCursor() = default;

		constexpr LexerCursor& operator=(const LexerCursor&) = default;
		constexpr LexerCursor& operator=(LexerCursor&&) = default;

	public:
		constexpr explicit LexerCursor(std::string_view buffer) noexcept : _buffer(buffer) {}

		constexpr std::string_view buffer() const noexcept { return _buffer; }
		constexpr uoffset position() const noexcept { return _position; }
		constexpr support::LineNumber line() const noexcept { return _line; }
		constexpr support::ColumnNumber column() const noexcept { return _column; }

		constexpr bool isAtEnd() const noexcept { return _position >= _buffer.size(); }

		constexpr char peek() const noexcept
		{
			if (isAtEnd())
				return '\0';
			return _buffer[_position];
		}

		constexpr char peek(ioffset offset) const noexcept
		{
			ioffset newPosition = static_cast<ioffset>(_position) + offset;
			if (newPosition < 0 || static_cast<uoffset>(newPosition) >= _buffer.size())
				return '\0';
			return _buffer[static_cast<uoffset>(newPosition)];
		}

		constexpr char advance() noexcept
		{
			return advanceOne();
		}

		constexpr char advance(uoffset offset) noexcept
		{
			if (isAtEnd())
				return '\0';

			char currentChar = _buffer[_position];
			uoffset target = (_position + offset < _buffer.size()) ? _position + offset : _buffer.size();
			while (_position < target)
				advanceOne();
			return currentChar;
		}

		constexpr char back() noexcept
		{
			if (_position == 0)
				return '\0';

			char c = _buffer[--_position];
			if (c == '\n')
				recomputeLineColumn();
			else
				_column--;
			return c;
		}

		constexpr char back(uoffset offset) noexcept
		{
			if (_position == 0)
				return '\0';

			_position = (_position >= offset) ? _position - offset : 0;
			recomputeLineColumn();
			return _buffer[_position];
		}

		constexpr bool check(char expected) const noexcept
		{
			return !isAtEnd() && _buffer[_position] == expected;
		}
		constexpr bool check(std::string_view expected) const noexcept
		{
			if (_position + expected.size() > _buffer.size())
				return false;

			return _buffer.substr(_position, expected.size()) == expected;
		}
		constexpr bool checkAny(std::string_view expectedChars) const noexcept
		{
			if (isAtEnd())
				return false;

			char currentChar = _buffer[_position];
			return expectedChars.find(currentChar) != std::string_view::npos;
		}
		bool check(std::move_only_function<bool(char)> func) const noexcept
		{
			if (isAtEnd())
				return false;
			return func(_buffer[_position]);
		}

		constexpr bool match(char expected) noexcept
		{
			if (!check(expected))
				return false;

			advanceOne();
			return true;
		}
		constexpr bool match(std::string_view expected) noexcept
		{
			if (!check(expected))
				return false;

			for (usize i = 0; i < expected.size(); ++i)
				advanceOne();
			return true;
		}
		constexpr bool matchAny(std::string_view expectedChars) noexcept
		{
			if (!checkAny(expectedChars))
				return false;

			advanceOne();
			return true;
		}
		bool match(std::move_only_function<bool(char)> func) noexcept
		{
			if (!check(std::move(func)))
				return false;

			advanceOne();
			return true;
		}

		constexpr usize skipUntil(char delimiter) noexcept
		{
			usize start = _position;
			while (!isAtEnd() && _buffer[_position] != delimiter)
				advanceOne();
			return _position - start;
		}

		constexpr usize skipUntil(std::string_view delimiter) noexcept
		{
			usize start = _position;
			while (!isAtEnd() && !check(delimiter))
				advanceOne();
			return _position - start;
		}
		constexpr usize skipUntilAny(std::string_view delimiters) noexcept
		{
			usize start = _position;
			while (!isAtEnd() && !checkAny(delimiters))
				advanceOne();
			return _position - start;
		}
		usize skipUntil(std::move_only_function<bool(char)> func) noexcept
		{
			usize start = _position;
			while (!isAtEnd() && !func(_buffer[_position]))
				advanceOne();
			return _position - start;
		}

		std::string_view consumeUntil(char delimiter) noexcept
		{
			usize start = _position;
			skipUntil(delimiter);
			return _buffer.substr(start, _position - start);
		}
		std::string_view consumeUntil(std::string_view delimiter) noexcept
		{
			usize start = _position;
			skipUntil(delimiter);
			return _buffer.substr(start, _position - start);
		}
		std::string_view consumeUntilAny(std::string_view delimiters) noexcept
		{
			usize start = _position;
			skipUntilAny(delimiters);
			return _buffer.substr(start, _position - start);
		}
		std::string_view consumeUntil(std::move_only_function<bool(char)> func) noexcept
		{
			usize start = _position;
			skipUntil(std::move(func));
			return _buffer.substr(start, _position - start);
		}

		constexpr std::string_view substr(ioffset start, usize length) const noexcept
		{
			if (start < 0 || static_cast<uoffset>(start) >= _buffer.size())
				return std::string_view{};
			uoffset adjustedStart = static_cast<uoffset>(start);
			uoffset adjustedLength = (adjustedStart + length > _buffer.size()) ? _buffer.size() - adjustedStart : length;
			return _buffer.substr(adjustedStart, adjustedLength);
		}

		constexpr std::string_view substr(ioffset start) const noexcept
		{
			if (start < 0 || static_cast<uoffset>(start) >= _buffer.size())
				return std::string_view{};
			uoffset adjustedStart = static_cast<uoffset>(start);
			return _buffer.substr(adjustedStart);
		}

		constexpr std::string_view substr() const noexcept
		{
			return _buffer.substr(_position);
		}

	public:
		constexpr explicit operator bool() const noexcept { return !isAtEnd(); }
		constexpr bool operator!() const noexcept { return isAtEnd(); }

		constexpr char operator*() const noexcept { return peek(); }
		constexpr char operator[](ioffset offset) const noexcept { return peek(offset); }

		constexpr LexerCursor& operator++() noexcept
		{
			advance();
			return *this;
		}

		constexpr LexerCursor& operator--() noexcept
		{
			back();
			return *this;
		}

		constexpr LexerCursor& operator+=(ioffset offset) noexcept
		{
			if (offset > 0)
				advance(static_cast<uoffset>(offset));
			else if (offset < 0)
				back(static_cast<uoffset>(-offset));
			return *this;
		}

		constexpr LexerCursor& operator-=(ioffset offset) noexcept
		{
			if (offset > 0)
				back(static_cast<uoffset>(offset));
			else if (offset < 0)
				advance(static_cast<uoffset>(-offset));
			return *this;
		}

	private:
		// The only place _position moves forward one character at a time - every forward-moving
		// method above (advance(uoffset), match(), skipUntil()...) routes through this instead of
		// jumping _position directly, so line/column bookkeeping can't be forgotten in one of them.
		constexpr char advanceOne() noexcept
		{
			if (isAtEnd())
				return '\0';

			char c = _buffer[_position++];
			if (c == '\n')
			{
				_line++;
				_column = 1;
			}
			else
			{
				_column++;
			}
			return c;
		}

		// line/column are a pure function of (buffer, position) - line is 1 + the number of '\n'
		// before position, column is the distance back to the previous '\n' (or to the start of the
		// buffer if there isn't one). Used to recompute both from scratch after a backward move that
		// crossed a '\n', where there is no cheaper way to know the previous line's length.
		constexpr void recomputeLineColumn() noexcept
		{
			support::LineNumber line = 1;
			uoffset lastNewlinePos = 0;
			bool sawNewline = false;

			for (uoffset i = 0; i < _position; ++i)
			{
				if (_buffer[i] == '\n')
				{
					line++;
					lastNewlinePos = i;
					sawNewline = true;
				}
			}

			_line = line;
			_column = sawNewline ? static_cast<support::ColumnNumber>(_position - lastNewlinePos) : static_cast<support::ColumnNumber>(_position + 1);
		}
	};

	// Lexer - turns a single source buffer into Token[] (via repeated next()).
	//
	// Deliberately takes a plain string_view + SourceId, not the whole SourceManager: a Lexer only
	// ever scans one file, and coupling it to the file-management concern would be a dependency it
	// doesn't need (same reasoning as ceres::casm::Lexer(source, stringPool) in CeresASM).
	class Lexer
	{
	private:
		using KeywordFactory = Token (*)(SourceLocation) noexcept;

	private:
		LexerCursor _cursor;
		support::SourceId _sourceId;
		support::DiagnosticEngine& _diagnostics;
		support::StringPool& _stringPool;

	public:
		Lexer() = delete;
		Lexer(const Lexer&) = delete;
		Lexer(Lexer&&) = default;
		~Lexer() = default;

		Lexer& operator=(const Lexer&) = delete;
		Lexer& operator=(Lexer&&) = delete;

	public:
		// The pool the string literals are interned in, for a consumer that makes a string of its own (the parser's
		// `__func__`) and needs it to live as long as the ones the source wrote.
		support::StringPool& stringPool() noexcept { return _stringPool; }

		explicit Lexer(std::string_view source, support::SourceId sourceId, support::DiagnosticEngine& diagnostics, support::StringPool& stringPool) noexcept :
			_cursor(source),
			_sourceId(sourceId),
			_diagnostics(diagnostics),
			_stringPool(stringPool)
		{}

	public:
		// Scans and returns the next token, advancing past it. Once the buffer is exhausted, every
		// further call keeps returning EndOfFile without advancing any further.
		Token next();

		constexpr bool isAtEnd() const noexcept { return _cursor.isAtEnd(); }

	private:
		SourceLocation currentLocation() const noexcept;

		void skipTrivia();

		Token scanIdentifierOrKeyword();
		Token scanNumber();
		Token scanRadixInteger(SourceLocation startLoc, uoffset startPos, int base);
		Token scanCharLiteral();
		// One token for a whole run of adjacent string literals - C's translation phase 6, which
		// happens before anything parses. `"a" "b"` and a literal split over three lines are each
		// one Token::LiteralString whose value is the pieces joined.
		Token scanStringLiteral();
		Token scanOperatorOrPunctuation();

		char scanEscapeSequence();

		Token makeIntToken(std::string_view lexeme, SourceLocation loc, int base, std::string_view digits, bool isUnsigned);
		Token makeFloatToken(std::string_view lexeme, std::string_view digits, SourceLocation loc);

	private:
		static constexpr bool isIdentifierStart(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
		static constexpr bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }
		static constexpr bool isIdentifierContinue(char c) noexcept { return isIdentifierStart(c) || isDigit(c); }
		static constexpr bool isHexDigit(char c) noexcept { return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
		static constexpr int hexDigitValue(char c) noexcept
		{
			if (c >= '0' && c <= '9') return c - '0';
			if (c >= 'a' && c <= 'f') return c - 'a' + 10;
			if (c >= 'A' && c <= 'F') return c - 'A' + 10;
			return 0;
		}

		static const std::unordered_map<std::string_view, KeywordFactory>& keywordTable();
	};
}
