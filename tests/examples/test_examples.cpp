#include <ceresc/driver/driver.h>
#include <ceresc/driver/options.h>
#include <ceresc/support/optimization.h>

#include "ceres_tool.h"
#include "framework.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The examples suite - §12's CI rule, and one of Fase 8's deliverables:
//
//     "cada archivo de examples/ se compila y ejecuta automáticamente en cada build, comparando su
//      salida contra un archivo .expected hermano. Un ejemplo que deja de compilar o de dar la
//      salida esperada rompe el build."
//
// So this is not a hand-written list of programs: it walks the real examples/ directory at run
// time. Adding a file there (plus its .expected) puts it under test with no edit here, and
// deleting one takes it out - which is the only way the rule stays true as the directory grows.
//
// Every example runs at -O0, -O1 AND -O2 and all three must print the same bytes, for the same
// reason tests/e2e does it: a golden file proves the output changed, running all three levels
// proves it still means the same thing. An example that disagrees between levels is bisected by
// turning individual -f switches off until it agrees again (support/optimization.h).
//
// The structural checks (every .c has a .expected, and there are at least the fourteen programs
// the plan asks for) run with no `ceres` binary at all, so a checkout without a sibling CeresASM
// still catches a half-added example. The compile-and-run half skips in that case - see
// ceres_tool.h's own note on why skipping beats failing there.

namespace
{
	namespace fs = std::filesystem;
	using namespace ceresc::testing;

	fs::path examplesDirectory()
	{
		return fs::path(CERESC_EXAMPLES_DIR);
	}

	// Sorted, so a failure report reads in the same order as the directory listing and so the run
	// is reproducible on a filesystem that does not enumerate alphabetically.
	std::vector<fs::path> exampleSources()
	{
		std::vector<fs::path> sources;
		std::error_code error;
		for (const fs::directory_entry& entry : fs::directory_iterator(examplesDirectory(), error))
		{
			if (entry.is_regular_file() && entry.path().extension() == ".c")
				sources.push_back(entry.path());
		}
		std::sort(sources.begin(), sources.end());
		return sources;
	}

	fs::path expectedFileFor(const fs::path& source)
	{
		fs::path expected = source;
		expected.replace_extension(".expected");
		return expected;
	}

	// Compiles `source` at `level`, assembles it and runs it for real, returning what the VM
	// printed - or kNoCeres when there is nothing to run it with.
	std::string compileAssembleAndRun(const fs::path& source, ceresc::support::OptimizationLevel level)
	{
		std::optional<fs::path> ceresDir = findCeresDirectory();
		if (!ceresDir)
			return std::string(kNoCeres);

		int levelNumber = static_cast<int>(level);
		fs::path workDir = fs::temp_directory_path() / "ceresc_examples" / std::format("O{}", levelNumber);
		fs::create_directories(workDir);

		std::string stem = source.stem().string();
		fs::path casmPath = workDir / std::format("{}.casm", stem);
		fs::path cresPath = workDir / std::format("{}.cres", stem);
		fs::path outputPath = workDir / std::format("{}.stdout.txt", stem);

		ceresc::driver::Options options;
		options.inputPath = source.string();
		options.outputPath = casmPath.string();
		options.run = false; // driven from here so stdout can be captured - see ceres_tool.h
		options.optimization = ceresc::support::OptimizationOptions::forLevel(level);
		if (ceresc::driver::run(options) != 0)
			return std::format("<ceresc failed to compile {}>", stem);

		fs::path ceresBinary = *ceresDir / kCeresExecutableName;
		std::string asmCommand = std::format("{} asm {} -o {}", quote(ceresBinary), quote(casmPath), quote(cresPath));
		if (runSubprocessCapturingStdout(asmCommand, outputPath) != 0)
			return std::format("<ceres asm failed: {}>", readFile(outputPath));

		std::string runCommand = std::format("{} run {}", quote(ceresBinary), quote(cresPath));
		if (runSubprocessCapturingStdout(runCommand, outputPath) != 0)
			return std::format("<ceres run faulted: {}>", readFile(outputPath));

		return withoutCarriageReturns(readFile(outputPath));
	}
}

TEST(examples, every_example_has_an_expected_output_beside_it)
{
	// A .c with no .expected would silently not be checked by the test below, which is exactly the
	// failure mode this rule exists to prevent.
	for (const fs::path& source : exampleSources())
	{
		fs::path expected = expectedFileFor(source);
		CHECK_EQ(std::format("{}: {}", source.filename().string(), fs::exists(expected)),
			std::format("{}: {}", source.filename().string(), true));
	}
}

TEST(examples, the_directory_holds_at_least_the_fourteen_programs_the_plan_asks_for)
{
	// "Suite examples/ de al menos 14 programas, cada uno con su .expected" (Fase 8). A count, not
	// a list of names: the point is that the suite does not quietly shrink.
	const std::size_t count = exampleSources().size();
	CHECK(count >= 14);
	if (count < 14)
		std::printf("          only %zu example(s) found in %s\n", count, examplesDirectory().string().c_str());
}

TEST(examples, no_expected_file_is_orphaned)
{
	// The other direction: an .expected whose .c was renamed or deleted is dead weight that looks
	// like coverage.
	std::error_code error;
	for (const fs::directory_entry& entry : fs::directory_iterator(examplesDirectory(), error))
	{
		if (!entry.is_regular_file() || entry.path().extension() != ".expected")
			continue;
		fs::path source = entry.path();
		source.replace_extension(".c");
		CHECK_EQ(std::format("{}: {}", entry.path().filename().string(), fs::exists(source)),
			std::format("{}: {}", entry.path().filename().string(), true));
	}
}

TEST(examples, every_example_compiles_assembles_and_prints_what_its_expected_file_says)
{
	using ceresc::support::OptimizationLevel;

	if (!findCeresDirectory())
	{
		std::printf("  (skipped: no sibling CeresASM checkout found - set CERESC_CERES_PATH)\n");
		return;
	}

	for (const fs::path& source : exampleSources())
	{
		std::string name = source.filename().string();
		std::string expected = withoutCarriageReturns(readFile(expectedFileFor(source)));

		for (OptimizationLevel level : { OptimizationLevel::O0, OptimizationLevel::O1, OptimizationLevel::O2 })
		{
			// The file name and the level are inside the compared strings, so a failure says which
			// example disagreed and at which level without needing the surrounding output.
			std::string prefix = std::format("{} -O{}:\n", name, static_cast<int>(level));
			CHECK_EQ(prefix + compileAssembleAndRun(source, level), prefix + expected);
		}
	}
}
