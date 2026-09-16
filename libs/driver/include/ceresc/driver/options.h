#pragma once

#include <ceresc/support/optimization.h>
#include <ceresc/support/types.h>
#include <optional>
#include <ostream>
#include <span>
#include <string>

// Options - command-line options: --emit-ast, --emit-ir, -S, --run, --ceres-path, -Werror (§11 of
// the architecture plan), plus the optimization controls: -O0/-O1/-O2 and the per-optimization
// -f<name>/-fno-<name> switches (support/optimization.h owns the list of names).
//
// Optimizations are ON by default: no -O flag means -O1. -O0 selects the simplified behaviour every
// optimization has a counterpart for, which is what makes a miscompilation bisectable and what the
// golden tests pin on both sides - see support/optimization.h's own header comment.
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
		// --run: also invoke `ceres asm`/`ceres run` as subprocesses. -S is its opposite and the
		// default, and the two resolve in COMMAND-LINE ORDER, exactly like -O and -f do below: the
		// last one written wins. §11 introduces -S as the explicit way to say "stop at .casm text"
		// for the case where a --run is added later out of habit, which only means anything if the
		// later flag is the one that counts.
		bool run = false;
		std::string ceresPath;       // --ceres-path <dir> - empty means "look up `ceres` on PATH"
		bool warningsAsErrors = false; // -Werror
		bool showVersion = false;    // --version: print the version and stop, before anything else

		// -O<n> and the -f switches, already resolved into the individual toggles every stage reads.
		// A later -f<name>/-fno-<name> overrides what the -O level set, in command-line order, the
		// same way a C compiler resolves them.
		support::OptimizationOptions optimization = support::OptimizationOptions::forLevel(support::OptimizationLevel::O1);
	};

	// Parses `args` (argv[1:], i.e. without the program name itself). Prints a usage line and
	// returns nullopt on an unrecognized option or a missing input file - the caller's exit code
	// should be non-zero in that case; a request for `--help`/no arguments at all also returns
	// nullopt after printing usage, which is not itself an error (the application entry point
	// distinguishes those requests from malformed argument lists).
	//
	// `--version` is the one flag that parses successfully WITHOUT an input file: it sets
	// showVersion and the caller prints and stops. It is handled here rather than by peeking at
	// argv[1] in main() so that it works wherever it appears on the line, like every other flag.
	std::optional<Options> parseOptions(std::span<const char* const> args, std::ostream& diagnosticsOut);

	// Writes the usage text `parseOptions` prints on `--help`. Exposed so `ceresc --help` and a
	// malformed command line cannot drift apart.
	void printUsage(std::ostream& out);
}
