#include <ceresc/support/source_location.h>

#include "framework.h"

using namespace ceresc;
using namespace ceresc::support;

namespace
{
	SourceId testSourceId(u32 value) { return SourceId::make(value); }
}

TEST(source_location, default_constructed_is_fully_zeroed_and_invalid)
{
	SourceLocation loc;
	CHECK(!loc);
	CHECK(loc.isInvalid());
	CHECK(!loc.isValid());
	CHECK_EQ(loc.line, 0u);
	CHECK_EQ(loc.column, 0u);
	CHECK_EQ(loc.offset, 0u);
}

TEST(source_location, explicit_constructor_sets_every_field)
{
	SourceLocation loc(testSourceId(3), 12, 5, 47);
	CHECK(loc.sourceId == testSourceId(3));
	CHECK_EQ(loc.line, 12u);
	CHECK_EQ(loc.column, 5u);
	CHECK_EQ(loc.offset, 47u);
	CHECK(loc.isValid());
	CHECK(static_cast<bool>(loc));
}

TEST(source_location, invalid_when_sourceId_is_invalid_even_with_line_and_column_set)
{
	SourceLocation loc(SourceId::invalid(), 1, 1, 0);
	CHECK(loc.isInvalid());
}

TEST(source_location, invalid_when_line_or_column_is_zero)
{
	SourceLocation withoutLine(testSourceId(1), 0, 1, 0);
	SourceLocation withoutColumn(testSourceId(1), 1, 0, 0);
	CHECK(withoutLine.isInvalid());
	CHECK(withoutColumn.isInvalid());
}

TEST(source_location, ordering_compares_sourceId_then_line_then_column)
{
	SourceLocation a(testSourceId(1), 1, 1, 0);
	SourceLocation b(testSourceId(2), 1, 1, 0);
	CHECK(a < b); // different file: lower sourceId sorts first

	SourceLocation c(testSourceId(1), 1, 1, 0);
	SourceLocation d(testSourceId(1), 2, 1, 0);
	CHECK(c < d); // same file, later line sorts after

	SourceLocation e(testSourceId(1), 5, 3, 0);
	SourceLocation f(testSourceId(1), 5, 8, 0);
	CHECK(e < f); // same line, later column sorts after
}

TEST(source_location, offset_breaks_ties_when_line_and_column_are_equal)
{
	// Shouldn't happen coming from a real SourceManager, but the comparison must still give a
	// strict, deterministic order if it ever does - this is exactly why operator<=> includes
	// offset instead of stopping at column.
	SourceLocation a(testSourceId(1), 4, 2, 10);
	SourceLocation b(testSourceId(1), 4, 2, 11);
	CHECK(a < b);
	CHECK(a != b);
}

TEST(source_location, equal_locations_compare_equal)
{
	SourceLocation a(testSourceId(1), 4, 2, 10);
	SourceLocation b(testSourceId(1), 4, 2, 10);
	CHECK(a == b);
}

TEST(source_location, hash_specialization_agrees_with_equality)
{
	SourceLocation a(testSourceId(1), 4, 2, 10);
	SourceLocation b(testSourceId(1), 4, 2, 10);
	std::hash<SourceLocation> hasher;
	CHECK_EQ(hasher(a), hasher(b));
}
