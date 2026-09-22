#pragma once

#include <ceresc/support/optimization.h>
#include <ceresc/support/types.h>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <utility>
#include <vector>

// Options - the ceresc command line: what to compile, where to put it, and how hard to try.
//
// §11 of the architecture plan fixes the core set (--emit-ast, --emit-ir, -S, --run, --ceres-path,
// -Werror); the optimization controls (-O0/-O1/-O2 and the per-optimization -f<name>/-fno-<name>
// switches, whose names live in support/optimization.h) and the preprocessor's own (-I, -D, -E)
// grew alongside the phases that needed them.
//
// Optimizations are ON by default: no -O flag means -O1. -O0 selects the simplified behaviour every
// optimization has a counterpart for, which is what makes a miscompilation bisectable and what the
// golden tests pin on both sides - see support/optimization.h's own header comment.
//
// Several inputs are allowed, and they need not all be C: a `.casm` file on the command line is
// assembled and linked alongside the compiled ones, which is how a routine written in assembly
// becomes callable from C (docs/07-CASM-Interop.md). Code that was built already comes in the same
// way: a `.cobj` object or a `.car` archive is linked as it is, and `--decls` says what it defines.

namespace ceresc::driver
{
	struct Options
	{
		// Every input, in command-line order. A `.c` file is compiled; a `.casm` file is passed
		// straight to the assembler; a `.cobj` (an object) and a `.car` (an archive of objects) are
		// handed to the linker untouched. At least one is required (except for --version/--help).
		std::vector<std::string> inputPaths;

		// --decls <file.casm>, repeatable: a declarations file every generated unit imports, in addition
		// to the one ceresc writes for the units it compiles. The assembler picks an opcode from what a
		// name IS (a function, a variable, its size) before anything has an address, so a unit can only
		// name what it has seen declared; ceresc knows the names of what it compiles, and knows nothing
		// of what is inside a `.cobj` or a `.car`. This is how it is told. --emit-decls writes the file
		// for what THIS build defines, so a library can publish its own.
		std::vector<std::string> declsFiles;
		std::string emitDeclsPath;   // --emit-decls <file>

		// -o <path>. With a single C input and no --run, this is the .casm to write. Otherwise it is
		// the linked program, and each unit's .casm is written next to its own source - one output
		// name cannot stand for several files, and inventing a directory layout would be worse than
		// the obvious rule.
		std::string outputPath;

		bool emitAst = false;        // --emit-ast: dump the annotated AST and stop
		bool emitIr = false;         // --emit-ir: dump the IR and stop
		bool emitPreprocessed = false; // -E: dump the preprocessed source and stop

		// --run: also invoke `ceres asm`/`ceres run` as subprocesses. -S is its opposite and the
		// default, and the two resolve in COMMAND-LINE ORDER, exactly like -O and -f do below: the
		// last one written wins. §11 introduces -S as the explicit way to say "stop at .casm text"
		// for the case where a --run is added later out of habit, which only means anything if the
		// later flag is the one that counts.
		bool run = false;

		// --clean removes every artifact the driver created after a successful --run. --clean-keep-casm
		// keeps the generated .casm, removing the objects, final program and shared .decls.casm.
		bool clean = false;
		bool cleanKeepCasm = false;

		std::string ceresPath;       // --ceres-path <dir or executable> - empty means "CERES_PATH, then PATH" (ceres_locator.h)
		std::vector<std::string> runArguments;   // --run-arg <arg>, in order: more for `ceres run` after the program's name
		bool warningsAsErrors = false; // -Werror
		bool showVersion = false;    // --version: print the version and stop, before anything else

		std::vector<std::string> includeDirectories;                  // -I <dir>, in order
		std::vector<std::pair<std::string, std::string>> defines;     // -D NAME[=value], in order

		// -L <dir> and -l <name>: where to find `lib<name>.car`/`.cobj`, and which to link. A
		// program built against the Ceres STDLIB names one archive; this and --sysroot are what let
		// it say so without spelling the path out. Resolved by the driver, then linked exactly like
		// a .car/.cobj given by name.
		std::vector<std::string> libraryDirectories;                  // -L <dir>, in order
		std::vector<std::string> libraries;                           // -l <name>, in order
		// --sysroot <dir>: `<dir>/include` joins the include search and `<dir>/lib` the library
		// search, so a toolchain or the STDLIB install lives under one root.
		std::string sysroot;

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
