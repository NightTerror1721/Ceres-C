#include <ceresc/lexer/token.h>
#include <ceresc/support/string_pool.h>

#include "framework.h"

using namespace ceresc;
using namespace ceresc::lexer;

namespace
{
	support::SourceLocation locAt(u32 line, u32 column, u32 offset = 0)
	{
		return support::SourceLocation(support::SourceId::make(1), line, column, offset);
	}
}

TEST(token, default_constructed_token_is_invalid)
{
	Token t;
	CHECK(t.isInvalid());
	CHECK(!t.isValid());
	CHECK(t.is(TokenKind::Invalid));
}

TEST(token, make_identifier_carries_no_value)
{
	Token t = Token::makeIdentifier("foo", locAt(1, 1));
	CHECK(t.is(TokenKind::Identifier));
	CHECK(t.isIdentifier());
	CHECK(!t.isKeyword());
	CHECK_EQ(t.lexeme(), "foo");
}

TEST(token, make_literal_int_carries_the_parsed_value)
{
	Token t = Token::makeLiteralInt("42", 42, locAt(1, 1));
	CHECK(t.isLiteralInt());
	CHECK(t.isLiteral());
	CHECK_EQ(t.integralValue(), TokenValue::IntegralValue{ 42 });
}

TEST(token, make_literal_float_carries_the_parsed_value)
{
	Token t = Token::makeLiteralFloat("1.5", 1.5, locAt(1, 1));
	CHECK(t.isLiteralFloat());
	CHECK_EQ(t.floatingValue(), TokenValue::FloatingValue{ 1.5 });
}

TEST(token, make_literal_char_carries_the_decoded_value)
{
	Token t = Token::makeLiteralChar("'\\n'", '\n', locAt(1, 1));
	CHECK(t.isLiteralChar());
	CHECK_EQ(t.charValue(), '\n');
}

TEST(token, make_literal_bool_carries_true_or_false)
{
	Token trueToken = Token::makeLiteralBool("true", true, locAt(1, 1));
	Token falseToken = Token::makeLiteralBool("false", false, locAt(1, 1));
	CHECK(trueToken.boolValue());
	CHECK(!falseToken.boolValue());
}

TEST(token, make_literal_string_carries_the_interned_value)
{
	support::StringPool pool;
	support::PooledString interned = pool.intern("hello");

	Token t = Token::makeLiteralString("\"hello\"", interned, locAt(1, 1));
	CHECK(t.isLiteralString());
	CHECK_EQ(t.stringValue().view(), "hello");
}

TEST(token, keyword_factories_produce_their_own_fixed_lexeme)
{
	Token t = Token::makeKwStruct(locAt(1, 1));
	CHECK(t.isKwStruct());
	CHECK(t.isKeyword());
	CHECK_EQ(t.lexeme(), "struct");
}

TEST(token, is_keyword_is_false_for_punctuation_and_literals)
{
	CHECK(!Token::makeLParen(locAt(1, 1)).isKeyword());
	CHECK(!Token::makeLiteralInt("1", 1, locAt(1, 1)).isKeyword());
}

TEST(token, dot_and_arrow_are_distinct_tokens)
{
	Token dot = Token::makeDot(locAt(1, 1));
	Token arrow = Token::makeArrow(locAt(1, 1));

	CHECK(dot.is(TokenKind::Dot));
	CHECK(arrow.is(TokenKind::Arrow));
	CHECK(!dot.is(TokenKind::Arrow));
	CHECK_EQ(dot.lexeme(), ".");
	CHECK_EQ(arrow.lexeme(), "->");
}

TEST(token, location_is_preserved)
{
	Token t = Token::makeIdentifier("x", locAt(3, 7, 20));
	CHECK(t.location() == locAt(3, 7, 20));
}

TEST(token, equality_compares_kind_lexeme_value_and_location)
{
	Token a = Token::makeLiteralInt("1", 1, locAt(1, 1));
	Token b = Token::makeLiteralInt("1", 1, locAt(1, 1));
	Token c = Token::makeLiteralInt("1", 2, locAt(1, 1));

	CHECK(a == b);
	CHECK(!(a == c));
}
