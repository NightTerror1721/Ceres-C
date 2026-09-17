#pragma once

#include "diagnostic_id.h"
#include "source_location.h"
#include <vector>

// DiagnosticPolicy - what a program's own `#pragma warning(...)` directives asked for, as a list of
// changes positioned in the source.
//
// The preprocessor builds it (it is the only phase that sees a directive at all) and records each
// change against the line of the EXPANDED text it appeared on - the same coordinate every later
// phase's SourceLocation uses, which is what lets a pragma written in the middle of a function
// affect a sema diagnostic about the line below it without sema knowing pragmas exist.
//
// Changes are replayed rather than folded. push/pop stay in the list as markers and the query walks
// the whole prefix up to the line it is asked about, which costs a few dozen comparisons on a
// pragma-free file and is why a nested push/pop cannot go subtly wrong the way an incremental fold
// with a wildcard in it can:
//
//     #pragma warning(push)
//     #pragma warning(disable: 2001)   // off in here
//     #pragma warning(pop)             // and exactly as it was again out here
//
// See docs/11-Diagnostics.md.

namespace ceresc::support
{
	enum class DiagnosticAction : u8
	{
		Default, // whatever the command line says - a warning, or an error under -Werror
		Ignored, // not reported at all
		Warning, // a warning even under -Werror
		Error    // an error, whatever the command line says
	};

	class DiagnosticPolicy
	{
	public:
		// `all` in a pragma: matches every controllable diagnostic. Not a real number, so it can
		// never collide with one.
		static constexpr u16 kAll = 0xFFFE;

	private:
		// kPush/kPop occupy the same field as a diagnostic number so the whole history is one
		// ordered list. Neither is a number a pragma could write.
		static constexpr u16 kPush = 0xFFFD;
		static constexpr u16 kPop  = 0xFFFC;

		struct Change
		{
			u32 line = 0;
			u16 number = 0;
			DiagnosticAction action = DiagnosticAction::Default;
		};

		std::vector<Change> _changes; // in source order, by construction
		SourceId _controlledSourceId{};

	public:
		bool empty() const noexcept { return _changes.empty(); }

		void set(u32 line, u16 number, DiagnosticAction action) { _changes.push_back({ line, number, action }); }
		void push(u32 line) { _changes.push_back({ line, kPush, DiagnosticAction::Default }); }
		void pop(u32 line) { _changes.push_back({ line, kPop, DiagnosticAction::Default }); }

		// Which buffer the lines above are lines of. Set by whoever registers the expanded text with
		// the SourceManager, for the same reason LineMap needs it: a location from another buffer -
		// a different translation unit, or a file the preprocessor was still reading - shares no
		// coordinate system with this list and must not be looked up in it.
		void setControlledSourceId(SourceId sourceId) noexcept { _controlledSourceId = sourceId; }
		SourceId controlledSourceId() const noexcept { return _controlledSourceId; }

		// What the program asked for about `id` at `line`. Default when it asked for nothing.
		DiagnosticAction actionFor(DiagnosticId id, u32 line) const
		{
			if (_changes.empty() || !isWarningId(id))
				return DiagnosticAction::Default;

			std::vector<Change> state;              // the changes in force, last match wins
			std::vector<std::vector<Change>> saved; // one entry per open push
			for (const Change& change : _changes)
			{
				if (change.line > line)
					break;
				if (change.number == kPush)
				{
					saved.push_back(state);
				}
				else if (change.number == kPop)
				{
					// A pop with nothing pushed is diagnosed where it is read, not here; ignoring it
					// leaves the state alone, which is the least surprising thing to compile.
					if (!saved.empty())
					{
						state = std::move(saved.back());
						saved.pop_back();
					}
				}
				else
				{
					state.push_back(change);
				}
			}

			DiagnosticAction action = DiagnosticAction::Default;
			for (const Change& change : state)
				if (change.number == kAll || change.number == diagnosticNumber(id))
					action = change.action;
			return action;
		}
	};
}
