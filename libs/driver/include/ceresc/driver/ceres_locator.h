#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// Finding the `ceres` executable that `--run` launches.
//
// Three places say where it is, and the most specific one wins:
//
//   1. --ceres-path <where>   on the command line
//   2. CERES_PATH             in the environment: the standard way to tell every tool where Ceres lives
//   3. PATH                   the ordinary search, first directory that holds `ceres`
//
// <where> and CERES_PATH are each either the directory that holds the executable or the executable itself.
// A place that is given but holds no `ceres` is an ERROR, not a step down the list: a stale CERES_PATH that
// quietly fell through to whatever PATH finds would run a different binary from the one the person set up.
// Unset and empty both mean "not given".
//
// The result is a full path, and that is what gets launched. Handing the bare name `ceres` to the operating
// system would leave the search to it, and on Windows the call `--run` uses does not search PATH or add
// `.exe` at all, so a `ceres` on PATH was never found that way.

namespace ceresc::driver
{
	// What the search reads from the process. Passed in, not read where it is used, so that every
	// combination can be tried without touching the real environment.
	struct CeresEnvironment
	{
		std::optional<std::string> ceresPath; // CERES_PATH, if set
		std::string path;                     // PATH

		static CeresEnvironment fromProcess();
	};

	struct CeresLookup
	{
		std::filesystem::path executable; // absolute; empty when nothing was found
		std::string source;               // "--ceres-path", "CERES_PATH" or "PATH": which of the three found it
		std::string error;                // when nothing was found: what was tried, ready to print

		bool found() const noexcept { return !executable.empty(); }
	};

	CeresLookup locateCeres(std::string_view explicitPath, const CeresEnvironment& environment);
}
