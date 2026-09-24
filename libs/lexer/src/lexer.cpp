#include <ceresc/lexer/lexer.h>

#include <charconv>
#include <limits>
#include <string>
#include <vector>

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

		// L'x', u"x", U"x", u8"x": a prefixed literal, read before the identifier the prefix would
		// otherwise start.
		support::LiteralEncoding encoding = support::LiteralEncoding::Plain;
		if (u32 prefixLength = literalPrefixLength(0, encoding); prefixLength != 0)
		{
			return _cursor.peek(static_cast<ioffset>(prefixLength)) == '"' ? scanStringLiteral(encoding, prefixLength)
				: scanCharLiteral(encoding, prefixLength);
		}

		if (isIdentifierStart(c))
			return scanIdentifierOrKeyword();

		if (isDigit(c) || (c == '.' && isDigit(_cursor.peek(1))))
			return scanNumber();

		if (c == '"')
			return scanStringLiteral(support::LiteralEncoding::Plain, 0);

		if (c == '\'')
			return scanCharLiteral(support::LiteralEncoding::Plain, 0);

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

	Token Lexer::makeIntToken(std::string_view lexeme, SourceLocation loc, int base, std::string_view digits, bool isUnsigned, bool isLongLong, bool isLong)
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
		else if (base == 10 && !isUnsigned && value > static_cast<u64>((std::numeric_limits<i64>::max)()))
		{
			// In range for u64 but out of range for a signed `long long`, whatever the suffix (none,
			// `l` or `ll`). C gives an out-of-range DECIMAL literal no type; for a hex, binary or
			// octal constant the list includes `unsigned long long`, so `0xFFFFFFFFFFFFFFFF` is a
			// legal unsigned value and is not reported. A warning and a 64-bit reinterpretation is
			// the useful answer for decimal.
			_diagnostics.warning(DiagId::IntegerLiteralOutOfRange, loc,
				"integer literal {} is too large for a signed 'long long'; it is treated as its 64-bit bit pattern",
				digits);
		}

		return Token::makeLiteralInt(lexeme, value, loc, isUnsigned, isLongLong, isLong, base == 10);
	}

	void Lexer::scanIntegerSuffix(bool& isUnsigned, bool& isLongLong, bool& isLong) noexcept
	{
		// C's integer-suffix: at most one `u`/`U` and at most one of `l`/`L` and `ll`/`LL`, in
		// either order. Two passes is enough for every legal combination (`u`, `l`, `ll`, `ul`,
		// `lu`, `ull`, `llu`); anything left after them (a third suffix letter, `lL` mixed...) is an
		// identifier of its own and the parser rejects it. The two `l`s of `ll` must match case, as
		// C's grammar requires (`ll` or `LL`, never `lL`/`Ll`): `1lL` is `1l` followed by `L`.
		for (int pass = 0; pass < 2; ++pass)
		{
			const char c = _cursor.peek();
			if (!isUnsigned && (c == 'u' || c == 'U'))
			{
				isUnsigned = true;
				_cursor.advance();
				continue;
			}
			if (!isLongLong && !isLong && (c == 'l' || c == 'L'))
			{
				_cursor.advance();
				if (_cursor.peek() == c)
				{
					isLongLong = true;
					_cursor.advance();
				}
				else
				{
					isLong = true;
				}
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

	Token Lexer::makeHexFloatToken(std::string_view lexeme, std::string_view digits, SourceLocation loc)
	{
		TokenValue::FloatingValue value = 0.0;
		auto result = std::from_chars(digits.data(), digits.data() + digits.size(), value, std::chars_format::hex);
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

		// A hexadecimal FLOAT: hex digits, an optional '.' and more of them, then a binary exponent
		// `p`/`P` - which C requires, since without it `0x1.8` could not be told from a member access
		// on a number. `0x1.8p3` is 1.5 * 2^3 = 12. The suffix is `f`/`F` or `l`/`L` (long double,
		// which is float here), and an `f` right after the digits is a digit, not a suffix, until
		// the exponent has been read.
		if (base == 16 && (_cursor.peek() == '.' || _cursor.peek() == 'p' || _cursor.peek() == 'P'))
		{
			bool sawPoint = false;
			if (_cursor.peek() == '.')
			{
				sawPoint = true;
				_cursor.advance();
				while (isHexDigit(_cursor.peek()))
					_cursor.advance();
			}
			const bool hasExponent = (_cursor.peek() == 'p' || _cursor.peek() == 'P') &&
				(isDigit(_cursor.peek(1)) || ((_cursor.peek(1) == '+' || _cursor.peek(1) == '-') && isDigit(_cursor.peek(2))));
			if (hasExponent)
			{
				_cursor.advance();
				if (_cursor.peek() == '+' || _cursor.peek() == '-')
					_cursor.advance();
				while (isDigit(_cursor.peek()))
					_cursor.advance();
			}
			uoffset floatEnd = _cursor.position();
			if (_cursor.peek() == 'f' || _cursor.peek() == 'F' || _cursor.peek() == 'l' || _cursor.peek() == 'L')
				_cursor.advance();
			std::string_view lexeme = _cursor.buffer().substr(startPos, _cursor.position() - startPos);
			std::string_view mantissa = _cursor.buffer().substr(digitsStart, floatEnd - digitsStart);
			if (!hasExponent)
			{
				_diagnostics.error(DiagId::HexFloatWithoutExponent, startLoc,
					"hexadecimal floating literal '{}' needs a 'p' exponent", lexeme);
				return Token::makeLiteralFloat(lexeme, 0.0, startLoc);
			}
			if (mantissa.size() == 0 || mantissa.front() == 'p' || mantissa.front() == 'P' ||
				(sawPoint && mantissa.size() >= 2 && mantissa[0] == '.' && (mantissa[1] == 'p' || mantissa[1] == 'P')))
			{
				_diagnostics.error(DiagId::HexLiteralHasNoDigits, startLoc, "hexadecimal literal has no digits");
				return Token::makeLiteralFloat(lexeme, 0.0, startLoc);
			}
			return makeHexFloatToken(lexeme, mantissa, startLoc);
		}

		uoffset digitsEnd = _cursor.position();
		// A `u`/`U` suffix makes the literal unsigned and `l`/`L` or `ll`/`LL` makes it long or 64-bit.
		// `f`/`F` is a hex DIGIT here, never a float suffix - `0xFFf` is one number - so only the
		// integer suffixes apply to a radix literal (a hex float is read above).
		bool isUnsigned = false;
		bool isLongLong = false;
		bool isLong = false;
		scanIntegerSuffix(isUnsigned, isLongLong, isLong);

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

		return makeIntToken(lexeme, startLoc, base, digits, isUnsigned, isLongLong, isLong);
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

		// A trailing suffix. `u`/`U` marks an integer literal unsigned and `l`/`L` or `ll`/`LL` makes it
		// long or 64-bit; `f`/`F` marks a float, and is also what turns a digit run with no
		// `.`/exponent into one (`1f`). On a float, `l`/`L` is long double - float here, like double -
		// but on a digit run it stays the integer `long` suffix (`1l`). The integer suffixes combine
		// in either order (`1ull`, `1llu`), while a float suffix stands alone.
		uoffset digitsEnd = _cursor.position();
		bool isUnsigned = false;
		bool isLongLong = false;
		bool isLong = false;
		if (isFloat)
		{
			if (_cursor.peek() == 'f' || _cursor.peek() == 'F' || _cursor.peek() == 'l' || _cursor.peek() == 'L')
				_cursor.advance();
		}
		else if (_cursor.peek() == 'f' || _cursor.peek() == 'F')
		{
			isFloat = true;
			_cursor.advance();
		}
		else
		{
			scanIntegerSuffix(isUnsigned, isLongLong, isLong);
		}

		std::string_view lexeme = _cursor.buffer().substr(startPos, _cursor.position() - startPos);
		std::string_view digits = _cursor.buffer().substr(startPos, digitsEnd - startPos);

		if (isFloat)
			return makeFloatToken(lexeme, digits, startLoc);

		// A leading 0 followed by more digits is C's octal prefix: `0755` is 493, not seven hundred
		// and fifty-five. (A lone `0` is octal too, and the same zero either way.) An 8 or a 9 in one
		// is an error rather than a silent decimal.
		if (digits.size() > 1 && digits.front() == '0')
		{
			std::string_view octal = digits.substr(1);
			for (char digit : octal)
			{
				if (digit == '8' || digit == '9')
				{
					_diagnostics.error(DiagId::InvalidOctalDigit, startLoc, "invalid digit '{}' in octal literal '{}'", digit, lexeme);
					return Token::makeLiteralInt(lexeme, 0, startLoc, isUnsigned, isLongLong, isLong, false);
				}
			}
			return makeIntToken(lexeme, startLoc, 8, octal, isUnsigned, isLongLong, isLong);
		}
		return makeIntToken(lexeme, startLoc, 10, digits, isUnsigned, isLongLong, isLong);
	}

	u32 Lexer::literalPrefixLength(uoffset offset, support::LiteralEncoding& encoding) const noexcept
	{
		// 'L', 'u', 'U' or 'u8' right before a quote. Anything else - 'Lx', 'u8x', a lone 'u' - is the
		// start of an identifier, which is what it always was.
		u32 length = 0;
		switch (_cursor.peek(static_cast<ioffset>(offset)))
		{
			case 'L': encoding = support::LiteralEncoding::Wide; length = 1; break;
			case 'U': encoding = support::LiteralEncoding::Utf32; length = 1; break;
			case 'u':
				if (_cursor.peek(static_cast<ioffset>(offset + 1)) == '8')
				{
					encoding = support::LiteralEncoding::Utf8;
					length = 2;
				}
				else
				{
					encoding = support::LiteralEncoding::Utf16;
					length = 1;
				}
				break;
			default: return 0;
		}
		const char quote = _cursor.peek(static_cast<ioffset>(offset + length));
		return quote == '"' || quote == '\'' ? length : 0;
	}

	Lexer::LiteralUnit Lexer::scanEscapeSequence()
	{
		// Called right after the backslash has already been consumed. A named escape, '\x' and an
		// octal escape give a code unit as written; '\u' and '\U' give a code point, which the
		// literal's encoding turns into as many units as it takes.
		SourceLocation loc = currentLocation();

		if (_cursor.isAtEnd() || _cursor.peek() == '\n')
		{
			_diagnostics.error(DiagId::UnterminatedEscapeSequence, loc, "unterminated escape sequence");
			return LiteralUnit{ 0, false, loc };
		}

		char c = _cursor.advance();
		auto unit = [&](u32 value) { return LiteralUnit{ value, false, loc }; };
		switch (c)
		{
			case 'n': return unit('\n');
			case 't': return unit('\t');
			case 'r': return unit('\r');
			case '\\': return unit('\\');
			case '\'': return unit('\'');
			case '"': return unit('"');
			case '?': return unit('?');
			case 'a': return unit('\a');
			case 'b': return unit('\b');
			case 'f': return unit('\f');
			case 'v': return unit('\v');
			case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7':
			{
				// Up to three octal digits: '\0', '\12', '\101'.
				u32 value = static_cast<u32>(c - '0');
				for (int digits = 1; digits < 3 && _cursor.peek() >= '0' && _cursor.peek() <= '7'; ++digits)
					value = value * 8 + static_cast<u32>(_cursor.advance() - '0');
				return unit(value);
			}
			case 'x':
			{
				// Every hex digit that follows belongs to the escape, as in C; the value has to fit
				// one code unit of the literal, which is checked once its encoding is known.
				u64 value = 0;
				int digitCount = 0;
				while (isHexDigit(_cursor.peek()))
				{
					value = value * 16 + static_cast<u64>(hexDigitValue(_cursor.advance()));
					if (value > 0xFFFFFFFFull)
						value = 0x100000000ull; // sticky: too large for any code unit
					digitCount++;
				}

				if (digitCount == 0)
				{
					_diagnostics.error(DiagId::HexEscapeHasNoDigits, loc, "\\x used with no following hex digits");
					return unit(0);
				}
				if (value > 0xFFFFFFFFull)
				{
					_diagnostics.error(DiagId::EscapeValueOutOfRange, loc, "hex escape sequence is out of range");
					return unit(0);
				}
				return unit(static_cast<u32>(value));
			}
			case 'u':
			case 'U':
			{
				// A universal character name: exactly four ('\u') or eight ('\U') hex digits naming a
				// Unicode code point, which may not be a surrogate or lie past U+10FFFF.
				const int wanted = c == 'u' ? 4 : 8;
				u32 value = 0;
				int digitCount = 0;
				while (digitCount < wanted && isHexDigit(_cursor.peek()))
				{
					value = value * 16 + static_cast<u32>(hexDigitValue(_cursor.advance()));
					digitCount++;
				}
				if (digitCount != wanted)
				{
					_diagnostics.error(DiagId::InvalidUniversalCharacterName, loc, "\\{} needs exactly {} hex digits", c, wanted);
					return unit(0);
				}
				if ((value >= 0xD800 && value <= 0xDFFF) || value > 0x10FFFF)
				{
					_diagnostics.error(DiagId::InvalidUniversalCharacterName, loc, "\\{} names U+{:04X}, which is not a Unicode character", c, value);
					return unit(0);
				}
				return LiteralUnit{ value, true, loc };
			}
			default:
				_diagnostics.error(DiagId::UnknownEscapeSequence, loc, "unknown escape sequence '\\{}'", c);
				return unit(static_cast<u8>(c));
		}
	}

	Lexer::LiteralUnit Lexer::scanSourceCharacter(bool decodeUtf8)
	{
		// The source is read as UTF-8. A well-formed sequence is one code point, which a wide literal
		// re-encodes (L"é" is one wchar_t, 0xE9) and a narrow one writes back as the same bytes; a
		// byte that does not start one is kept as a code unit of its own.
		SourceLocation loc = currentLocation();
		const u8 lead = static_cast<u8>(_cursor.advance());
		if (!decodeUtf8 || lead < 0x80)
			return LiteralUnit{ lead, false, loc };

		u32 length = 0;
		u32 value = 0;
		u32 minimum = 0;
		if (lead >= 0xC2 && lead <= 0xDF) { length = 2; value = lead & 0x1Fu; minimum = 0x80; }
		else if (lead >= 0xE0 && lead <= 0xEF) { length = 3; value = lead & 0x0Fu; minimum = 0x800; }
		else if (lead >= 0xF0 && lead <= 0xF4) { length = 4; value = lead & 0x07u; minimum = 0x10000; }
		else
			return LiteralUnit{ lead, false, loc };

		for (u32 i = 1; i < length; ++i)
		{
			const u8 next = static_cast<u8>(_cursor.peek(static_cast<ioffset>(i - 1)));
			if ((next & 0xC0u) != 0x80u)
				return LiteralUnit{ lead, false, loc };
			value = (value << 6) | (next & 0x3Fu);
		}
		if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF))
			return LiteralUnit{ lead, false, loc };

		_cursor.advance(length - 1);
		return LiteralUnit{ value, true, loc };
	}

	namespace
	{
		void appendCodeUnit(std::string& out, u32 unit, u32 size)
		{
			// Little-endian, like the VM: the bytes are the array's memory image as they stand.
			for (u32 i = 0; i < size; ++i)
				out.push_back(static_cast<char>((unit >> (8 * i)) & 0xFFu));
		}
	}

	bool Lexer::encodeLiteralUnit(std::string& out, const LiteralUnit& unit, support::LiteralEncoding encoding)
	{
		const u32 size = support::codeUnitSize(encoding);
		if (!unit.isCodePoint)
		{
			if (unit.value > support::maxCodeUnit(encoding))
			{
				_diagnostics.error(DiagId::EscapeValueOutOfRange, unit.location, "escape sequence value 0x{:X} does not fit a {}-byte {}character",
					unit.value, size, support::literalPrefix(encoding));
				return false;
			}
			appendCodeUnit(out, unit.value, size);
			return true;
		}

		const u32 cp = unit.value;
		switch (encoding)
		{
			case support::LiteralEncoding::Plain:
			case support::LiteralEncoding::Utf8:
				if (cp < 0x80)
					out.push_back(static_cast<char>(cp));
				else if (cp < 0x800)
				{
					out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
					out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
				}
				else if (cp < 0x10000)
				{
					out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
					out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
					out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
				}
				else
				{
					out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
					out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
					out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
					out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
				}
				return true;
			case support::LiteralEncoding::Utf16:
				if (cp < 0x10000)
					appendCodeUnit(out, cp, 2);
				else
				{
					appendCodeUnit(out, 0xD800u + ((cp - 0x10000u) >> 10), 2);
					appendCodeUnit(out, 0xDC00u + ((cp - 0x10000u) & 0x3FFu), 2);
				}
				return true;
			default:
				appendCodeUnit(out, cp, 4);
				return true;
		}
	}

	Token Lexer::scanCharLiteral(support::LiteralEncoding encoding, u32 prefixLength)
	{
		SourceLocation startLoc = currentLocation();
		uoffset startPos = _cursor.position();
		if (prefixLength != 0)
			_cursor.advance(prefixLength);
		_cursor.advance(); // opening '

		LiteralUnit unit{ 0, false, startLoc };
		bool haveUnit = false;

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
				unit = scanEscapeSequence();
			}
			else
			{
				// A plain 'é' stays two bytes, and so the multi-character error below, as before; a
				// prefixed one is the code point the prefix encodes.
				unit = scanSourceCharacter(encoding != support::LiteralEncoding::Plain);
			}
			haveUnit = true;

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

		// One code unit of the literal's encoding, and its value as the literal's type has it: a
		// plain char is signed, so '\xFF' is -1; wchar_t is int; the others are unsigned.
		u32 value = 0;
		if (haveUnit)
		{
			std::string encoded;
			if (encodeLiteralUnit(encoded, unit, encoding))
			{
				const u32 size = support::codeUnitSize(encoding);
				if (encoded.size() != size)
				{
					_diagnostics.error(DiagId::CharacterNotRepresentable, startLoc, "character literal {} needs {} code units; it can hold one",
						lexeme, encoded.size() / size);
				}
				else
				{
					for (u32 i = 0; i < size; ++i)
						value |= static_cast<u32>(static_cast<u8>(encoded[i])) << (8 * i);
				}
			}
		}

		TokenValue::CharValue typed = 0;
		switch (encoding)
		{
			case support::LiteralEncoding::Plain: typed = static_cast<signed char>(static_cast<u8>(value)); break;
			case support::LiteralEncoding::Wide: typed = static_cast<i32>(value); break;
			default: typed = static_cast<TokenValue::CharValue>(value); break;
		}
		return Token::makeLiteralChar(lexeme, typed, startLoc, encoding);
	}

	Token Lexer::scanStringLiteral(support::LiteralEncoding encoding, u32 prefixLength)
	{
		SourceLocation startLoc = currentLocation();
		uoffset startPos = _cursor.position();

		std::vector<LiteralUnit> units;
		uoffset endPos = startPos;

		// C joins adjacent string literals into one, before the grammar ever sees them - which is
		// what makes `"a" "b"` a single 3-byte object rather than a syntax error, and what lets a
		// long string be written over several lines. One token comes out of the whole run, so the
		// parser, sizeof and the .rodata entry all see exactly what the program meant to write.
		//
		// The pieces are read as code points and escapes first and encoded once the run is over,
		// because an unprefixed piece takes the prefix of any other piece (C11 6.4.5p5): "a" L"b"
		// is a wide string, "a" included. Two different prefixes cannot be joined.
		for (;;)
		{
			SourceLocation pieceLoc = currentLocation();
			if (prefixLength != 0)
				_cursor.advance(prefixLength);
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
					units.push_back(scanEscapeSequence());
				}
				else
				{
					units.push_back(scanSourceCharacter(true));
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
			if (_cursor.isAtEnd())
				break;
			support::LiteralEncoding nextEncoding = support::LiteralEncoding::Plain;
			prefixLength = _cursor.peek() == '"' ? 0 : literalPrefixLength(0, nextEncoding);
			if (_cursor.peek(static_cast<ioffset>(prefixLength)) != '"')
				break;
			if (prefixLength != 0 && nextEncoding != encoding)
			{
				if (encoding == support::LiteralEncoding::Plain)
					encoding = nextEncoding;
				else
					_diagnostics.error(DiagId::MixedStringLiteralPrefixes, currentLocation(), "cannot join a {}\"...\" string literal to a {}\"...\" one",
						support::literalPrefix(nextEncoding), support::literalPrefix(encoding));
			}
		}

		std::string encoded;
		encoded.reserve(units.size() * support::codeUnitSize(encoding));
		for (const LiteralUnit& unit : units)
			encodeLiteralUnit(encoded, unit, encoding);

		std::string_view lexeme = _cursor.buffer().substr(startPos, endPos - startPos);
		support::PooledString interned = _stringPool.intern(encoded);
		if (!interned)
			_diagnostics.error(DiagId::StringPoolExhausted, startLoc, "out of memory interning string literal");

		return Token::makeLiteralString(lexeme, interned, startLoc, encoding);
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
