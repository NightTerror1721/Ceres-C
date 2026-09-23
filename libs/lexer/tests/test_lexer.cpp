#include <ceresc/lexer/lexer.h>
#include <ceresc/support/diagnostics.h>
#include <ceresc/support/string_pool.h>

#include "framework.h"

using namespace ceresc;
using namespace ceresc::lexer;

namespace
{
	support::SourceId testSourceId() { return support::SourceId::make(1); }
}

// ---- identifiers and keywords -----------------------------------------------------------------

TEST(lexer, plain_identifier)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("foo_bar123", testSourceId(), diagnostics, pool);

	Token t = lexer.next();
	CHECK(t.is(TokenKind::Identifier));
	CHECK_EQ(t.lexeme(), "foo_bar123");
	CHECK(!diagnostics.hasDiagnostics());
}

TEST(lexer, identifier_may_start_with_an_underscore)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("_private", testSourceId(), diagnostics, pool);

	CHECK(lexer.next().is(TokenKind::Identifier));
}

TEST(lexer, v1_keyword_is_recognized_as_its_own_kind)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("struct", testSourceId(), diagnostics, pool);

	Token t = lexer.next();
	CHECK(t.isKwStruct());
	CHECK(!t.isIdentifier());
}

TEST(lexer, reserved_keyword_is_still_recognized_by_the_lexer)
{
	// double/union/... are reserved for a version after v1 (token.h), but the lexer itself does
	// not know that - it emits the real keyword kind exactly like any other, same as §5 documents.
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("double union volatile restrict register alignof", testSourceId(), diagnostics, pool);

	CHECK(lexer.next().isKwDouble());
	CHECK(lexer.next().isKwUnion());
	CHECK(lexer.next().isKwVolatile());
	CHECK(lexer.next().isKwRestrict());
	CHECK(lexer.next().isKwRegister());
	CHECK(lexer.next().isKwAlignof());
}

TEST(lexer, true_and_false_are_bool_literals_not_identifiers)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("true false", testSourceId(), diagnostics, pool);

	Token t = lexer.next();
	CHECK(t.isLiteralBool());
	CHECK(t.boolValue());

	Token f = lexer.next();
	CHECK(f.isLiteralBool());
	CHECK(!f.boolValue());
}

// ---- integer literals ---------------------------------------------------------------------------

TEST(lexer, decimal_integer_literal)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("12345", testSourceId(), diagnostics, pool);

	Token t = lexer.next();
	CHECK(t.isLiteralInt());
	CHECK_EQ(t.integralValue(), TokenValue::IntegralValue{ 12345 });
	CHECK(!diagnostics.hasDiagnostics());
}

TEST(lexer, hexadecimal_integer_literal)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("0xFF", testSourceId(), diagnostics, pool);

	Token t = lexer.next();
	CHECK(t.isLiteralInt());
	CHECK_EQ(t.integralValue(), TokenValue::IntegralValue{ 255 });
}

TEST(lexer, binary_integer_literal)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("0b101", testSourceId(), diagnostics, pool);

	Token t = lexer.next();
	CHECK(t.isLiteralInt());
	CHECK_EQ(t.integralValue(), TokenValue::IntegralValue{ 5 });
}

TEST(lexer, hexadecimal_literal_with_no_digits_reports_a_diagnostic)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("0x", testSourceId(), diagnostics, pool);

	Token t = lexer.next();
	CHECK(t.isLiteralInt());
	CHECK_EQ(t.integralValue(), TokenValue::IntegralValue{ 0 });
	CHECK(diagnostics.hasErrors());
}

TEST(lexer, integer_literal_overflow_reports_a_diagnostic)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	// One digit past u64's range.
	Lexer lexer("99999999999999999999", testSourceId(), diagnostics, pool);

	Token t = lexer.next();
	CHECK(t.isLiteralInt());
	CHECK(diagnostics.hasErrors());
}

// ---- float literals ------------------------------------------------------------------------------

TEST(lexer, simple_float_literal)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("1.5", testSourceId(), diagnostics, pool);

	Token t = lexer.next();
	CHECK(t.isLiteralFloat());
	CHECK_EQ(t.floatingValue(), TokenValue::FloatingValue{ 1.5 });
}

TEST(lexer, float_literal_with_a_leading_dot)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer(".5", testSourceId(), diagnostics, pool);

	Token t = lexer.next();
	CHECK(t.isLiteralFloat());
	CHECK_EQ(t.floatingValue(), TokenValue::FloatingValue{ 0.5 });
}

TEST(lexer, float_literal_with_a_trailing_dot_and_no_fraction_digits)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("1.", testSourceId(), diagnostics, pool);

	Token t = lexer.next();
	CHECK(t.isLiteralFloat());
	CHECK_EQ(t.floatingValue(), TokenValue::FloatingValue{ 1.0 });
}

TEST(lexer, float_literal_with_an_exponent)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("1.5e-3", testSourceId(), diagnostics, pool);

	Token t = lexer.next();
	CHECK(t.isLiteralFloat());
	CHECK_EQ(t.floatingValue(), TokenValue::FloatingValue{ 1.5e-3 });
}

TEST(lexer, a_float_literal_accepts_an_f_suffix)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("1.5f", testSourceId(), diagnostics, pool);

	Token t = lexer.next();
	CHECK(t.isLiteralFloat());
	CHECK_EQ(t.floatingValue(), TokenValue::FloatingValue{ 1.5 });
	CHECK(!diagnostics.hasDiagnostics());
}

TEST(lexer, an_f_suffix_turns_a_digit_run_into_a_float)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("1F", testSourceId(), diagnostics, pool);

	Token t = lexer.next();
	CHECK(t.isLiteralFloat());
	CHECK_EQ(t.floatingValue(), TokenValue::FloatingValue{ 1.0 });
}

TEST(lexer, a_u_suffix_marks_an_integer_literal_unsigned)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("42u 43U 0xFFu 0b101U", testSourceId(), diagnostics, pool);

	for (u64 expected : { u64(42), u64(43), u64(255), u64(5) })
	{
		Token t = lexer.next();
		CHECK(t.isLiteralInt());
		CHECK(t.isUnsigned());
		CHECK_EQ(t.integralValue(), expected);
	}
	CHECK(!diagnostics.hasDiagnostics());
}

TEST(lexer, an_unsuffixed_integer_is_not_unsigned)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("42", testSourceId(), diagnostics, pool);

	Token t = lexer.next();
	CHECK(t.isLiteralInt());
	CHECK(!t.isUnsigned());
}

TEST(lexer, an_ll_suffix_marks_an_integer_literal_64_bit)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("42ll 43LL 0xFFll 0b101LL", testSourceId(), diagnostics, pool);

	for (u64 expected : { u64(42), u64(43), u64(255), u64(5) })
	{
		Token t = lexer.next();
		CHECK(t.isLiteralInt());
		CHECK(t.isLongLong());
		CHECK(!t.isUnsigned());
		CHECK_EQ(t.integralValue(), expected);
	}
	CHECK(!diagnostics.hasDiagnostics());
}

TEST(lexer, the_u_and_ll_suffixes_combine_in_either_order)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("1ull 2llu 3ULL 4LLU 5uLL 6llU", testSourceId(), diagnostics, pool);

	for (u64 expected : { u64(1), u64(2), u64(3), u64(4), u64(5), u64(6) })
	{
		Token t = lexer.next();
		CHECK(t.isLiteralInt());
		CHECK(t.isLongLong());
		CHECK(t.isUnsigned());
		CHECK_EQ(t.integralValue(), expected);
	}
	CHECK(!diagnostics.hasDiagnostics());
}

TEST(lexer, a_single_l_suffix_is_not_consumed)
{
	// This subset has no `long` literal suffix (docs/06-Known-Limitations.md), so `1l` stays `1`
	// followed by an identifier `l` - the parser rejects that, rather than the lexer silently
	// dropping the suffix and handing back a plain `int` literal.
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("1l", testSourceId(), diagnostics, pool);

	Token number = lexer.next();
	CHECK(number.isLiteralInt());
	CHECK(!number.isLongLong());
	CHECK(!number.isUnsigned());
	CHECK_EQ(number.integralValue(), u64(1));

	Token identifier = lexer.next();
	CHECK(identifier.isIdentifier());
	CHECK_EQ(identifier.lexeme(), std::string_view("l"));
}

// ---- char literals ---------------------------------------------------------------------------

TEST(lexer, simple_char_literal)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("'a'", testSourceId(), diagnostics, pool);

	Token t = lexer.next();
	CHECK(t.isLiteralChar());
	CHECK_EQ(t.charValue(), 'a');
	CHECK(!diagnostics.hasDiagnostics());
}

TEST(lexer, char_literal_with_a_named_escape)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("'\\n'", testSourceId(), diagnostics, pool);

	Token t = lexer.next();
	CHECK(t.isLiteralChar());
	CHECK_EQ(t.charValue(), '\n');
}

TEST(lexer, char_literal_with_a_hex_escape)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("'\\x41'", testSourceId(), diagnostics, pool); // 0x41 == 'A'

	Token t = lexer.next();
	CHECK(t.isLiteralChar());
	CHECK_EQ(t.charValue(), 'A');
}

TEST(lexer, empty_char_literal_reports_a_diagnostic)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("''", testSourceId(), diagnostics, pool);

	Token t = lexer.next();
	CHECK(t.isLiteralChar());
	CHECK(diagnostics.hasErrors());
}

TEST(lexer, multi_character_literal_reports_a_diagnostic_and_keeps_the_first_character)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("'ab'", testSourceId(), diagnostics, pool);

	Token t = lexer.next();
	CHECK(t.isLiteralChar());
	CHECK_EQ(t.charValue(), 'a');
	CHECK(diagnostics.hasErrors());

	// recovery: the closing quote was consumed, so the next token starts clean after it.
	Token next = lexer.next();
	CHECK(next.is(TokenKind::EndOfFile));
}

TEST(lexer, unterminated_char_literal_reports_a_diagnostic)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("'a", testSourceId(), diagnostics, pool);

	Token t = lexer.next();
	CHECK(t.isLiteralChar());
	CHECK(diagnostics.hasErrors());
}

// ---- string literals -----------------------------------------------------------------------------

TEST(lexer, simple_string_literal)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("\"hello\"", testSourceId(), diagnostics, pool);

	Token t = lexer.next();
	CHECK(t.isLiteralString());
	CHECK_EQ(t.stringValue().view(), "hello");
	CHECK(!diagnostics.hasDiagnostics());
}

TEST(lexer, string_literal_decodes_escapes_in_its_value_but_keeps_the_raw_lexeme)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("\"a\\nb\"", testSourceId(), diagnostics, pool);

	Token t = lexer.next();
	CHECK(t.isLiteralString());
	CHECK_EQ(t.stringValue().view(), "a\nb");   // decoded: 3 bytes
	CHECK_EQ(t.lexeme(), "\"a\\nb\"");          // raw source text: 6 bytes, escape untouched
}

TEST(lexer, unterminated_string_literal_reports_a_diagnostic_and_keeps_what_it_saw)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("\"hello", testSourceId(), diagnostics, pool);

	Token t = lexer.next();
	CHECK(t.isLiteralString());
	CHECK_EQ(t.stringValue().view(), "hello");
	CHECK(diagnostics.hasErrors());
}

TEST(lexer, adjacent_string_literals_are_one_token)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("\"a\" \"b\" \"c\"", testSourceId(), diagnostics, pool);

	Token t = lexer.next();
	CHECK(t.isLiteralString());
	CHECK_EQ(t.stringValue().view(), "abc");
	CHECK_EQ(t.lexeme(), "\"a\" \"b\" \"c\""); // the whole run, as written
	CHECK(lexer.next().isEndOfFile());
	CHECK(!diagnostics.hasDiagnostics());
}

TEST(lexer, a_run_of_string_literals_may_cross_lines_and_comments)
{
	// What the joining is FOR: a long string written over several lines, and the
	// `__DATE__ " " __TIME__` that expands to three of them side by side.
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("\"one \"\n   /* between */ \"two\"", testSourceId(), diagnostics, pool);

	Token t = lexer.next();
	CHECK(t.isLiteralString());
	CHECK_EQ(t.stringValue().view(), "one two");
	CHECK(!diagnostics.hasDiagnostics());
}

TEST(lexer, only_another_string_literal_joins_and_the_next_token_is_untouched)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("\"a\" , \"b\"", testSourceId(), diagnostics, pool);

	Token first = lexer.next();
	CHECK(first.isLiteralString());
	CHECK_EQ(first.stringValue().view(), "a");   // the comma stopped the run
	CHECK(lexer.next().is(TokenKind::Comma));
	Token second = lexer.next();
	CHECK(second.isLiteralString());
	CHECK_EQ(second.stringValue().view(), "b");
	CHECK(lexer.next().isEndOfFile());
}

TEST(lexer, escapes_are_decoded_per_piece_and_a_piece_may_be_empty)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("\"a\\n\" \"\" \"b\"", testSourceId(), diagnostics, pool);

	Token t = lexer.next();
	CHECK(t.isLiteralString());
	CHECK_EQ(t.stringValue().view(), "a\nb");
	CHECK(!diagnostics.hasDiagnostics());
}

TEST(lexer, an_unterminated_piece_in_a_run_is_reported_once_and_ends_the_run)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("\"a\" \"b", testSourceId(), diagnostics, pool);

	Token t = lexer.next();
	CHECK(t.isLiteralString());
	CHECK_EQ(t.stringValue().view(), "ab");
	CHECK_EQ(diagnostics.diagnosticCount(), usize{ 1 });
	CHECK(diagnostics.diagnostics()[0].id == support::DiagnosticId::UnterminatedStringLiteral);
	// And the report points at the piece that was wrong, not at the one that was fine.
	CHECK_EQ(diagnostics.diagnostics()[0].location.column, u32{ 5 });
}

// ---- operators: maximal munch --------------------------------------------------------------------

TEST(lexer, all_operators_and_punctuation_are_recognized)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("( ) { } [ ] , ; : ? ~ . -> -- -= - ++ += + *= * /= / %= % && &= & || |= | ^= ^ != ! == = <<= << <= < >>= >> >= >", testSourceId(), diagnostics, pool);

	const TokenKind expectedKinds[] = {
		TokenKind::LParen, TokenKind::RParen, TokenKind::LBrace, TokenKind::RBrace,
		TokenKind::LBracket, TokenKind::RBracket, TokenKind::Comma, TokenKind::Semicolon,
		TokenKind::Colon, TokenKind::Question, TokenKind::Tilde, TokenKind::Dot, TokenKind::Arrow,
		TokenKind::MinusMinus, TokenKind::MinusEqual, TokenKind::Minus,
		TokenKind::PlusPlus, TokenKind::PlusEqual, TokenKind::Plus,
		TokenKind::StarEqual, TokenKind::Star, TokenKind::SlashEqual, TokenKind::Slash,
		TokenKind::PercentEqual, TokenKind::Percent,
		TokenKind::AmpersandAmpersand, TokenKind::AmpersandEqual, TokenKind::Ampersand,
		TokenKind::PipePipe, TokenKind::PipeEqual, TokenKind::Pipe,
		TokenKind::CaretEqual, TokenKind::Caret, TokenKind::BangEqual, TokenKind::Bang,
		TokenKind::EqualEqual, TokenKind::Equal,
		TokenKind::LessLessEqual, TokenKind::LessLess, TokenKind::LessEqual, TokenKind::Less,
		TokenKind::GreaterGreaterEqual, TokenKind::GreaterGreater, TokenKind::GreaterEqual, TokenKind::Greater,
	};

	for (TokenKind expected : expectedKinds)
		CHECK(lexer.next().is(expected));

	CHECK(lexer.next().isEndOfFile());
	CHECK(!diagnostics.hasDiagnostics());
}

TEST(lexer, shift_assign_is_not_confused_with_shift_or_relational)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("<<= << <= <", testSourceId(), diagnostics, pool);

	CHECK(lexer.next().is(TokenKind::LessLessEqual));
	CHECK(lexer.next().is(TokenKind::LessLess));
	CHECK(lexer.next().is(TokenKind::LessEqual));
	CHECK(lexer.next().is(TokenKind::Less));
}

TEST(lexer, shift_right_assign_is_not_confused_with_shift_or_relational)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer(">>= >> >= >", testSourceId(), diagnostics, pool);

	CHECK(lexer.next().is(TokenKind::GreaterGreaterEqual));
	CHECK(lexer.next().is(TokenKind::GreaterGreater));
	CHECK(lexer.next().is(TokenKind::GreaterEqual));
	CHECK(lexer.next().is(TokenKind::Greater));
}

TEST(lexer, logical_and_is_not_confused_with_bitwise_and_or_and_assign)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("&& &= &", testSourceId(), diagnostics, pool);

	CHECK(lexer.next().is(TokenKind::AmpersandAmpersand));
	CHECK(lexer.next().is(TokenKind::AmpersandEqual));
	CHECK(lexer.next().is(TokenKind::Ampersand));
}

TEST(lexer, increment_and_decrement_are_not_confused_with_plus_minus_or_compound_assign)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("++ += + -- -= -", testSourceId(), diagnostics, pool);

	CHECK(lexer.next().is(TokenKind::PlusPlus));
	CHECK(lexer.next().is(TokenKind::PlusEqual));
	CHECK(lexer.next().is(TokenKind::Plus));
	CHECK(lexer.next().is(TokenKind::MinusMinus));
	CHECK(lexer.next().is(TokenKind::MinusEqual));
	CHECK(lexer.next().is(TokenKind::Minus));
}

TEST(lexer, arrow_is_not_confused_with_minus_followed_by_greater)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("a->b a - >b", testSourceId(), diagnostics, pool);

	CHECK(lexer.next().isIdentifier());       // a
	CHECK(lexer.next().is(TokenKind::Arrow)); // ->
	CHECK(lexer.next().isIdentifier());       // b
	CHECK(lexer.next().isIdentifier());       // a
	CHECK(lexer.next().is(TokenKind::Minus)); // - (separated by whitespace from '>')
	CHECK(lexer.next().is(TokenKind::Greater));
	CHECK(lexer.next().isIdentifier());       // b
}

TEST(lexer, dot_and_arrow_are_distinct_tokens_from_the_lexer_too)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("a.b p->x", testSourceId(), diagnostics, pool);

	CHECK(lexer.next().isIdentifier());      // a
	CHECK(lexer.next().is(TokenKind::Dot));  // .
	CHECK(lexer.next().isIdentifier());      // b
	CHECK(lexer.next().isIdentifier());      // p
	CHECK(lexer.next().is(TokenKind::Arrow));// ->
	CHECK(lexer.next().isIdentifier());      // x
}

// ---- comments --------------------------------------------------------------------------------

TEST(lexer, line_comment_is_skipped_up_to_the_newline)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("a // comment until end of line\nb", testSourceId(), diagnostics, pool);

	Token a = lexer.next();
	CHECK_EQ(a.lexeme(), "a");
	CHECK_EQ(a.location().line, support::LineNumber{ 1 });

	Token b = lexer.next();
	CHECK_EQ(b.lexeme(), "b");
	CHECK_EQ(b.location().line, support::LineNumber{ 2 });
}

TEST(lexer, block_comment_can_span_multiple_lines)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("a /* line one\nline two */ b", testSourceId(), diagnostics, pool);

	CHECK_EQ(lexer.next().lexeme(), "a");
	Token b = lexer.next();
	CHECK_EQ(b.lexeme(), "b");
	CHECK_EQ(b.location().line, support::LineNumber{ 2 });
	CHECK(!diagnostics.hasDiagnostics());
}

TEST(lexer, unterminated_block_comment_reports_a_diagnostic_but_still_reaches_end_of_file)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("a /* never closed", testSourceId(), diagnostics, pool);

	CHECK_EQ(lexer.next().lexeme(), "a");
	CHECK(lexer.next().is(TokenKind::EndOfFile));
	CHECK(diagnostics.hasErrors());
}

// ---- source locations ----------------------------------------------------------------------------

TEST(lexer, tokens_on_later_lines_report_the_correct_line_and_column)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("int x;\nint y;", testSourceId(), diagnostics, pool);

	lexer.next(); // int
	Token x = lexer.next(); // x
	CHECK_EQ(x.location().line, support::LineNumber{ 1 });
	CHECK_EQ(x.location().column, support::ColumnNumber{ 5 });

	lexer.next(); // ;
	Token secondInt = lexer.next(); // int (line 2)
	CHECK_EQ(secondInt.location().line, support::LineNumber{ 2 });
	CHECK_EQ(secondInt.location().column, support::ColumnNumber{ 1 });
}

// ---- error recovery -----------------------------------------------------------------------------

TEST(lexer, an_unrecognized_character_yields_invalid_and_does_not_stop_the_lexer)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("a @ b", testSourceId(), diagnostics, pool);

	CHECK(lexer.next().isIdentifier());       // a

	Token invalid = lexer.next();
	CHECK(invalid.is(TokenKind::Invalid));
	CHECK_EQ(invalid.lexeme(), "@");
	CHECK(diagnostics.hasErrors());

	CHECK(lexer.next().isIdentifier());       // b - lexing continued past the bad character
}

// ---- end of file ----------------------------------------------------------------------------------

TEST(lexer, end_of_file_is_returned_repeatedly_without_advancing_further)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("x", testSourceId(), diagnostics, pool);

	lexer.next(); // x
	Token first = lexer.next();
	Token second = lexer.next();

	CHECK(first.is(TokenKind::EndOfFile));
	CHECK(second.is(TokenKind::EndOfFile));
	CHECK(lexer.isAtEnd());
}

TEST(lexer, empty_source_is_immediately_end_of_file)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("", testSourceId(), diagnostics, pool);

	CHECK(lexer.next().is(TokenKind::EndOfFile));
}

// ---- the variadic ellipsis -------------------------------------------------------------------

TEST(lexer, three_dots_are_one_ellipsis_token)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("(int, ...)", testSourceId(), diagnostics, pool);

	CHECK(lexer.next().isLParen());
	CHECK(lexer.next().isKwInt());
	CHECK(lexer.next().isComma());
	Token ellipsis = lexer.next();
	CHECK(ellipsis.is(TokenKind::Ellipsis));
	CHECK_EQ(ellipsis.lexeme(), std::string_view("..."));
	CHECK(lexer.next().isRParen());
	CHECK(!diagnostics.hasErrors());
}

TEST(lexer, fewer_than_three_dots_stay_dots)
{
	// `..` is not a token in C at all, so maximal munch must not swallow a lone pair into an
	// Ellipsis - each dot comes back on its own for the parser to reject in its own position.
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("a..b.c", testSourceId(), diagnostics, pool);

	CHECK(lexer.next().isIdentifier());
	CHECK(lexer.next().isDot());
	CHECK(lexer.next().isDot());
	CHECK(lexer.next().isIdentifier());
	CHECK(lexer.next().isDot());
	CHECK(lexer.next().isIdentifier());
}

TEST(lexer, four_dots_are_an_ellipsis_followed_by_a_dot)
{
	support::DiagnosticEngine diagnostics;
	support::StringPool pool;
	Lexer lexer("....", testSourceId(), diagnostics, pool);

	CHECK(lexer.next().is(TokenKind::Ellipsis));
	CHECK(lexer.next().isDot());
	CHECK(lexer.next().is(TokenKind::EndOfFile));
}
