#include <ceresc/lexer/lexer.h>

#include <charconv>
#include <string>

namespace ceresc::lexer
{
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
					_diagnostics.error(startLoc, "unterminated block comment");
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

	Token Lexer::makeIntToken(std::string_view lexeme, SourceLocation loc, int base, std::string_view digits)
	{
		TokenValue::IntegralValue value = 0;
		auto result = std::from_chars(digits.data(), digits.data() + digits.size(), value, base);
		if (result.ec == std::errc::result_out_of_range)
			_diagnostics.error(loc, "integer literal is too large to represent");

		return Token::makeLiteralInt(lexeme, value, loc);
	}

	Token Lexer::makeFloatToken(std::string_view lexeme, SourceLocation loc)
	{
		TokenValue::FloatingValue value = 0.0;
		auto result = std::from_chars(lexeme.data(), lexeme.data() + lexeme.size(), value);
		if (result.ec == std::errc::result_out_of_range)
			_diagnostics.error(loc, "floating-point literal is out of range");

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

		std::string_view lexeme = _cursor.buffer().substr(startPos, _cursor.position() - startPos);
		std::string_view digits = _cursor.buffer().substr(digitsStart, _cursor.position() - digitsStart);

		if (digits.empty())
		{
			if (base == 16)
				_diagnostics.error(startLoc, "hexadecimal literal has no digits");
			else
				_diagnostics.error(startLoc, "binary literal has no digits");
			return Token::makeLiteralInt(lexeme, 0, startLoc);
		}

		return makeIntToken(lexeme, startLoc, base, digits);
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

		std::string_view lexeme = _cursor.buffer().substr(startPos, _cursor.position() - startPos);

		if (isFloat)
			return makeFloatToken(lexeme, startLoc);
		return makeIntToken(lexeme, startLoc, 10, lexeme);
	}

	char Lexer::scanEscapeSequence()
	{
		// Called right after the backslash has already been consumed.
		SourceLocation loc = currentLocation();

		if (_cursor.isAtEnd() || _cursor.peek() == '\n')
		{
			_diagnostics.error(loc, "unterminated escape sequence");
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
					_diagnostics.error(loc, "\\x used with no following hex digits");
					return '\0';
				}
				return static_cast<char>(value);
			}
			default:
				_diagnostics.error(loc, "unknown escape sequence '\\{}'", c);
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
			_diagnostics.error(startLoc, "empty character literal");
			_cursor.advance();
		}
		else if (_cursor.isAtEnd() || _cursor.peek() == '\n')
		{
			_diagnostics.error(startLoc, "unterminated character literal");
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
				_diagnostics.error(startLoc, "unterminated character literal");
			}
			else
			{
				_diagnostics.error(startLoc, "character literal contains more than one character");
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
		_cursor.advance(); // opening "

		std::string decoded;
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

		if (!terminated)
			_diagnostics.error(startLoc, "unterminated string literal");

		std::string_view lexeme = _cursor.buffer().substr(startPos, _cursor.position() - startPos);
		support::PooledString interned = _stringPool.intern(decoded);
		if (!interned)
			_diagnostics.error(startLoc, "out of memory interning string literal");

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
		_diagnostics.error(loc, "unexpected character '{}'", c);
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
