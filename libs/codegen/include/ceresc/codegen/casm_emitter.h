#pragma once

#include <ceresc/support/types.h>
#include <format>
#include <string>
#include <string_view>

// CasmEmitter - deliberately dumb: accumulates indented lines of .casm text, no builder and no
// output-side AST of its own. CodeGen (codegen.h) is the only thing that decides WHAT to write;
// this class only knows HOW a line looks once decided (indentation, a trailing comment, a label's
// lack of one).
//
// Every emitted instruction can carry a comment citing the originating C line (`; file.c:12`) -
// CodeGen builds that text itself (from the SourceLocation each IR instruction/AST node keeps) and
// passes it in already formatted, since resolving a SourceLocation to "file.c:12" needs a
// SourceManager (libs/support), which this class deliberately does not hold - keeping it "dumb" in
// exactly the sense the architecture plan's §10 asks for. This is what makes the output double as
// teaching material, the stated goal of option A over option B in the prior audit.
//
// Implemented in Fase 6 of the phased plan (§13).

namespace ceresc::codegen
{
	class CasmEmitter
	{
	public:
		CasmEmitter() = default;
		CasmEmitter(const CasmEmitter&) = delete;
		CasmEmitter(CasmEmitter&&) = delete;
		~CasmEmitter() = default;

		CasmEmitter& operator=(const CasmEmitter&) = delete;
		CasmEmitter& operator=(CasmEmitter&&) = delete;

	public:
		// A global label, e.g. a function's own entry point - `name:` at column 0, no indentation
		// (this project only ever emits one, file-scope `global` symbol per function - see
		// codegen.cpp - so `global` itself is written by the caller as part of `name`, not a
		// separate parameter here).
		void label(std::string_view name) { _output += std::format("{}:\n", name); }

		// A label local to the nearest preceding global one (10-Language-Syntax.md/
		// 12-Labels-and-Symbols.md) - `.name:`, used for every basic block inside a function so two
		// functions can each have their own `.L0`/`.L1`/... without colliding.
		void localLabel(std::string_view name) { _output += std::format(".{}:\n", name); }

		// One instruction, indented, with an optional trailing comment - `comment` is written
		// verbatim (already formatted, e.g. "suma.c:12") after a `// ` (CASM's own line-comment
		// marker, 10-Language-Syntax.md - NOT `;`, which is not a comment character in this
		// language at all), or omitted entirely when empty.
		void instr(std::string_view text, std::string_view comment = {})
		{
			if (comment.empty())
				_output += std::format("    {}\n", text);
			else
				_output += std::format("    {}{}// {}\n", text, std::string(paddingFor(text), ' '), comment);
		}

		// A raw line of text with no indentation applied - `struct Frame`/`endstruct`, a blank
		// separator line, or a top-level comment describing the function about to follow.
		void raw(std::string_view text) { _output += std::string(text); _output += '\n'; }

		void blank() { _output += '\n'; }

		const std::string& text() const noexcept { return _output; }

		// Returns the accumulated text and resets internal state - used once per function/module by
		// CodeGen.
		std::string take()
		{
			std::string result = std::move(_output);
			_output.clear();
			return result;
		}

	private:
		// Comments line up in a single column when the instruction text is short enough to allow
		// it (matching the hand-written style throughout CeresASM's own examples), and fall back to
		// a single space otherwise rather than a negative pad.
		static usize paddingFor(std::string_view text) noexcept
		{
			constexpr usize column = 22;
			return text.size() + 1 < column ? column - text.size() : 1;
		}

	private:
		std::string _output;
	};
}
