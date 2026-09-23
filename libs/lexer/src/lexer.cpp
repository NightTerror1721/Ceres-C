#include <ceresc/lexer/lexer.h>

#include <charconv>
#include <limits>
#include <string>

namespace ceresc::lexer
{
	// Shorthand for the ids these messages are classified by - every error() and warning()
	// call below names one. See support/diagnostic_id.h.
	using DiagId = support::DiagnosticId;

	SourceLocation Lexer::currentLocation() const noexcept
	{
		return SourceLocation(_sourceId, _cursor.line(), _cursor.column(), static_cast<Offset>(_cursor.position()));
	}

	void Lexer::skipTrivia()
	{
		for (;;)
		{
			if (_cursor.matchAny(" \t\r\n"))
				continue;

			if (_cursor.check("//"))
			{
				_cursor.advance(2);
				_cursor.skipUntil('\n');
				continue;
			}

			if (_cursor.check("/*"))
			{
				SourceLocation startLoc = currentLocation();
				_cursor.advance(2);
				_cursor.skipUntil("*/");
				if (!_cursor.match("*/"))
					_diagnostics.error(DiagId::UnterminatedBlockComment, startLoc, "unterminated block comment");
				continue;
			}

			break;
		}
	}

	Token Lexer::next()
	{
		skipTrivia();

		SourceLocation loc = currentLocation();

		if (_cursor.isAtEnd())
			return Token::makeEndOfFile(loc);

		char c = _cursor.peek();

		if (isIdentifierStart(c))
			return scanIdentifierOrKeyword();

		if (isDigit(c) || (c == '.' && isDigit(_cursor.peek(1))))
			return scanNumber();

		if (c == '"')
			return scanStringLiteral();

		if (c == '\'')
			return scanCharLiteral();

		return scanOperatorOrPunctuation();
	}

	Token Lexer::scanIdentifierOrKeyword()
	{
		SourceLocation startLoc = currentLocation();
		uoffset startPos = _cursor.position();

		while (isIdentifierContinue(_cursor.peek()))
			_cursor.advance();

		std::string_view lexeme = _cursor.buffer().substr(startPos, _cursor.position() - startPos);

		if (lexeme == "true")
			return Token::makeLiteralBool(lexeme, true, startLoc);
		if (lexeme == "false")
			return Token::makeLiteralBool(lexeme, false, startLoc);

		if (auto it = keywordTable().find(lexeme); it != keywordTable().end())
			return it->second(startLoc);

		return Token::makeIdentifier(lexeme, startLoc);
	}

	Token Lexer::makeIntToken(std::string_view lexeme, SourceLocation loc, int base, std::string_view digits, bool isUnsigned, bool isLongLong)
	{
		TokenValue::IntegralValue value = 0;
		auto result = std::from_chars(digits.data(), digits.data() + digits.size(), value, base);
		if (result.ec == std::errc::result_out_of_range)
		{
			// The literal does not fit a 64-bit value at all. Report it once and keep the digits that
			// did fit (from_chars leaves `value` untouched on overflow, so this is 0) rather than
			// silently wrapping to a different number.
			_diagnostics.error(DiagId::IntegerLiteralTooLarge, loc, "integer literal is too large to represent");
		}
		else if (base == 10 && isLongLong && !isUnsigned && value > static_cast<u64>((std::numeric_limits<i64>::max)()))
		{
			// In range for u64 but out of range for a signed `long long`. C gives an out-of-range
			// DECIMAL literal no type; for a hex/binary constant the suffix list includes
			// `unsigned long long`, so `0xFFFFFFFFFFFFFFFFLL` is a legal unsigned value and is not
			// reported. A warning and a 64-bit reinterpretation is the useful answer for decimal.
			_diagnostics.warning(DiagId::IntegerLiteralOutOfRange, loc,
				"integer literal {} is too large for a signed 'long long'; it is treated as its 64-bit bit pattern",
				digits);
		}

		return Token::makeLiteralInt(lexeme, value, loc, isUnsigned, isLongLong);
	}

	void Lexer::scanIntegerSuffix(bool& isUnsigned, bool& isLongLong) noexcept
	{
		// C's integer-suffix: at most one `u`/`U` and at most one `ll`/`LL`, in either order. Two
		// passes is enough for every legal combination (`u`, `ll`, `ull`, `llu`); anything left after
		// them (a third suffix letter, a lone `l`, `lL` mixed with digits...) is an identifier of its
		// own and the parser rejects it exactly as before. The two `l`s must match case, as C's
		// grammar requires (`ll` or `LL`, never `lL`/`Ll`).
		for (int pass = 0; pass < 2; ++pass)
		{
			if (!isUnsigned && (_cursor.peek() == 'u' || _cursor.peek() == 'U'))
			{
				isUnsigned = true;
				_cursor.advance();
				continue;
			}
			if (!isLongLong && ((_cursor.peek() == 'l' && _cursor.peek(1) == 'l') ||
				(_cursor.peek() == 'L' && _cursor.peek(1) == 'L')))
			{
				isLongLong = true;
				_cursor.advance();
				_cursor.advance();
				continue;
			}
			break;
		}
	}

	Token Lexer::makeFloatToken(std::string_view lexeme, std::string_view digits, SourceLocation loc)
	{
		TokenValue::FloatingValue value = 0.0;
		auto result = std::from_chars(digits.data(), digits.data() + digits.size(), value);
		if (result.ec == std::errc::result_out_of_range)
			_diagnostics.error(DiagId::FloatLiteralOutOfRange, loc, "floating-point literal is out of range");

		return Token::makeLiteralFloat(lexeme, value, loc);
	}

	Token Lexer::scanRadixInteger(SourceLocation startLoc, uoffset startPos, int base)
	{
		_cursor.advance(2); // '0' then 'x'/'X' or 'b'/'B'

		uoffset digitsStart = _cursor.position();
		if (base == 16)
		{
			while (isHexDigit(_cursor.peek()))
				_cursor.advance();
		}
		else
		{
			while (_cursor.peek() == '0' || _cursor.peek() == '1')
				_cursor.advance();
		}

		uoffset digitsEnd = _cursor.position();
		// A `u`/`U` suffix makes the literal unsigned and `ll`/`LL` makes it 64-bit. `f`/`F` is a hex
		// DIGIT here, never a float suffix - `0xFFf` is one number - so only the integer suffixes
		// apply to a radix literal.
		bool isUnsigned = false;
		bool isLongLong = false;
		scanIntegerSuffix(isUnsigned, isLongLong);

		std::string_view lexeme = _cursor.buffer().substr(startPos, _cursor.position() - startPos);
		std::string_view digits = _cursor.buffer().substr(digitsStart, digitsEnd - digitsStart);

		if (digits.empty())
		{
			if (base == 16)
				_diagnostics.error(DiagId::HexLiteralHasNoDigits, startLoc, "hexadecimal literal has no digits");
			else
				_diagnostics.error(DiagId::BinaryLiteralHasNoDigits, startLoc, "binary literal has no digits");
			return Token::makeLiteralInt(lexeme, 0, startLoc);
		}

		return makeIntToken(lexeme, startLoc, base, digits, isUnsigned, isLongLong);
	}

	Token Lexer::scanNumber()
	{
		SourceLocation startLoc = currentLocation();
		uoffset startPos = _cursor.position();

		if (_cursor.peek() == '0' && (_cursor.peek(1) == 'x' || _cursor.peek(1) == 'X'))
			return scanRadixInteger(startLoc, startPos, 16);
		if (_cursor.peek() == '0' && (_cursor.peek(1) == 'b' || _cursor.peek(1) == 'B'))
			return scanRadixInteger(startLoc, startPos, 2);

		while (isDigit(_cursor.peek()))
			_cursor.advance();

		bool isFloat = false;

		// Once a digit run has started (or we got here on a leading '.', already confirmed by next()
		// to be followed by a digit), a '.' is unconditionally part of the number - maximal munch,
		// same as any real C lexer: `1.field` lexes as float `1.` then identifier `field`, it is not
		// the lexer's job to notice that is nonsensical.
		if (_cursor.peek() == '.')
		{
			isFloat = true;
			_cursor.advance();
			while (isDigit(_cursor.peek()))
				_cursor.advance();
		}

		if ((_cursor.peek() == 'e' || _cursor.peek() == 'E') &&
			(isDigit(_cursor.peek(1)) || ((_cursor.peek(1) == '+' || _cursor.peek(1) == '-') && isDigit(_cursor.peek(2)))))
		{
			isFloat = true;
			_cursor.advance();
			if (_cursor.peek() == '+' || _cursor.peek() == '-')
				_cursor.advance();
			while (isDigit(_cursor.peek()))
				_cursor.advance();
		}

		// A trailing suffix. `u`/`U` marks an integer literal unsigned and `ll`/`LL` makes it 64-bit;
		// `f`/`F` marks a float, and is also what turns a digit run with no `.`/exponent into one
		// (`1f`). The integer suffixes combine in either order (`1ull`, `1llu`), while `f` stands
		// alone. Anything else is an identifier of its own, exactly as before.
		uoffset digitsEnd = _cursor.position();
		bool isUnsigned = false;
		bool isLongLong = false;
		if (isFloat)
		{
			if (_cursor.peek() == 'f' || _cursor.peek() == 'F')
				_cursor.advance();
		}
		else if (_cursor.peek() == 'f' || _cursor.peek() == 'F')
		{
			isFloat = true;
			_cursor.advance();
		}
		else
		{
			scanIntegerSuffix(isUnsigned, isLongLong);
		}

		std::string_view lexeme = _cursor.buffer().substr(startPos, _cursor.position() - startPos);
		std::string_view digits = _cursor.buffer().substr(startPos, digitsEnd - startPos);

		if (isFloat)
			return makeFloatToken(lexeme, digits, startLoc);
		return makeIntToken(lexeme, startLoc, 10, digits, isUnsigned, isLongLong);
	}

	char Lexer::scanEscapeSequence()
	{
		// Called right after the backslash has already been consumed.
		SourceLocation loc = currentLocation();

		if (_cursor.isAtEnd() || _cursor.peek() == '\n')
		{
			_diagnostics.error(DiagId::UnterminatedEscapeSequence, loc, "unterminated escape sequence");
			return '\0';
		}

		char c = _cursor.advance();
		switch (c)
		{
			case 'n': return '\n';
			case 't': return '\t';
			case 'r': return '\r';
			case '0': return '\0';
			case '\\': return '\\';
			case '\'': return '\'';
			case '"': return '"';
			case 'a': return '\a';
			case 'b': return '\b';
			case 'f': return '\f';
			case 'v': return '\v';
			case 'x':
			{
				int value = 0;
				int digitCount = 0;
				while (digitCount < 2 && isHexDigit(_cursor.peek()))
				{
					value = value * 16 + hexDigitValue(_cursor.advance());
					digitCount++;
				}

				if (digitCount == 0)
				{
					_diagnostics.error(DiagId::HexEscapeHasNoDigits, loc, "\\x used with no following hex digits");
					return '\0';
				}
				return static_cast<char>(value);
			}
			default:
				_diagnostics.error(DiagId::UnknownEscapeSequence, loc, "unknown escape sequence '\\{}'", c);
				return c;
		}
	}

	Token Lexer::scanCharLiteral()
	{
		SourceLocation startLoc = currentLocation();
		uoffset startPos = _cursor.position();
		_cursor.advance(); // opening '

		char value = '\0';

		if (_cursor.peek() == '\'')
		{
			_diagnostics.error(DiagId::EmptyCharacterLiteral, startLoc, "empty character literal");
			_cursor.advance();
		}
		else if (_cursor.isAtEnd() || _cursor.peek() == '\n')
		{
			_diagnostics.error(DiagId::UnterminatedCharacterLiteral, startLoc, "unterminated character literal");
		}
		else
		{
			if (_cursor.peek() == '\\')
			{
				_cursor.advance();
				value = scanEscapeSequence();
			}
			else
			{
				value = _cursor.advance();
			}

			if (_cursor.peek() == '\'')
			{
				_cursor.advance(); // closing '
			}
			else if (_cursor.isAtEnd() || _cursor.peek() == '\n')
			{
				_diagnostics.error(DiagId::UnterminatedCharacterLiteral, startLoc, "unterminated character literal");
			}
			else
			{
				_diagnostics.error(DiagId::MultiCharacterLiteral, startLoc, "character literal contains more than one character");
				_cursor.skipUntilAny("'\n");
				_cursor.match('\'');
			}
		}

		std::string_view lexeme = _cursor.buffer().substr(startPos, _cursor.position() - startPos);
		return Token::makeLiteralChar(lexeme, value, startLoc);
	}

	Token Lexer::scanStringLiteral()
	{
		SourceLocation startLoc = currentLocation();
		uoffset startPos = _cursor.position();

		std::string decoded;
		uoffset endPos = startPos;

		// C joins adjacent string literals into one, before the grammar ever sees them - which is
		// what makes `"a" "b"` a single 3-byte object rather than a syntax error, and what lets a
		// long string be written over several lines. One token comes out of the whole run, so the
		// parser, sizeof and the .rodata entry all see exactly what the program meant to write.
		for (;;)
		{
			SourceLocation pieceLoc = currentLocation();
			_cursor.advance(); // opening "
			bool terminated = false;

			while (!_cursor.isAtEnd() && _cursor.peek() != '\n')
			{
				char c = _cursor.peek();
				if (c == '"')
				{
					_cursor.advance();
					terminated = true;
					break;
				}

				if (c == '\\')
				{
					_cursor.advance();
					decoded.push_back(scanEscapeSequence());
				}
				else
				{
					decoded.push_back(_cursor.advance());
				}
			}

			// Before the trivia below, so the token's lexeme is the literals and whatever sits
			// between them, and not the blank line that happens to follow the last one.
			endPos = _cursor.position();

			if (!terminated)
			{
				_diagnostics.error(DiagId::UnterminatedStringLiteral, pieceLoc, "unterminated string literal");
				break;
			}

			// Whitespace, newlines and comments may all sit between two pieces. Skipping them is
			// not undone when what follows turns out not to be another literal: next() would have
			// skipped exactly the same run on its next call, and putting the cursor back would mean
			// skipTrivia() reporting an unterminated block comment here and then again there.
			skipTrivia();
			if (_cursor.isAtEnd() || _cursor.peek() != '"')
				break;
		}

		std::string_view lexeme = _cursor.buffer().substr(startPos, endPos - startPos);
		support::PooledString interned = _stringPool.intern(decoded);
		if (!interned)
			_diagnostics.error(DiagId::StringPoolExhausted, startLoc, "out of memory interning string literal");

		return Token::makeLiteralString(lexeme, interned, startLoc);
	}

	Token Lexer::scanOperatorOrPunctuation()
	{
		SourceLocation loc = currentLocation();

		switch (_cursor.advance())
		{
			case '(': return Token::makeLParen(loc);
			case ')': return Token::makeRParen(loc);
			case '{': return Token::makeLBrace(loc);
			case '}': return Token::makeRBrace(loc);
			case '[': return Token::makeLBracket(loc);
			case ']': return Token::makeRBracket(loc);
			case ',': return Token::makeComma(loc);
			case ';': return Token::makeSemicolon(loc);
			case ':': return Token::makeColon(loc);
			case '?': return Token::makeQuestion(loc);
			case '~': return Token::makeTilde(loc);
			// Maximal munch again, but for a three-character token: `...` is one token, and `.` is
			// only Dot when it is NOT the start of one. `..` is not a token in C at all, so a lone
			// pair falls through to Dot and lets the parser complain about the second one.
			case '.':
				if (_cursor.match("..")) return Token::makeEllipsis(loc);
				return Token::makeDot(loc);

			// Maximal munch: test the suffix after consuming the shared prefix.
			case '-':
				if (_cursor.match('>')) return Token::makeArrow(loc);
				if (_cursor.match('-')) return Token::makeMinusMinus(loc);
				if (_cursor.match('=')) return Token::makeMinusEqual(loc);
				return Token::makeMinus(loc);

			case '+':
				if (_cursor.match('+')) return Token::makePlusPlus(loc);
				if (_cursor.match('=')) return Token::makePlusEqual(loc);
				return Token::makePlus(loc);

			case '*':
				if (_cursor.match('=')) return Token::makeStarEqual(loc);
				return Token::makeStar(loc);

			case '/':
				if (_cursor.match('=')) return Token::makeSlashEqual(loc);
				return Token::makeSlash(loc);

			case '%':
				if (_cursor.match('=')) return Token::makePercentEqual(loc);
				return Token::makePercent(loc);

			case '&':
				if (_cursor.match('&')) return Token::makeAmpersandAmpersand(loc);
				if (_cursor.match('=')) return Token::makeAmpersandEqual(loc);
				return Token::makeAmpersand(loc);

			case '|':
				if (_cursor.match('|')) return Token::makePipePipe(loc);
				if (_cursor.match('=')) return Token::makePipeEqual(loc);
				return Token::makePipe(loc);

			case '^':
				if (_cursor.match('=')) return Token::makeCaretEqual(loc);
				return Token::makeCaret(loc);

			case '!':
				if (_cursor.match('=')) return Token::makeBangEqual(loc);
				return Token::makeBang(loc);

			case '=':
				if (_cursor.match('=')) return Token::makeEqualEqual(loc);
				return Token::makeEqual(loc);

			case '<':
				if (_cursor.match('<'))
					return _cursor.match('=') ? Token::makeLessLessEqual(loc) : Token::makeLessLess(loc);
				if (_cursor.match('=')) return Token::makeLessEqual(loc);
				return Token::makeLess(loc);

			case '>':
				if (_cursor.match('>'))
					return _cursor.match('=') ? Token::makeGreaterGreaterEqual(loc) : Token::makeGreaterGreater(loc);
				if (_cursor.match('=')) return Token::makeGreaterEqual(loc);
				return Token::makeGreater(loc);

			default:
				break;
		}

		std::string_view lexeme = _cursor.buffer().substr(_cursor.position() - 1, 1);
		char c = lexeme.front();
		_diagnostics.error(DiagId::UnexpectedCharacter, loc, "unexpected character '{}'", c);
		return Token::makeInvalid(lexeme, loc);
	}

	const std::unordered_map<std::string_view, Lexer::KeywordFactory>& Lexer::keywordTable()
	{
		static const std::unordered_map<std::string_view, KeywordFactory> table = {
			// V1
			{"void", &Token::makeKwVoid},
			{"char", &Token::makeKwChar},
			{"short", &Token::makeKwShort},
			{"int", &Token::makeKwInt},
			{"long", &Token::makeKwLong},
			{"float", &Token::makeKwFloat},
			{"bool", &Token::makeKwBool},
			{"signed", &Token::makeKwSigned},
			{"unsigned", &Token::makeKwUnsigned},
			{"const", &Token::makeKwConst},
			{"struct", &Token::makeKwStruct},
			{"enum", &Token::makeKwEnum},
			{"typedef", &Token::makeKwTypedef},
			{"if", &Token::makeKwIf},
			{"else", &Token::makeKwElse},
			{"while", &Token::makeKwWhile},
			{"for", &Token::makeKwFor},
			{"do", &Token::makeKwDo},
			{"switch", &Token::makeKwSwitch},
			{"case", &Token::makeKwCase},
			{"default", &Token::makeKwDefault},
			{"goto", &Token::makeKwGoto},
			{"return", &Token::makeKwReturn},
			{"break", &Token::makeKwBreak},
			{"continue", &Token::makeKwContinue},
			{"inline", &Token::makeKwInline},
			{"static", &Token::makeKwStatic},
			{"extern", &Token::makeKwExtern},
			{"auto", &Token::makeKwAuto},
			{"sizeof", &Token::makeKwSizeof},
			// Reserved for a version after v1 (see token.h)
			{"double", &Token::makeKwDouble},
			{"union", &Token::makeKwUnion},
			{"volatile", &Token::makeKwVolatile},
			{"restrict", &Token::makeKwRestrict},
			{"register", &Token::makeKwRegister},
			{"alignof", &Token::makeKwAlignof},
			{"__interrupt", &Token::makeKwInterrupt},
			{"__interrupt_vector", &Token::makeKwInterruptVector},
		};
		return table;
	}
}
