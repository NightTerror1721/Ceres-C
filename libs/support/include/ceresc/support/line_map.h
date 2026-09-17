#pragma once

#include "source_location.h"
#include <span>
#include <vector>

// LineMap - "this line of the buffer a phase is reading came from that line of that file".
//
// The preprocessor builds one (libs/preprocessor): it concatenates a .c file with everything it
// includes, so line 47 of what the lexer reads may be line 5 of a header. Every SourceLocation
// downstream of the lexer therefore names a line of the EXPANDED text, and anything that shows a
// location to a human has to map it back first - the diagnostics printer, and codegen, whose whole
// output is C locations in the form of trailing comments.
//
// It lives here rather than in libs/preprocessor because of who its consumers are. libs/codegen
// must not know the preprocessor exists (that is the layering the architecture plan's §3 asks
// for), and a map from one location to another is a libs/support concept in exactly the way
// SourceManager and SourceLocation are. libs/preprocessor produces one; libs/driver and
// libs/codegen consume one; none of the three needs the others.
//
// The alternative - emitting `#line` markers and teaching the lexer to read them - would give the
// lexer a feature that is not about lexing. See docs/08-Preprocessor.md.

namespace ceresc::support
{
	// One entry per line of the expanded text. `outputLine` is 1-based, like every line number here.
	struct LineMapEntry
	{
		u32 outputLine = 1;
		SourceId sourceId{};
		u32 sourceLine = 1;
	};

	class LineMap
	{
	private:
		std::vector<LineMapEntry> _entries; // sorted by outputLine, one per output line
		SourceId _expandedSourceId{};       // unset until setExpandedSourceId() - see below

	public:
		void append(u32 outputLine, SourceId sourceId, u32 sourceLine)
		{
			_entries.push_back(LineMapEntry{ outputLine, sourceId, sourceLine });
		}

		std::span<const LineMapEntry> entries() const noexcept { return _entries; }
		bool empty() const noexcept { return _entries.empty(); }

		// Which buffer this map's output lines are lines OF. The producer cannot know it - the
		// expanded text is registered with the SourceManager after the expansion that built this -
		// so whoever registers it says so here, and toOriginal() then leaves every location from
		// any OTHER buffer exactly as it came in.
		//
		// That is not a nicety. A location naming a header is already original, and mapping it a
		// second time would index this table with a line number that means nothing in it: the
		// preprocessor's own diagnostics are exactly that case, and so is every location produced
		// while a different translation unit was being compiled. Leaving it unset maps everything,
		// which is what a test holding a map and nothing else wants.
		void setExpandedSourceId(SourceId sourceId) noexcept { _expandedSourceId = sourceId; }

		// Rewrites a location in the expanded text into one in the file that line was written in.
		// The column is carried across unchanged: a line's text is copied verbatim apart from macro
		// expansion, so the column is right unless a macro on that same line changed the text's
		// length before the position in question - an inaccuracy worth the whole mechanism being
		// this simple.
		//
		// A location this map has no entry for is returned as it came in, which is what makes it
		// safe to run every location through an empty map: a phase driven directly from a string
		// (every unit test below libs/driver) has no preprocessor and therefore no mapping to do.
		SourceLocation toOriginal(SourceLocation location) const noexcept
		{
			if (_expandedSourceId && location.sourceId != _expandedSourceId)
				return location; // not a location in the text this map describes - see above
			if (_entries.empty() || location.line == 0 || location.line > _entries.size())
				return location;
			const LineMapEntry& entry = _entries[location.line - 1];
			return { entry.sourceId, entry.sourceLine, location.column, location.offset };
		}
	};
}
