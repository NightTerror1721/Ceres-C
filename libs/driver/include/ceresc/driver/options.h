#pragma once

#include <ceresc/support/types.h>
#include <optional>
#include <ostream>
#include <span>
#include <string>

// Options - command-line options: --emit-ast, --emit-ir, -S, --run, --ceres-path, -Werror (§11 of
// the architecture plan).
//
// Implemented in Fase 6 (--run, the minimum needed to invoke `ceres asm`/`ceres run` for the
// first end-to-end test) and completed in Fase 8 of the phased plan (§13).

namespace ceresc::driver
{
	struct Options
	{
		std::string inputPath;
		std::string outputPath;      // -o <path> - empty means "next to the input, same stem, .casm"
		bool emitAst = false;        // --emit-ast: dump the annotated AST and stop
		bool emitIr = false;         // --emit-ir: dump the IR and stop
		bool run = false;            // --run: also invoke `ceres asm`/`ceres run` as subprocesses
		std::string ceresPath;       // --ceres-path <dir> - empty means "look up `ceres` on PATH"
		bool warningsAsErrors = false; // -Werror
	};

	// Parses `args` (argv[1:], i.e. without the program name itself). Prints a usage line and
	// returns nullopt on an unrecognized option or a missing input file - the caller's exit code
	// should be non-zero in that case; a request for `--help`/no arguments at all also returns
	// nullopt after printing usage, which is not itself an error (the application entry point
	// distinguishes those requests from malformed argument lists).
	std::optional<Options> parseOptions(std::span<const char* const> args, std::ostream& diagnosticsOut);
}
