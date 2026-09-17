#include <ceresc/support/line_map.h>
#include <ceresc/support/source_manager.h>

#include "framework.h"

using namespace ceresc;

namespace
{
	// The shape the preprocessor produces for `#include "header.h"` at the top of `main.c`, where
	// the header is two lines long: output lines 1-2 are the header's, output line 3 onward is
	// main.c from its second line.
	support::LineMap twoLineHeaderThenMain(support::SourceId header, support::SourceId main)
	{
		support::LineMap map;
		map.append(1, header, 1);
		map.append(2, header, 2);
		map.append(3, main, 2);
		map.append(4, main, 3);
		return map;
	}
}

TEST(line_map, an_expanded_line_maps_back_to_the_file_and_line_it_was_written_in)
{
	support::SourceManager sources;
	support::SourceId header = sources.registerBuffer("header.h", "int a;\nint b;\n");
	support::SourceId main = sources.registerBuffer("main.c", "#include \"header.h\"\nint c;\nint d;\n");
	support::LineMap map = twoLineHeaderThenMain(header, main);

	support::SourceLocation fromHeader = map.toOriginal(support::SourceLocation{ {}, 2, 5, 0 });
	CHECK(fromHeader.sourceId == header);
	CHECK_EQ(fromHeader.line, u32(2));
	CHECK_EQ(fromHeader.column, u32(5)); // carried across unchanged

	support::SourceLocation fromMain = map.toOriginal(support::SourceLocation{ {}, 4, 1, 0 });
	CHECK(fromMain.sourceId == main);
	CHECK_EQ(fromMain.line, u32(3));
}

TEST(line_map, a_line_the_map_has_no_entry_for_comes_back_unchanged)
{
	support::LineMap empty;
	support::SourceLocation location{ {}, 7, 2, 0 };
	CHECK(empty.toOriginal(location) == location);

	support::SourceManager sources;
	support::SourceId header = sources.registerBuffer("header.h", "int a;\n");
	support::SourceId main = sources.registerBuffer("main.c", "int c;\n");
	support::LineMap map = twoLineHeaderThenMain(header, main);
	support::SourceLocation past{ {}, 99, 1, 0 };
	CHECK(map.toOriginal(past) == past);
}

TEST(line_map, a_location_from_another_buffer_is_never_remapped)
{
	// The bug this rules out: a location that is ALREADY original - a preprocessor diagnostic about
	// a header, or anything produced while a different translation unit was being compiled - would
	// otherwise be looked up by a line number that means nothing in this table, and come back
	// naming a plausible file and a wrong line.
	support::SourceManager sources;
	support::SourceId header = sources.registerBuffer("header.h", "int a;\nint b;\n");
	support::SourceId main = sources.registerBuffer("main.c", "int c;\nint d;\n");
	support::SourceId expanded = sources.registerBuffer("main.c", "int a;\nint b;\nint c;\nint d;\n");

	support::LineMap map = twoLineHeaderThenMain(header, main);
	map.setExpandedSourceId(expanded);

	support::SourceLocation alreadyOriginal{ header, 2, 1, 0 };
	CHECK(map.toOriginal(alreadyOriginal) == alreadyOriginal);

	support::SourceLocation inExpandedText{ expanded, 2, 1, 0 };
	support::SourceLocation mapped = map.toOriginal(inExpandedText);
	CHECK(mapped.sourceId == header);
	CHECK_EQ(mapped.line, u32(2));
}
