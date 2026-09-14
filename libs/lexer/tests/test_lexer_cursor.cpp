#include <ceresc/lexer/lexer.h>

#include "framework.h"

using namespace ceresc;
using namespace ceresc::lexer;

TEST(lexer_cursor, starts_at_line_1_column_1)
{
	LexerCursor cur("abc");
	CHECK_EQ(cur.line(), support::LineNumber{ 1 });
	CHECK_EQ(cur.column(), support::ColumnNumber{ 1 });
	CHECK_EQ(cur.position(), uoffset{ 0 });
}

TEST(lexer_cursor, peek_does_not_move_the_cursor)
{
	LexerCursor cur("ab");
	CHECK_EQ(cur.peek(), 'a');
	CHECK_EQ(cur.peek(), 'a');
	CHECK_EQ(cur.position(), uoffset{ 0 });
}

TEST(lexer_cursor, peek_with_offset_looks_ahead_without_consuming)
{
	LexerCursor cur("abc");
	CHECK_EQ(cur.peek(1), 'b');
	CHECK_EQ(cur.peek(2), 'c');
	CHECK_EQ(cur.peek(3), '\0'); // past the end
	CHECK_EQ(cur.position(), uoffset{ 0 });
}

TEST(lexer_cursor, advance_returns_the_consumed_character_and_moves_the_column)
{
	LexerCursor cur("ab");
	CHECK_EQ(cur.advance(), 'a');
	CHECK_EQ(cur.column(), support::ColumnNumber{ 2 });
	CHECK_EQ(cur.advance(), 'b');
	CHECK_EQ(cur.column(), support::ColumnNumber{ 3 });
}

TEST(lexer_cursor, advancing_past_a_newline_bumps_the_line_and_resets_the_column)
{
	LexerCursor cur("a\nb");
	cur.advance(); // a
	cur.advance(); // \n
	CHECK_EQ(cur.line(), support::LineNumber{ 2 });
	CHECK_EQ(cur.column(), support::ColumnNumber{ 1 });
}

TEST(lexer_cursor, is_at_end_true_once_the_buffer_is_exhausted)
{
	LexerCursor cur("a");
	CHECK(!cur.isAtEnd());
	cur.advance();
	CHECK(cur.isAtEnd());
	CHECK_EQ(cur.advance(), '\0'); // past the end is harmless, does not move
	CHECK_EQ(cur.position(), uoffset{ 1 });
}

TEST(lexer_cursor, bulk_advance_returns_the_character_at_the_starting_position)
{
	LexerCursor cur("abcd");
	char first = cur.advance(3); // consumes "abc"
	CHECK_EQ(first, 'a');
	CHECK_EQ(cur.position(), uoffset{ 3 });
	CHECK_EQ(cur.peek(), 'd');
}

TEST(lexer_cursor, bulk_advance_tracks_every_newline_crossed_in_the_range)
{
	LexerCursor cur("ab\ncd\nef");
	cur.advance(6); // consumes "ab\ncd\n"
	CHECK_EQ(cur.line(), support::LineNumber{ 3 });
	CHECK_EQ(cur.column(), support::ColumnNumber{ 1 });
}

TEST(lexer_cursor, back_undoes_a_non_newline_character_cheaply)
{
	LexerCursor cur("ab");
	cur.advance();
	cur.advance();

	cur.back();
	CHECK_EQ(cur.position(), uoffset{ 1 });
	CHECK_EQ(cur.column(), support::ColumnNumber{ 2 });
	CHECK_EQ(cur.peek(), 'b');
}

TEST(lexer_cursor, back_across_a_newline_recomputes_the_previous_lines_column)
{
	// "ab\ncd" - back() from right after 'c' must land on line 1, column 3 (right after 'b'),
	// not just blindly decrement the column it already had.
	LexerCursor cur("ab\ncd");
	cur.advance(4); // consumes "ab\nc"
	CHECK_EQ(cur.line(), support::LineNumber{ 2 });

	cur.back(); // undo 'c'
	CHECK_EQ(cur.line(), support::LineNumber{ 2 });
	CHECK_EQ(cur.column(), support::ColumnNumber{ 1 });

	cur.back(); // undo '\n'
	CHECK_EQ(cur.line(), support::LineNumber{ 1 });
	CHECK_EQ(cur.column(), support::ColumnNumber{ 3 });
}

TEST(lexer_cursor, bulk_back_recomputes_line_and_column_in_one_go)
{
	LexerCursor cur("ab\ncd\nef");
	cur.advance(7); // consumes "ab\ncd\ne", lands right before 'f'
	cur.back(4);     // back onto 'c', the first character of line 2
	CHECK_EQ(cur.position(), uoffset{ 3 });
	CHECK_EQ(cur.line(), support::LineNumber{ 2 });
	CHECK_EQ(cur.column(), support::ColumnNumber{ 1 });
}

TEST(lexer_cursor, check_does_not_consume_and_match_only_consumes_on_success)
{
	LexerCursor cur("if");
	CHECK(cur.check("if"));
	CHECK(!cur.check("else"));
	CHECK(!cur.match("else"));
	CHECK_EQ(cur.position(), uoffset{ 0 });

	CHECK(cur.match("if"));
	CHECK(cur.isAtEnd());
}

TEST(lexer_cursor, match_tracks_newlines_inside_the_matched_text)
{
	LexerCursor cur("a\nb");
	CHECK(cur.match("a\nb"));
	CHECK_EQ(cur.line(), support::LineNumber{ 2 });
	CHECK_EQ(cur.column(), support::ColumnNumber{ 2 });
}

TEST(lexer_cursor, skip_until_stops_before_the_delimiter_and_tracks_lines_crossed)
{
	LexerCursor cur("ab\ncd*/ef");
	cur.skipUntil("*/");
	CHECK_EQ(cur.line(), support::LineNumber{ 2 });
	CHECK(cur.check("*/"));
}

TEST(lexer_cursor, skip_until_reaches_end_of_buffer_when_the_delimiter_never_appears)
{
	LexerCursor cur("abc");
	cur.skipUntil('z');
	CHECK(cur.isAtEnd());
}

TEST(lexer_cursor, substr_returns_a_view_without_moving_the_cursor)
{
	LexerCursor cur("hello world");
	CHECK_EQ(cur.substr(0, 5), "hello");
	CHECK_EQ(cur.position(), uoffset{ 0 });
}
