#pragma once

#include <ceresc/support/diagnostic_policy.h>
#include <ceresc/support/diagnostics.h>
#include <ceresc/support/line_map.h>
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
// What it implements:
//
//   #include "file.h"   Relative to the including file's own directory first, then the search path.
//   #include <file.h>   The search path only.
//   #define NAME text   An object-like macro.
//   #define F(x) text   A function-like macro, including `...` / `__VA_ARGS__`.
//                      `#x` inside the body stringifies the argument, and `a ## b` pastes two
//                      tokens into one.
//   #undef NAME         Forgets one.
//   \ at end of line    A backslash immediately before the newline splices the next physical line
//                       onto this one, so a #define body, an #if or any other line may span lines.
//   #if/#elif/#else/#endif and #ifdef/#ifndef conditional compilation.
//   #error/#warning diagnostics from the source.
//   #pragma once        This file contributes nothing if it is included again.
//   #pragma warning(...) Turns one of this compiler's own warnings off, back on, or into an error
//                        over the lines that follow - see docs/11-Diagnostics.md.
//
// Plus the macros the language predefines rather than the program: __LINE__, __FILE__, __DATE__,
// __TIME__, __STDC__, __STDC_HOSTED__ and a few extensions beside them - see
// definePredefinedMacros(), and docs/08-Preprocessor.md for the whole table and what each one is
// worth here.
//
// `#line` is intentionally not implemented. Every other directive is reported by name rather than
// skipped in silence.
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
	// The line map itself lives in libs/support (line_map.h), because libs/codegen consumes one too
	// and must not know this library exists. These aliases are what keep it spelled the way its
	// producer names it.
	using LineMapEntry = support::LineMapEntry;
	using LineMap = support::LineMap;

	struct PreprocessedSource
	{
		std::string text;   // the whole translation unit, includes expanded, macros substituted
		LineMap lineMap;
		// What the program's own `#pragma warning(...)` directives asked for, positioned by line of
		// `text` - so a later phase's warning about a line is governed by whatever was in force at
		// that line, without that phase knowing pragmas exist. Empty for a file that wrote none.
		support::DiagnosticPolicy diagnosticPolicy;
		bool ok = true;     // false when a directive failed; the text is still usable for recovery
	};

	class Preprocessor
	{
	public:
		// A predefined macro whose replacement depends on WHERE it is expanded, so it cannot be a
		// string handed out once at startup the way __DATE__ and __STDC__ are. Every one of these
		// is an ordinary entry in the macro table apart from that - `#ifdef __LINE__` is true and
		// `#undef __FILE__` takes it away, exactly as in C, where doing either is your own problem.
		enum class Builtin : u8
		{
			None,
			Line,         // __LINE__          the line of the ORIGINAL file, not of the expansion
			File,         // __FILE__          the file that line was written in, as a string literal
			BaseFile,     // __BASE_FILE__     the .c the translation unit started from
			IncludeLevel, // __INCLUDE_LEVEL__ 0 in that .c, 1 in a header it includes, and so on
			Counter       // __COUNTER__       0, then 1, then 2 - a fresh number at every expansion
		};

		struct Macro
		{
			std::string replacement;
			std::vector<std::string> parameters;
			bool functionLike = false;
			bool variadic = false;
			Builtin builtin = Builtin::None;
		};

		struct Conditional
		{
			bool parentActive = true;
			bool active = true;
			bool branchTaken = false;
			bool sawElse = false;
			support::SourceLocation location{};
		};

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
		void define(std::string name, std::string replacement)
		{
			_predefines[name] = replacement;
			Macro macro;
			macro.replacement = std::move(replacement);
			_macros[std::move(name)] = std::move(macro);
		}

		// Expands `path` and everything it includes. Registers every file it reads with the
		// SourceManager, so a diagnostic about a header names the header.
		PreprocessedSource run(const std::string& path);

	private:
		// One file's worth of expansion, appended to `out`. `includeStack` is both the recursion
		// guard and what makes a cycle reportable by name rather than as a stack overflow.
		bool expandFile(const std::string& path, PreprocessedSource& out, std::vector<std::string>& includeStack);

		// Resolves an `#include` target to a path that exists, or returns an empty string. `outDirectoryIndex`
		// receives which `_includeDirectories` entry matched (-1 when the target was found next to the
		// including file, or through the bare-name fallback), which is what `#include_next` needs.
		std::string resolveInclude(std::string_view target, bool angled, const std::string& includingFile,
			i32* outDirectoryIndex = nullptr) const;

		// `#include_next`: like `#include`, but searches the include directories AFTER the one that
		// contained `currentFile` (a header's way of reaching the same-named header "further along"
		// the search path). Falls back to searching from the first directory.
		std::string resolveIncludeNext(std::string_view target, const std::string& currentFile,
			i32* outDirectoryIndex = nullptr) const;

		// Substitutes object-like and function-like macros, leaving literals and comments alone.
		std::string expandMacros(std::string_view line, support::SourceLocation location, bool& inBlockComment);
		// `includingFile` is the file the `#if` was written in, so `__has_include("...")` can resolve
		// a quoted target relative to it exactly as `#include "..."` does.
		bool evaluateIfExpression(std::string_view expression, support::SourceLocation location,
			std::string_view includingFile, i64& value);

		// One `#pragma warning(...)` - the body between the parentheses, already trimmed. Records
		// what it asked for against `outputLine` of the expanded text, which is the coordinate every
		// later phase's SourceLocation uses. Anything it cannot read is a warning of its own rather
		// than an error: a pragma is advice, and a compiler that does not understand a piece of it
		// should still compile the program.
		void applyWarningPragma(std::string_view body, support::SourceLocation location, u32 outputLine,
			support::DiagnosticPolicy& policy);

		// Reports `id` unless the policy in force at `outputLine` says otherwise. The preprocessor
		// is the one phase that cannot leave this to the DiagnosticEngine: its own diagnostics point
		// at the ORIGINAL file, not at the expanded text the policy is indexed by, so it is the only
		// one that knows which output line it is on.
		template <typename... Args>
		void policedWarning(support::DiagnosticId id, support::SourceLocation location, u32 outputLine,
			const support::DiagnosticPolicy& policy, std::format_string<Args...> format, Args&&... args)
		{
			switch (policy.actionFor(id, outputLine))
			{
				case support::DiagnosticAction::Ignored:
					return;
				case support::DiagnosticAction::Error:
					_diagnostics.error(id, location, format, std::forward<Args>(args)...);
					return;
				default:
					_diagnostics.warning(id, location, format, std::forward<Args>(args)...);
					return;
			}
		}

		// Seeds the macro table with everything the language predefines, before the command line's
		// own -D macros go in on top - so `-D __STDC_HOSTED__=1` is a program's own decision to
		// make rather than an error. Called once per run().
		void definePredefinedMacros();
		// What a Builtin expands to at `location`. Not const: __COUNTER__ hands out a new number.
		std::string expandBuiltin(Builtin builtin, support::SourceLocation location);

	private:
		support::SourceManager& _sourceManager;
		support::DiagnosticEngine& _diagnostics;
		std::vector<std::string> _includeDirectories;
		std::unordered_map<std::string, std::string> _predefines;
		std::unordered_map<std::string, Macro> _macros;
		std::vector<std::string> _pragmaOnce; // canonical paths that asked not to be included again
		u32 _warningPragmaDepth = 0;          // open `#pragma warning(push)`es, for the unmatched-pop check
		std::string _basePath;                // what run() was given - __BASE_FILE__
		u32 _includeLevel = 0;                // __INCLUDE_LEVEL__, maintained by expandFile()
		u32 _counter = 0;                     // __COUNTER__'s next value
	};
}
