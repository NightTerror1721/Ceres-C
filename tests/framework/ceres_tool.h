#pragma once

// Driving the real `ceres` binary from a test.
//
// Two suites need this: tests/e2e (§12's end-to-end layer, which compiles a source string, then
// assembles and runs it) and tests/examples (the CI rule from §12: every file under examples/ is
// compiled and run on every build and its output compared against a sibling .expected). Both do
// the same three things - find the binary, run a program with its output captured, read a file back -
// so those three live here instead of once per suite. A program's output is its terminal's, in the
// machine's window, and never the process's stdout (CeresASM plan/v2 F5.7): runProgram() runs it
// headless and reads it from --transcript.
//
// Deliberately header-only and in tests/framework: it is test scaffolding, not a library. Nothing
// under libs/ may depend on it, and libs/driver keeps its own private subprocess launcher, which
// inherits the terminal rather than capturing it - production `ceresc --run` has no reason to read
// its own child's output.
//
// Finding the binary: CERESC_CERES_PATH (a directory holding `ceres`/`ceres.exe`) wins, then a few
// relative locations that cover a CeresASM checkout sitting next to this one. When nothing is
// found the suites SKIP rather than fail, so a checkout of Ceres-C on its own stays green; a CI
// job that means to test this for real is responsible for setting the variable (see
// .github/workflows/ci.yml).

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace ceresc::testing
{
	namespace fs = std::filesystem;

	// Returned by a runner when this environment has no `ceres` to run against. Compared against
	// rather than thrown, so the caller decides whether that is a skip or a failure.
	inline constexpr std::string_view kNoCeres = "<no ceres binary found>";

#if defined(_WIN32)
	inline constexpr std::string_view kCeresExecutableName = "ceres.exe";
#else
	inline constexpr std::string_view kCeresExecutableName = "ceres";
#endif

	inline std::optional<fs::path> findCeresDirectory()
	{
		if (const char* env = std::getenv("CERESC_CERES_PATH"))
		{
			fs::path dir = env;
			if (fs::is_regular_file(dir / kCeresExecutableName))
				return dir;
		}

		// Convenience for a local checkout with CeresASM cloned as a sibling of Ceres-C. CI sets
		// CERESC_CERES_PATH explicitly rather than relying on this, and should: which directory a
		// test runs in is ctest's business, not this project's.
		//
		// Walks up to the filesystem root instead of trying a fixed number of "../" steps, because
		// how deep the working directory sits varies: running a suite by hand from the source root
		// is one level from the sibling, while ctest runs it from build/<preset>/tests/<suite>,
		// which is five. A loop cannot be off by one the way that list was.
		std::error_code error;
		for (fs::path here = fs::current_path(error); !here.empty(); here = here.parent_path())
		{
			fs::path candidate = here / "CeresASM";
			if (fs::is_regular_file(candidate / kCeresExecutableName))
				return candidate;
			if (here == here.parent_path()) // reached the root; parent_path() stops changing there
				break;
		}
		return std::nullopt;
	}

	inline std::string quote(const fs::path& path) { return std::format("\"{}\"", path.string()); }

	// Runs `command` with stdout and stderr redirected into `outputFile`, and returns its exit
	// code. The extra pair of quotes on Windows is cmd.exe's own rule: it strips the outermost
	// quotes of the whole line, so a command that starts with a quoted path needs one more layer
	// or the drive letter is parsed as a separate token.
	inline int runSubprocessCapturingStdout(const std::string& command, const fs::path& outputFile)
	{
		std::string redirected = std::format("{} > \"{}\" 2>&1", command, outputFile.string());
#if defined(_WIN32)
		std::string wrapped = std::format("\"{}\"", redirected);
		return std::system(wrapped.c_str());
#else
		return std::system(redirected.c_str());
#endif
	}

	inline std::string readFile(const fs::path& path)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in)
			return {};
		std::ostringstream buffer;
		buffer << in.rdbuf();
		return buffer.str();
	}

	// What a program wrote to its terminal, from a --transcript file: the error stream's bytes, which the
	// transcript keeps between ESC [ E and ESC [ e, are left where they were written and the markers dropped - so
	// the output reads as it would have on a terminal where both streams show.
	inline std::string plainTranscript(std::string_view transcript)
	{
		std::string result;
		result.reserve(transcript.size());
		for (std::size_t i = 0; i < transcript.size(); ++i)
		{
			if (transcript[i] == '\x1b' && i + 2 < transcript.size() && transcript[i + 1] == '[' &&
				(transcript[i + 2] == 'E' || transcript[i + 2] == 'e'))
			{
				i += 2;
				continue;
			}
			result += transcript[i];
		}
		return result;
	}

	// Runs `ceres run <program> <options>` the way a test needs it (plan/v2 F5.9): no window, as fast as the host
	// goes, and the program's output from --transcript, since a program never writes to the process's own stdout.
	// `typed`, when given, is typed on the program's terminal with --type. Returns the exit status; `output` gets
	// the transcript, then whatever the run itself said on stdout and stderr (its diagnostics), as `2>&1` used to
	// put them together. `stem` names the scratch files, next to it.
	inline int runProgram(const fs::path& ceresBinary, const fs::path& program, std::string_view options,
		const fs::path& stem, std::string& output, std::string_view typed = {})
	{
		const fs::path transcript = fs::path(stem.string() + ".transcript");
		const fs::path hostOutput = fs::path(stem.string() + ".host.txt");
		std::error_code error;
		fs::remove(transcript, error);
		std::string command = std::format("{} run {} --headless --speed max --transcript {}", quote(ceresBinary), quote(program), quote(transcript));
		if (!typed.empty())
		{
			const fs::path typeFile = fs::path(stem.string() + ".typed");
			std::ofstream(typeFile, std::ios::binary) << typed;
			command += " --type " + quote(typeFile);
		}
		if (!options.empty())
			command += std::format(" {}", options);
		const int status = runSubprocessCapturingStdout(command, hostOutput);
		output = plainTranscript(readFile(transcript)) + readFile(hostOutput);
		return status;
	}

	// Drops carriage returns so a golden file compares the same on both platforms. The transcript is
	// written byte for byte, but what `ceres` itself says goes to a TEXT stream on Windows, where '\n'
	// becomes "\r\n". The .expected files are stored with newlines only (see .gitattributes).
	inline std::string withoutCarriageReturns(std::string_view text)
	{
		std::string result;
		result.reserve(text.size());
		for (char c : text)
		{
			if (c != '\r')
				result += c;
		}
		return result;
	}
}
