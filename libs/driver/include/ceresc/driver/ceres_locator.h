#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Finding Ceres: the `ceres` executable that `--run` launches, and the C library `--stdlib` compiles against.
//
// Ceres is installed in one directory, the one CERES_PATH names:
//
//   <dir>/ceres, <dir>/ceresc          the tools
//   <dir>/shell/shell.cres             the shell `ceres run` starts
//   <dir>/stdlib/include, stdlib/lib   the C library: its headers, libceres.car and the optional modules
//
// `ceres` is looked for in four places, and the most specific one wins:
//
//   1. --ceres-path <where>   on the command line
//   2. CERES_PATH             in the environment: the standard way to tell every tool where Ceres lives
//   3. the directory ceresc itself is in (an installation keeps the two together)
//   4. PATH                   the ordinary search, first directory that holds `ceres`
//
// <where> and CERES_PATH are each either the directory that holds the executable or the executable itself.
// A place that is given but holds no `ceres` is an ERROR, not a step down the list: a stale CERES_PATH that
// quietly fell through to whatever PATH finds would run a different binary from the one the person set up.
// Unset and empty both mean "not given".
//
// The C library is looked for in the same directories - --ceres-path, else CERES_PATH and then ceresc's own - as
// `stdlib/`; there it is the first that has one (a CERES_PATH that only names where `ceres` is does not stop the
// search), and PATH is never searched: a library is not a program.
//
// The results are full paths. Handing the bare name `ceres` to the operating system would leave the search to
// it, and on Windows the call `--run` uses does not search PATH or add `.exe` at all, so a `ceres` on PATH was
// never found that way.

namespace ceresc::driver
{
	// What the search reads from the process. Passed in, not read where it is used, so that every
	// combination can be tried without touching the real environment.
	struct CeresEnvironment
	{
		std::optional<std::string> ceresPath;       // CERES_PATH, if set
		std::string path;                           // PATH
		std::filesystem::path executableDirectory;  // where the running ceresc is; empty when unknown

		static CeresEnvironment fromProcess();
	};

	struct CeresLookup
	{
		std::filesystem::path executable; // absolute; empty when nothing was found
		std::string source;               // "--ceres-path", "CERES_PATH", "ceresc's directory" or "PATH": which found it
		std::string error;                // when nothing was found: what was tried, ready to print

		bool found() const noexcept { return !executable.empty(); }
	};

	CeresLookup locateCeres(std::string_view explicitPath, const CeresEnvironment& environment);

	// The C library of an installation (--stdlib): `<dir>/stdlib`, with its include and lib directories.
	struct StdlibLookup
	{
		std::filesystem::path includeDirectory;   // absolute; empty when nothing was found
		std::filesystem::path libraryDirectory;
		std::string error;                        // when nothing was found: where it looked, ready to print

		bool found() const noexcept { return !includeDirectory.empty(); }
	};

	StdlibLookup locateStdlib(std::string_view explicitPath, const CeresEnvironment& environment);
}
