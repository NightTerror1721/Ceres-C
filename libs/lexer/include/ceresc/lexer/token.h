#pragma once

#include <ceresc/support/types.h>
#include <ceresc/support/source_location.h>
#include <ceresc/support/string_pool.h>
#include <variant>

// TokenKind, TokenValue and Token (§5 of the architecture plan).
//
// TokenValue is a small closed std::variant over the handful of literal value shapes a token can
// carry (u64 for IntLiteral, char for CharLiteral, string_view for StringLiteral) - same pattern
// as ceres::casm::TokenPayload in CeresASM. Token itself views into the SourceManager's buffer
// (libs/support), it never owns its lexeme.
//
// TokenKind is bigger than what libs/parser/libs/ast can build today (Fase 1): most of it is V1
// scope that just hasn't had its phase yet (e.g. KwSwitch exists here well before SwitchStmt exists
// in stmt.h - that's ordinary phased development, see §13, not a language restriction). A small
// explicitly-marked group at the end of the keyword list is different: it's reserved for a version
// *after* v1 entirely (no f64 support in Ceres at all, `union`'s overlapping-storage layout, etc).
// Once the parser exists, it must reject a token from that reserved group with a diagnostic saying
// the construct isn't implemented in this version - never silently misparse it. Until each V1
// keyword's own phase lands, the parser simply doesn't call the scanner path that produces it yet;
// no separate rejection logic needed for those. The C preprocessor is not part of this token set at
// all (it's in the reserved group as a phase, not as a token): `#include`/`#define` would be
// resolved by a separate pass that runs before the Lexer even sees the buffer, the same way a real
// C toolchain treats phase 4 as distinct from tokenization.
//
// Implemented in Fase 1 of the phased plan (§13).

namespace ceresc::lexer
{
	using support::SourceLocation;

	enum class TokenKind : u8
	{
		// Control
		Invalid,			// A character that is not part of the grammar, e.g. `@` or `#` in C.
		EndOfFile,			// The end of the input buffer, after the last character.

		// Literals (V1)
		LiteralInt,			// 123, 0x7B, 0b101
		LiteralFloat,		// 1.23, 1e-3 (f32 - see type.h; no f64 support in Ceres, so no LiteralDouble)
		LiteralChar,		// 'a', '\n', '\x7F'
		LiteralBool,		// true, false
		LiteralString,		// "hello", "world\n"

		// Identifiers and Keywords (V1 - built out across Fase 1-8, see §13; a keyword existing here
		// does not mean its AST node/parser support exists yet, only that it is in scope for v1)
		Identifier,			// foo, bar, my_variable
		KwVoid,				// void
		KwChar,				// char
		KwShort,			// short
		KwInt,				// int
		KwLong,				// long
		KwFloat,			// float (f32 - free: maps directly onto Ceres's native F32, see type.h)
		KwBool,				// bool
		KwSigned,			// signed
		KwUnsigned,			// unsigned
		KwConst,			// const
		KwStruct,			// struct
		KwEnum,				// enum
		KwTypedef,			// typedef
		KwIf,				// if
		KwElse,				// else
		KwWhile,			// while
		KwFor,				// for
		KwDo,				// do
		KwSwitch,			// switch
		KwCase,				// case
		KwDefault,			// default
		KwGoto,				// goto
		KwReturn,			// return
		KwBreak,			// break
		KwContinue,			// continue
		KwInline,			// inline
		KwStatic,			// static
		KwExtern,			// extern
		KwAuto,				// auto
		KwSizeof,			// sizeof

		// Keywords (reserved for a version after v1 - lexed now, parser must reject with a "not
		// implemented in this version" diagnostic; unlike the V1 group above, these are not on the
		// phased plan at all yet)
		KwDouble,			// double (f64 - unlike float, Ceres has no f64 support at all: not free)
		KwUnion,			// union
		KwVolatile,			// volatile
		KwRestrict,			// restrict
		KwRegister,			// register
		KwAlignof,			// alignof
		KwInterrupt,		// __interrupt - marks a function as a VM interrupt handler (docs/10-Interrupts.md)
		KwInterruptVector,	// __interrupt_vector - points one vector number at such a handler

		// Punctuation and Operators
		LParen,				// (
		RParen,				// )
		LBrace,				// {
		RBrace,				// }
		LBracket,			// [
		RBracket,			// ]
		Comma,				// ,
		Semicolon,			// ;
		Colon,				// :
		Question,			// ?
		Dot,				// .
		Arrow,				// ->
		Ellipsis,			// ... (the variadic parameter marker - only ever valid last in a parameter list)
		Plus,				// +
		Minus,				// -
		Star,				// *
		Slash,				// /
		Percent,			// %
		PlusPlus,			// ++
		MinusMinus,			// --
		Ampersand,			// &
		Pipe,				// |
		Caret,				// ^
		Tilde,				// ~
		AmpersandAmpersand,	// &&
		PipePipe,			// ||
		Bang,				// !
		Less,				// <
		LessEqual,			// <=
		Greater,			// >
		GreaterEqual,		// >=
		EqualEqual,			// ==
		BangEqual,			// !=
		LessLess,			// <<
		GreaterGreater,		// >>
		Equal,				// =
		PlusEqual,			// +=
		MinusEqual,			// -=
		StarEqual,			// *=
		SlashEqual,			// /=
		PercentEqual,		// %=
		AmpersandEqual,		// &=
		PipeEqual,			// |=
		CaretEqual,			// ^=
		LessLessEqual,		// <<=
		GreaterGreaterEqual,// >>=
	};

	class TokenValue
	{
	public:
		using IntegralValue = u64;
		using FloatingValue = f64;
		using CharValue = char;
		using BoolValue = bool;
		using StringValue = support::PooledString;
		using ValueVariant = std::variant<std::monostate, IntegralValue, FloatingValue, CharValue, BoolValue, StringValue>;

	private:
		ValueVariant _value;

	public:
		constexpr TokenValue() noexcept = default;
		constexpr TokenValue(const TokenValue&) noexcept = default;
		constexpr TokenValue(TokenValue&&) noexcept = default;
		constexpr ~TokenValue() noexcept = default;

		constexpr TokenValue& operator=(const TokenValue&) noexcept = default;
		constexpr TokenValue& operator=(TokenValue&&) noexcept = default;

		constexpr bool operator==(const TokenValue&) const noexcept = default;

	private:
		constexpr explicit TokenValue(ValueVariant&& value) noexcept : _value(std::move(value)) {}

	public:
		constexpr bool hasValue() const noexcept { return !_value.valueless_by_exception() && !std::holds_alternative<std::monostate>(_value); }
		constexpr bool isInvalid() const noexcept { return std::holds_alternative<std::monostate>(_value); }
		constexpr bool isIntegral() const noexcept { return std::holds_alternative<IntegralValue>(_value); }
		constexpr bool isFloating() const noexcept { return std::holds_alternative<FloatingValue>(_value); }
		constexpr bool isChar() const noexcept { return std::holds_alternative<CharValue>(_value); }
		constexpr bool isBool() const noexcept { return std::holds_alternative<BoolValue>(_value); }
		constexpr bool isString() const noexcept { return std::holds_alternative<StringValue>(_value); }

		constexpr IntegralValue getIntegral() const noexcept { return std::get<IntegralValue>(_value); }
		constexpr FloatingValue getFloating() const noexcept { return std::get<FloatingValue>(_value); }
		constexpr CharValue getChar() const noexcept { return std::get<CharValue>(_value); }
		constexpr BoolValue getBool() const noexcept { return std::get<BoolValue>(_value); }
		constexpr StringValue getString() const noexcept { return std::get<StringValue>(_value); }

	public:
		static forceinline constexpr TokenValue makeInvalid() noexcept { return TokenValue(ValueVariant(std::monostate{})); }
		static forceinline constexpr TokenValue makeIntegral(IntegralValue value) noexcept { return TokenValue(ValueVariant(value)); }
		static forceinline constexpr TokenValue makeFloating(FloatingValue value) noexcept { return TokenValue(ValueVariant(value)); }
		static forceinline constexpr TokenValue makeChar(CharValue value) noexcept { return TokenValue(ValueVariant(value)); }
		static forceinline constexpr TokenValue makeBool(BoolValue value) noexcept { return TokenValue(ValueVariant(value)); }
		static forceinline constexpr TokenValue makeString(StringValue value) noexcept { return TokenValue(ValueVariant(value)); }
	};

	class Token
	{
	private:
		TokenKind		 _kind		= TokenKind::Invalid;
		std::string_view _lexeme	= {};
		TokenValue		 _value		= {};
		SourceLocation	 _location	= {};
		bool			 _isUnsigned = false; // an integer literal's `u`/`U` suffix
		bool			 _isLongLong = false; // an integer literal's `ll`/`LL` suffix

	public:
		constexpr Token() noexcept = default;
		constexpr Token(const Token&) noexcept = default;
		constexpr Token(Token&&) noexcept = default;
		constexpr ~Token() noexcept = default;

		constexpr Token& operator=(const Token&) noexcept = default;
		constexpr Token& operator=(Token&&) noexcept = default;

		constexpr bool operator==(const Token&) const noexcept = default;

	private:
		constexpr Token(TokenKind kind, std::string_view lexeme, TokenValue value, SourceLocation location, bool isUnsigned = false, bool isLongLong = false) noexcept :
			_kind(kind),
			_lexeme(lexeme),
			_value(value),
			_location(location),
			_isUnsigned(isUnsigned),
			_isLongLong(isLongLong)
		{}

	public:
		constexpr TokenKind kind() const noexcept { return _kind; }
		constexpr std::string_view lexeme() const noexcept { return _lexeme; }
		constexpr TokenValue value() const noexcept { return _value; }
		constexpr SourceLocation location() const noexcept { return _location; }
		// True only for an integer literal written with a `u`/`U` suffix (`42u`) - the one suffix that
		// changes a literal's TYPE rather than its kind, so it has to ride along on the token.
		constexpr bool isUnsigned() const noexcept { return _isUnsigned; }
		// True for an integer literal written with an `ll`/`LL` suffix (`42ll`, `0xFFull`), which
		// names the 64-bit type. Paired with isUnsigned() it picks between `long long` and
		// `unsigned long long`; alone it is `long long`.
		constexpr bool isLongLong() const noexcept { return _isLongLong; }

		constexpr TokenValue::IntegralValue integralValue() const noexcept { return _value.getIntegral(); }
		constexpr TokenValue::FloatingValue floatingValue() const noexcept { return _value.getFloating(); }
		constexpr TokenValue::CharValue charValue() const noexcept { return _value.getChar(); }
		constexpr TokenValue::BoolValue boolValue() const noexcept { return _value.getBool(); }
		constexpr TokenValue::StringValue stringValue() const noexcept { return _value.getString(); }

		constexpr bool isValid() const noexcept { return _kind != TokenKind::Invalid; }
		constexpr bool isInvalid() const noexcept { return _kind == TokenKind::Invalid; }

		constexpr bool is(TokenKind kind) const noexcept { return _kind == kind; }

		constexpr bool isEndOfFile() const noexcept { return _kind == TokenKind::EndOfFile; }
		constexpr bool isLiteral() const noexcept
		{
			switch (_kind)
			{
				case TokenKind::LiteralInt:
				case TokenKind::LiteralFloat:
				case TokenKind::LiteralChar:
				case TokenKind::LiteralBool:
				case TokenKind::LiteralString:
					return true;
				default:
					return false;
			}
		}
		constexpr bool isLiteralInt() const noexcept { return _kind == TokenKind::LiteralInt; }
		constexpr bool isLiteralFloat() const noexcept { return _kind == TokenKind::LiteralFloat; }
		constexpr bool isLiteralChar() const noexcept { return _kind == TokenKind::LiteralChar; }
		constexpr bool isLiteralBool() const noexcept { return _kind == TokenKind::LiteralBool; }
		constexpr bool isLiteralString() const noexcept { return _kind == TokenKind::LiteralString; }
		constexpr bool isIdentifier() const noexcept { return _kind == TokenKind::Identifier; }
		constexpr bool isKeyword() const noexcept
		{
			switch (_kind)
			{
				case TokenKind::KwVoid:
				case TokenKind::KwChar:
				case TokenKind::KwShort:
				case TokenKind::KwInt:
				case TokenKind::KwLong:
				case TokenKind::KwFloat:
				case TokenKind::KwDouble:
				case TokenKind::KwBool:
				case TokenKind::KwSigned:
				case TokenKind::KwUnsigned:
				case TokenKind::KwConst:
				case TokenKind::KwStruct:
				case TokenKind::KwEnum:
				case TokenKind::KwUnion:
				case TokenKind::KwTypedef:
				case TokenKind::KwIf:
				case TokenKind::KwElse:
				case TokenKind::KwWhile:
				case TokenKind::KwFor:
				case TokenKind::KwReturn:
				case TokenKind::KwBreak:
				case TokenKind::KwContinue:
				case TokenKind::KwSwitch:
				case TokenKind::KwCase:
				case TokenKind::KwDefault:
				case TokenKind::KwGoto:
				case TokenKind::KwDo:
				case TokenKind::KwInline:
				case TokenKind::KwStatic:
				case TokenKind::KwExtern:
				case TokenKind::KwRegister:
				case TokenKind::KwAuto:
				case TokenKind::KwVolatile:
				case TokenKind::KwRestrict:
				case TokenKind::KwSizeof:
				case TokenKind::KwAlignof:
				case TokenKind::KwInterrupt:
				case TokenKind::KwInterruptVector:
				return true;
			default:
				return false;
			}
		}

		constexpr bool isKwVoid() const noexcept { return _kind == TokenKind::KwVoid; }
		constexpr bool isKwChar() const noexcept { return _kind == TokenKind::KwChar; }
		constexpr bool isKwShort() const noexcept { return _kind == TokenKind::KwShort; }
		constexpr bool isKwInt() const noexcept { return _kind == TokenKind::KwInt; }
		constexpr bool isKwLong() const noexcept { return _kind == TokenKind::KwLong; }
		constexpr bool isKwFloat() const noexcept { return _kind == TokenKind::KwFloat; }
		constexpr bool isKwDouble() const noexcept { return _kind == TokenKind::KwDouble; }
		constexpr bool isKwBool() const noexcept { return _kind == TokenKind::KwBool; }
		constexpr bool isKwSigned() const noexcept { return _kind == TokenKind::KwSigned; }
		constexpr bool isKwUnsigned() const noexcept { return _kind == TokenKind::KwUnsigned; }
		constexpr bool isKwConst() const noexcept { return _kind == TokenKind::KwConst; }
		constexpr bool isKwStruct() const noexcept { return _kind == TokenKind::KwStruct; }
		constexpr bool isKwEnum() const noexcept { return _kind == TokenKind::KwEnum; }
		constexpr bool isKwUnion() const noexcept { return _kind == TokenKind::KwUnion; }
		constexpr bool isKwTypedef() const noexcept { return _kind == TokenKind::KwTypedef; }
		constexpr bool isKwIf() const noexcept { return _kind == TokenKind::KwIf; }
		constexpr bool isKwElse() const noexcept { return _kind == TokenKind::KwElse; }
		constexpr bool isKwWhile() const noexcept { return _kind == TokenKind::KwWhile; }
		constexpr bool isKwFor() const noexcept { return _kind == TokenKind::KwFor; }
		constexpr bool isKwReturn() const noexcept { return _kind == TokenKind::KwReturn; }
		constexpr bool isKwBreak() const noexcept { return _kind == TokenKind::KwBreak; }
		constexpr bool isKwContinue() const noexcept { return _kind == TokenKind::KwContinue; }
		constexpr bool isKwSwitch() const noexcept { return _kind == TokenKind::KwSwitch; }
		constexpr bool isKwCase() const noexcept { return _kind == TokenKind::KwCase; }
		constexpr bool isKwDefault() const noexcept { return _kind == TokenKind::KwDefault; }
		constexpr bool isKwGoto() const noexcept { return _kind == TokenKind::KwGoto; }
		constexpr bool isKwDo() const noexcept { return _kind == TokenKind::KwDo; }
		constexpr bool isKwInline() const noexcept { return _kind == TokenKind::KwInline; }
		constexpr bool isKwStatic() const noexcept { return _kind == TokenKind::KwStatic; }
		constexpr bool isKwExtern() const noexcept { return _kind == TokenKind::KwExtern; }
		constexpr bool isKwRegister() const noexcept { return _kind == TokenKind::KwRegister; }
		constexpr bool isKwInterrupt() const noexcept { return _kind == TokenKind::KwInterrupt; }
		constexpr bool isKwInterruptVector() const noexcept { return _kind == TokenKind::KwInterruptVector; }
		constexpr bool isKwAuto() const noexcept { return _kind == TokenKind::KwAuto; }
		constexpr bool isKwVolatile() const noexcept { return _kind == TokenKind::KwVolatile; }
		constexpr bool isKwRestrict() const noexcept { return _kind == TokenKind::KwRestrict; }
		constexpr bool isKwSizeof() const noexcept { return _kind == TokenKind::KwSizeof; }
		constexpr bool isKwAlignof() const noexcept { return _kind == TokenKind::KwAlignof; }

		constexpr bool isLParen() const noexcept { return _kind == TokenKind::LParen; }
		constexpr bool isRParen() const noexcept { return _kind == TokenKind::RParen; }
		constexpr bool isLBrace() const noexcept { return _kind == TokenKind::LBrace; }
		constexpr bool isRBrace() const noexcept { return _kind == TokenKind::RBrace; }
		constexpr bool isLBracket() const noexcept { return _kind == TokenKind::LBracket; }
		constexpr bool isRBracket() const noexcept { return _kind == TokenKind::RBracket; }
		constexpr bool isComma() const noexcept { return _kind == TokenKind::Comma; }
		constexpr bool isSemicolon() const noexcept { return _kind == TokenKind::Semicolon; }
		constexpr bool isColon() const noexcept { return _kind == TokenKind::Colon; }
		constexpr bool isQuestion() const noexcept { return _kind == TokenKind::Question; }
		constexpr bool isDot() const noexcept { return _kind == TokenKind::Dot; }
		constexpr bool isArrow() const noexcept { return _kind == TokenKind::Arrow; }
		constexpr bool isEllipsis() const noexcept { return _kind == TokenKind::Ellipsis; }
		constexpr bool isPlus() const noexcept { return _kind == TokenKind::Plus; }
		constexpr bool isMinus() const noexcept { return _kind == TokenKind::Minus; }
		constexpr bool isStar() const noexcept { return _kind == TokenKind::Star; }
		constexpr bool isSlash() const noexcept { return _kind == TokenKind::Slash; }
		constexpr bool isPercent() const noexcept { return _kind == TokenKind::Percent; }
		constexpr bool isPlusPlus() const noexcept { return _kind == TokenKind::PlusPlus; }
		constexpr bool isMinusMinus() const noexcept { return _kind == TokenKind::MinusMinus; }
		constexpr bool isAmpersand() const noexcept { return _kind == TokenKind::Ampersand; }
		constexpr bool isPipe() const noexcept { return _kind == TokenKind::Pipe; }
		constexpr bool isCaret() const noexcept { return _kind == TokenKind::Caret; }
		constexpr bool isTilde() const noexcept { return _kind == TokenKind::Tilde; }
		constexpr bool isAmpersandAmpersand() const noexcept { return _kind == TokenKind::AmpersandAmpersand; }
		constexpr bool isPipePipe() const noexcept { return _kind == TokenKind::PipePipe; }
		constexpr bool isBang() const noexcept { return _kind == TokenKind::Bang; }
		constexpr bool isLess() const noexcept { return _kind == TokenKind::Less; }
		constexpr bool isLessEqual() const noexcept { return _kind == TokenKind::LessEqual; }
		constexpr bool isGreater() const noexcept { return _kind == TokenKind::Greater; }
		constexpr bool isGreaterEqual() const noexcept { return _kind == TokenKind::GreaterEqual; }
		constexpr bool isEqualEqual() const noexcept { return _kind == TokenKind::EqualEqual; }
		constexpr bool isBangEqual() const noexcept { return _kind == TokenKind::BangEqual; }
		constexpr bool isLessLess() const noexcept { return _kind == TokenKind::LessLess; }
		constexpr bool isGreaterGreater() const noexcept { return _kind == TokenKind::GreaterGreater; }
		constexpr bool isEqual() const noexcept { return _kind == TokenKind::Equal; }
		constexpr bool isPlusEqual() const noexcept { return _kind == TokenKind::PlusEqual; }
		constexpr bool isMinusEqual() const noexcept { return _kind == TokenKind::MinusEqual; }
		constexpr bool isStarEqual() const noexcept { return _kind == TokenKind::StarEqual; }
		constexpr bool isSlashEqual() const noexcept { return _kind == TokenKind::SlashEqual; }
		constexpr bool isPercentEqual() const noexcept { return _kind == TokenKind::PercentEqual; }
		constexpr bool isAmpersandEqual() const noexcept { return _kind == TokenKind::AmpersandEqual; }
		constexpr bool isPipeEqual() const noexcept { return _kind == TokenKind::PipeEqual; }
		constexpr bool isCaretEqual() const noexcept { return _kind == TokenKind::CaretEqual; }
		constexpr bool isLessLessEqual() const noexcept { return _kind == TokenKind::LessLessEqual; }
		constexpr bool isGreaterGreaterEqual() const noexcept { return _kind == TokenKind::GreaterGreaterEqual; }

	private:
		static forceinline constexpr Token makeWithoutValue(TokenKind kind, std::string_view lexeme, SourceLocation location) noexcept
		{
			return Token(kind, lexeme, TokenValue{}, location);
		}

	public:
		static forceinline constexpr Token makeInvalid(std::string_view lexeme, SourceLocation location) noexcept { return makeWithoutValue(TokenKind::Invalid, lexeme, location); }
		static forceinline constexpr Token makeEndOfFile(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::EndOfFile, {}, location); }
		static forceinline constexpr Token makeLiteralInt(std::string_view lexeme, TokenValue::IntegralValue value, SourceLocation location, bool isUnsigned = false, bool isLongLong = false) noexcept { return Token(TokenKind::LiteralInt, lexeme, TokenValue::makeIntegral(value), location, isUnsigned, isLongLong); }
		static forceinline constexpr Token makeLiteralFloat(std::string_view lexeme, TokenValue::FloatingValue value, SourceLocation location) noexcept { return Token(TokenKind::LiteralFloat, lexeme, TokenValue::makeFloating(value), location); }
		static forceinline constexpr Token makeLiteralChar(std::string_view lexeme, TokenValue::CharValue value, SourceLocation location) noexcept { return Token(TokenKind::LiteralChar, lexeme, TokenValue::makeChar(value), location); }
		static forceinline constexpr Token makeLiteralBool(std::string_view lexeme, TokenValue::BoolValue value, SourceLocation location) noexcept { return Token(TokenKind::LiteralBool, lexeme, TokenValue::makeBool(value), location); }
		static forceinline constexpr Token makeLiteralString(std::string_view lexeme, TokenValue::StringValue value, SourceLocation location) noexcept { return Token(TokenKind::LiteralString, lexeme, TokenValue::makeString(value), location); }
		static forceinline constexpr Token makeIdentifier(std::string_view lexeme, SourceLocation location) noexcept { return makeWithoutValue(TokenKind::Identifier, lexeme, location); }

		static forceinline constexpr Token makeKwVoid(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::KwVoid, "void", location); }
		static forceinline constexpr Token makeKwChar(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::KwChar, "char", location); }
		static forceinline constexpr Token makeKwShort(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::KwShort, "short", location); }
		static forceinline constexpr Token makeKwInt(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::KwInt, "int", location); }
		static forceinline constexpr Token makeKwLong(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::KwLong, "long", location); }
		static forceinline constexpr Token makeKwFloat(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::KwFloat, "float", location); }
		static forceinline constexpr Token makeKwDouble(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::KwDouble, "double", location); }
		static forceinline constexpr Token makeKwBool(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::KwBool, "bool", location); }
		static forceinline constexpr Token makeKwSigned(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::KwSigned, "signed", location); }
		static forceinline constexpr Token makeKwUnsigned(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::KwUnsigned, "unsigned", location); }
		static forceinline constexpr Token makeKwConst(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::KwConst, "const", location); }
		static forceinline constexpr Token makeKwStruct(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::KwStruct, "struct", location); }
		static forceinline constexpr Token makeKwEnum(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::KwEnum, "enum", location); }
		static forceinline constexpr Token makeKwUnion(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::KwUnion, "union", location); }
		static forceinline constexpr Token makeKwTypedef(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::KwTypedef, "typedef", location); }
		static forceinline constexpr Token makeKwIf(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::KwIf, "if", location); }
		static forceinline constexpr Token makeKwElse(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::KwElse, "else", location); }
		static forceinline constexpr Token makeKwWhile(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::KwWhile, "while", location); }
		static forceinline constexpr Token makeKwFor(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::KwFor, "for", location); }
		static forceinline constexpr Token makeKwReturn(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::KwReturn, "return", location); }
		static forceinline constexpr Token makeKwBreak(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::KwBreak, "break", location); }
		static forceinline constexpr Token makeKwContinue(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::KwContinue, "continue", location); }
		static forceinline constexpr Token makeKwSwitch(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::KwSwitch, "switch", location); }
		static forceinline constexpr Token makeKwCase(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::KwCase, "case", location); }
		static forceinline constexpr Token makeKwDefault(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::KwDefault, "default", location); }
		static forceinline constexpr Token makeKwGoto(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::KwGoto, "goto", location); }
		static forceinline constexpr Token makeKwDo(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::KwDo, "do", location); }
		static forceinline constexpr Token makeKwInline(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::KwInline, "inline", location); }
		static forceinline constexpr Token makeKwStatic(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::KwStatic, "static", location); }
		static forceinline constexpr Token makeKwExtern(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::KwExtern, "extern", location); }
		static forceinline constexpr Token makeKwRegister(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::KwRegister, "register", location); }
		static forceinline constexpr Token makeKwAuto(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::KwAuto, "auto", location); }
		static forceinline constexpr Token makeKwVolatile(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::KwVolatile, "volatile", location); }
		static forceinline constexpr Token makeKwRestrict(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::KwRestrict, "restrict", location); }
		static forceinline constexpr Token makeKwSizeof(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::KwSizeof, "sizeof", location); }
		static forceinline constexpr Token makeKwAlignof(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::KwAlignof, "alignof", location); }
		static forceinline constexpr Token makeKwInterrupt(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::KwInterrupt, "__interrupt", location); }
		static forceinline constexpr Token makeKwInterruptVector(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::KwInterruptVector, "__interrupt_vector", location); }
		static forceinline constexpr Token makeLParen(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::LParen, "(", location); }
		static forceinline constexpr Token makeRParen(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::RParen, ")", location); }
		static forceinline constexpr Token makeLBrace(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::LBrace, "{", location); }
		static forceinline constexpr Token makeRBrace(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::RBrace, "}", location); }
		static forceinline constexpr Token makeLBracket(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::LBracket, "[", location); }
		static forceinline constexpr Token makeRBracket(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::RBracket, "]", location); }
		static forceinline constexpr Token makeComma(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::Comma, ",", location); }
		static forceinline constexpr Token makeSemicolon(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::Semicolon, ";", location); }
		static forceinline constexpr Token makeColon(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::Colon, ":", location); }
		static forceinline constexpr Token makeQuestion(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::Question, "?", location); }
		static forceinline constexpr Token makeDot(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::Dot, ".", location); }
		static forceinline constexpr Token makeArrow(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::Arrow, "->", location); }
		static forceinline constexpr Token makeEllipsis(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::Ellipsis, "...", location); }
		static forceinline constexpr Token makePlus(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::Plus, "+", location); }
		static forceinline constexpr Token makeMinus(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::Minus, "-", location); }
		static forceinline constexpr Token makeStar(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::Star, "*", location); }
		static forceinline constexpr Token makeSlash(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::Slash, "/", location); }
		static forceinline constexpr Token makePercent(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::Percent, "%", location); }
		static forceinline constexpr Token makePlusPlus(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::PlusPlus, "++", location); }
		static forceinline constexpr Token makeMinusMinus(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::MinusMinus, "--", location); }
		static forceinline constexpr Token makeAmpersand(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::Ampersand, "&", location); }
		static forceinline constexpr Token makePipe(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::Pipe, "|", location); }
		static forceinline constexpr Token makeCaret(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::Caret, "^", location); }
		static forceinline constexpr Token makeTilde(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::Tilde, "~", location); }
		static forceinline constexpr Token makeAmpersandAmpersand(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::AmpersandAmpersand, "&&", location); }
		static forceinline constexpr Token makePipePipe(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::PipePipe, "||", location); }
		static forceinline constexpr Token makeBang(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::Bang, "!", location); }
		static forceinline constexpr Token makeLess(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::Less, "<", location); }
		static forceinline constexpr Token makeLessEqual(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::LessEqual, "<=", location); }
		static forceinline constexpr Token makeGreater(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::Greater, ">", location); }
		static forceinline constexpr Token makeGreaterEqual(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::GreaterEqual, ">=", location); }
		static forceinline constexpr Token makeEqualEqual(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::EqualEqual, "==", location); }
		static forceinline constexpr Token makeBangEqual(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::BangEqual, "!=", location); }
		static forceinline constexpr Token makeLessLess(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::LessLess, "<<", location); }
		static forceinline constexpr Token makeGreaterGreater(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::GreaterGreater, ">>", location); }
		static forceinline constexpr Token makeEqual(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::Equal, "=", location); }
		static forceinline constexpr Token makePlusEqual(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::PlusEqual, "+=", location); }
		static forceinline constexpr Token makeMinusEqual(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::MinusEqual, "-=", location); }
		static forceinline constexpr Token makeStarEqual(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::StarEqual, "*=", location); }
		static forceinline constexpr Token makeSlashEqual(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::SlashEqual, "/=", location); }
		static forceinline constexpr Token makePercentEqual(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::PercentEqual, "%=", location); }
		static forceinline constexpr Token makeAmpersandEqual(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::AmpersandEqual, "&=", location); }
		static forceinline constexpr Token makePipeEqual(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::PipeEqual, "|=", location); }
		static forceinline constexpr Token makeCaretEqual(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::CaretEqual, "^=", location); }
		static forceinline constexpr Token makeLessLessEqual(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::LessLessEqual, "<<=", location); }
		static forceinline constexpr Token makeGreaterGreaterEqual(SourceLocation location) noexcept { return makeWithoutValue(TokenKind::GreaterGreaterEqual, ">>=", location); }
	};
}
