#include <ceresc/support/source_manager.h>

#include "framework.h"

using namespace ceresc;
using namespace ceresc::support;

TEST(source_manager, register_buffer_returns_a_valid_id_and_preserves_content)
{
	SourceManager sm;
	SourceId id = sm.registerBuffer("test.c", std::string("int x;"));
	CHECK(static_cast<bool>(id));

	const SourceBuffer* buffer = sm.getBuffer(id);
	CHECK(buffer != nullptr);
	CHECK_EQ(buffer->name(), "test.c");
	CHECK_EQ(buffer->buffer(), "int x;");
}

TEST(source_manager, unknown_id_returns_null_buffer_and_invalid_location)
{
	SourceManager sm;
	SourceId neverRegistered = SourceId::make(999);

	CHECK(sm.getBuffer(neverRegistered) == nullptr);
	SourceLocation loc = sm.getLocation(neverRegistered, 0);
	CHECK(loc.isInvalid());
}

TEST(source_manager, first_character_is_line_one_column_one)
{
	SourceManager sm;
	SourceId id = sm.registerBuffer("a.c", std::string("abc"));
	SourceLocation loc = sm.getLocation(id, 0);
	CHECK_EQ(loc.line, 1u);
	CHECK_EQ(loc.column, 1u);
}

TEST(source_manager, offset_right_after_newline_starts_the_next_line_at_column_one)
{
	SourceManager sm;
	SourceId id = sm.registerBuffer("a.c", std::string("ab\ncd\n"));
	SourceLocation loc = sm.getLocation(id, 3); // 'c'
	CHECK_EQ(loc.line, 2u);
	CHECK_EQ(loc.column, 1u);
}

TEST(source_manager, eof_without_trailing_newline_reports_the_real_column)
{
	// This is the case that used to be wrong: EOF right after "cd" (no trailing \n) must be
	// line 2, column 3 - not column 1.
	SourceManager sm;
	SourceId id = sm.registerBuffer("a.c", std::string("ab\ncd"));
	SourceLocation loc = sm.getLocation(id, 5); // offset == buffer size
	CHECK_EQ(loc.line, 2u);
	CHECK_EQ(loc.column, 3u);
}

TEST(source_manager, eof_with_trailing_newline_is_an_empty_line_at_column_one)
{
	SourceManager sm;
	SourceId id = sm.registerBuffer("a.c", std::string("ab\ncd\n"));
	SourceLocation loc = sm.getLocation(id, 6); // offset == buffer size
	CHECK_EQ(loc.line, 3u);
	CHECK_EQ(loc.column, 1u);
}

TEST(source_manager, empty_file_offset_zero_is_line_one_column_one)
{
	SourceManager sm;
	SourceId id = sm.registerBuffer("empty.c", std::string(""));
	SourceLocation loc = sm.getLocation(id, 0);
	CHECK_EQ(loc.line, 1u);
	CHECK_EQ(loc.column, 1u);
}

TEST(source_manager, offset_past_the_end_of_the_buffer_is_invalid)
{
	SourceManager sm;
	SourceId id = sm.registerBuffer("a.c", std::string("abc"));
	SourceLocation loc = sm.getLocation(id, 4); // one past the end
	CHECK(loc.isInvalid());
}

TEST(source_manager, multiple_files_have_independent_ids_and_buffers)
{
	SourceManager sm;
	SourceId first = sm.registerBuffer("first.c", std::string("int a;"));
	SourceId second = sm.registerBuffer("second.c", std::string("int bb;"));

	CHECK(first != second);
	CHECK_EQ(sm.getBuffer(first)->name(), "first.c");
	CHECK_EQ(sm.getBuffer(second)->name(), "second.c");

	// Offsets are per-file, not a shared address space.
	SourceLocation locInFirst = sm.getLocation(first, 4);
	SourceLocation locInSecond = sm.getLocation(second, 4);
	CHECK(locInFirst.sourceId == first);
	CHECK(locInSecond.sourceId == second);
}

TEST(source_manager, varying_line_lengths_translate_correctly)
{
	// line 1: "a"          (1 char + \n)
	// line 2: "bbbbbbbbbb" (10 chars + \n)
	// line 3: "cc"         (2 chars, no trailing \n)
	SourceManager sm;
	SourceId id = sm.registerBuffer("a.c", std::string("a\nbbbbbbbbbb\ncc"));

	SourceLocation onLine2 = sm.getLocation(id, 5); // 4th 'b'
	CHECK_EQ(onLine2.line, 2u);
	CHECK_EQ(onLine2.column, 4u);

	SourceLocation onLine3 = sm.getLocation(id, 14); // last 'c'
	CHECK_EQ(onLine3.line, 3u);
	CHECK_EQ(onLine3.column, 2u);
}
