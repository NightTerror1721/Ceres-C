#pragma once

#include <ceresc/support/diagnostics.h>
#include <ceresc/support/source_location.h>
#include <ceresc/support/source_manager.h>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Preprocessor - a text-to-text pass that runs BEFORE the lexer, exactly as the architecture plan
// describes it (§5's note that the preprocessor is "una fase de texto-a-texto que corre antes del
// Lexer", not a TokenKind). It turns a source file plus everything it includes into one buffer for
// libs/lexer to read, and a line map that says where each line of that buffer came from.
//
// What it implements, and nothing else:
//
//   #include "file.h"   Relative to the including file's own directory first, then the search path.
//   #include <file.h>   The search path only.
//   #define NAME text   An object-like macro: every later NAME identifier becomes `text`.
//   #undef NAME         Forgets one.
//   #pragma once        This file contributes nothing if it is included again.
//
// Macros with arguments, #if/#ifdef/#else/#endif, #error, #line and token pasting are not here.
// That is a deliberate line, not an oversight, and every other directive is reported as a
// diagnostic naming itself rather than being skipped in silence.
//
// `#pragma once` carries real weight because of what is missing around it: without #ifndef there is
// no other way to write an include guard, so a header included twice would declare everything
// twice. It is the one thing that makes headers usable at all in this version.
//
// ---- why a line map, and not #line markers ----------------------------------------------------
//
// Every diagnostic in this compiler carries a SourceLocation, and a location that pointed into the
// expanded text would name a line number nobody can find - "error at line 412" of a file whose
// original is 30 lines long. The alternative most preprocessors take is to emit `#line` markers and
// have the lexer understand them, which means the lexer grows a feature that is not about lexing.
//
// So instead the expansion records, for every line it writes, which file and which line of it that
// came from. libs/driver maps a diagnostic's location back through that table before printing it,
// and nothing between the lexer and codegen has to know the preprocessor exists.
//
// Implemented in Fase 9 of the phased plan (§13), which is where it was always scheduled.

namespace ceresc::preprocessor
{
	// One entry per line of the expanded text. `outputLine` is 1-based, like every line number here.
	struct LineMapEntry
	{
		u32 outputLine = 1;
		support::SourceId sourceId{};
		u32 sourceLine = 1;
	};

	class LineMap
	{
	private:
		std::vector<LineMapEntry> _entries; // sorted by outputLine, one per output line

	public:
		void append(u32 outputLine, support::SourceId sourceId, u32 sourceLine)
		{
			_entries.push_back(LineMapEntry{ outputLine, sourceId, sourceLine });
		}

		std::span<const LineMapEntry> entries() const noexcept { return _entries; }
		bool empty() const noexcept { return _entries.empty(); }

		// Rewrites a location in the expanded text into one in the file that line was written in.
		// The column is carried across unchanged: a line's text is copied verbatim apart from macro
		// expansion, so the column is right unless a macro on that same line changed the text's
		// length before the position in question - an inaccuracy worth the whole mechanism being
		// this simple.
		support::SourceLocation toOriginal(support::SourceLocation location) const noexcept;
	};

	struct PreprocessedSource
	{
		std::string text;   // the whole translation unit, includes expanded, macros substituted
		LineMap lineMap;
		bool ok = true;     // false when a directive failed; the text is still usable for recovery
	};

	class Preprocessor
	{
	public:
		Preprocessor(support::SourceManager& sourceManager, support::DiagnosticEngine& diagnostics) noexcept :
			_sourceManager(sourceManager), _diagnostics(diagnostics)
		{}
		Preprocessor(const Preprocessor&) = delete;
		Preprocessor(Preprocessor&&) = delete;
		~Preprocessor() = default;

		Preprocessor& operator=(const Preprocessor&) = delete;
		Preprocessor& operator=(Preprocessor&&) = delete;

	public:
		// Where `#include <...>` looks, in order. `#include "..."` looks next to the including file
		// first, then here - the same two-step every C compiler uses.
		void addIncludeDirectory(std::string directory) { _includeDirectories.push_back(std::move(directory)); }

		// Predefines an object-like macro, as `-D NAME=value` on the command line does.
		void define(std::string name, std::string replacement) { _macros[std::move(name)] = std::move(replacement); }

		// Expands `path` and everything it includes. Registers every file it reads with the
		// SourceManager, so a diagnostic about a header names the header.
		PreprocessedSource run(const std::string& path);

	private:
		// One file's worth of expansion, appended to `out`. `includeStack` is both the recursion
		// guard and what makes a cycle reportable by name rather than as a stack overflow.
		bool expandFile(const std::string& path, PreprocessedSource& out, std::vector<std::string>& includeStack);

		// Resolves an `#include` target to a path that exists, or returns an empty string.
		std::string resolveInclude(std::string_view target, bool angled, const std::string& includingFile) const;

		// Substitutes every object-like macro in `line`, leaving string literals, character literals
		// and comments alone. Rescans its own output so one macro may expand into another, bounded
		// by a small pass limit rather than by a recursion guard - a macro that expands to itself
		// stops being rewritten instead of looping forever.
		std::string expandMacros(std::string_view line, support::SourceLocation location);

	private:
		support::SourceManager& _sourceManager;
		support::DiagnosticEngine& _diagnostics;
		std::vector<std::string> _includeDirectories;
		std::unordered_map<std::string, std::string> _macros;
		std::vector<std::string> _pragmaOnce; // canonical paths that asked not to be included again
	};
}
