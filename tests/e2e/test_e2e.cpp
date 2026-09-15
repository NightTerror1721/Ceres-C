#include <ceresc/driver/driver.h>
#include <ceresc/driver/options.h>

#include "framework.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

// The end-to-end suite (§12 of the architecture plan): compile a real .c file, invoke `ceres asm`
// and `ceres run` as real subprocesses (no mocks - libs/driver is the only library allowed to
// spawn one, and it does exactly this same thing for `ceresc --run`), and check the ACTUAL
// computed value, not just a successful exit.
//
// `ceres run`'s own process exit code is always 0 on a clean halt and 1 on a VM fault - never a
// program-chosen value (verified against CeresASM's Ceres/libs/driver/src/machine_runner.cpp:
// there is no register-to-exit-code channel at all). So every fixture here pokes CeresASM's
// TerminalDevice MMIO register directly (a raw pointer cast + dereferenced store - this project
// has no printf yet) and this suite reads the resulting byte back from `ceres run`'s own captured
// stdout - the same mechanism codegen.h's own header comment describes `main` needing to halt
// through instead of an ordinary `ret`.
//
// Needs a sibling CeresASM checkout with a built `ceres` binary to run at all - set
// CERESC_CERES_PATH to its directory (containing `ceres`/`ceres.exe`) if it is not one of the
// common relative locations this looks for. Skips (passes trivially, with a printed note) rather
// than failing when no such binary can be found, so a checkout of Ceres-C alone - without its
// sibling - does not fail this suite; that absence is exactly what a CI job running this suite for
// real is responsible for avoiding by setting the environment variable.

namespace
{
	namespace fs = std::filesystem;

#if defined(_WIN32)
	constexpr std::string_view kCeresExecutableName = "ceres.exe";
#else
	constexpr std::string_view kCeresExecutableName = "ceres";
#endif

	std::optional<fs::path> findCeresDirectory()
	{
		if (const char* env = std::getenv("CERESC_CERES_PATH"))
		{
			fs::path dir = env;
			if (fs::is_regular_file(dir / kCeresExecutableName))
				return dir;
		}
		// Convenience for a local checkout with CeresASM cloned as a sibling of Ceres-C - CI should
		// set CERESC_CERES_PATH explicitly rather than relying on this.
		for (std::string_view candidate : { "../CeresASM", "../../CeresASM", "../../../CeresASM", "../../../../CeresASM" })
		{
			fs::path dir = candidate;
			if (fs::is_regular_file(dir / kCeresExecutableName))
				return dir;
		}
		return std::nullopt;
	}

	std::string quote(const fs::path& path) { return std::format("\"{}\"", path.string()); }

	// Mirrors driver.cpp's own runSubprocess()/cmd.exe-quoting workaround, plus stdout redirection
	// to `outputFile` - needed here (and not in libs/driver itself) only so this suite can read
	// back what the VM actually printed; production `ceresc --run` has no reason to capture its
	// own child's output instead of just inheriting the terminal it already has.
	int runSubprocessCapturingStdout(const std::string& command, const fs::path& outputFile)
	{
		std::string redirected = std::format("{} > \"{}\" 2>&1", command, outputFile.string());
#if defined(_WIN32)
		std::string wrapped = std::format("\"{}\"", redirected);
		return std::system(wrapped.c_str());
#else
		return std::system(redirected.c_str());
#endif
	}

	std::string readFile(const fs::path& path)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in)
			return {};
		std::ostringstream buffer;
		buffer << in.rdbuf();
		return buffer.str();
	}

	// Compiles `source` (a whole .c program), assembles it and runs it for real, returning what
	// `ceres run` printed to stdout - or "<no ceres binary found>" if this environment has no
	// sibling CeresASM checkout to run against (see findCeresDirectory()'s own note; every TEST()
	// below treats that sentinel as "skip, not fail").
	std::string compileAssembleAndRun(std::string_view name, std::string_view source)
	{
		std::optional<fs::path> ceresDir = findCeresDirectory();
		if (!ceresDir)
			return "<no ceres binary found>";

		fs::path workDir = fs::temp_directory_path() / "ceresc_e2e";
		fs::create_directories(workDir);
		fs::path cPath = workDir / std::format("{}.c", name);
		fs::path casmPath = workDir / std::format("{}.casm", name);
		fs::path cresPath = workDir / std::format("{}.cres", name);
		fs::path outputPath = workDir / std::format("{}.stdout.txt", name);

		{
			std::ofstream out(cPath, std::ios::binary);
			out << source;
		}

		ceresc::driver::Options options;
		options.inputPath = cPath.string();
		options.outputPath = casmPath.string();
		options.run = false; // this suite drives asm/run itself, to capture stdout - see the header comment
		int compileResult = ceresc::driver::run(options);
		CHECK_EQ(compileResult, 0);
		if (compileResult != 0)
			return "<ceresc compile failed>";

		fs::path ceresBinary = *ceresDir / kCeresExecutableName;
		std::string asmCommand = std::format("{} asm {} -o {}", quote(ceresBinary), quote(casmPath), quote(cresPath));
		int asmResult = runSubprocessCapturingStdout(asmCommand, outputPath);
		CHECK_EQ(asmResult, 0);
		if (asmResult != 0)
			return std::format("<ceres asm failed: {}>", readFile(outputPath));

		std::string runCommand = std::format("{} run {}", quote(ceresBinary), quote(cresPath));
		int runResult = runSubprocessCapturingStdout(runCommand, outputPath);
		CHECK_EQ(runResult, 0);
		return readFile(outputPath);
	}
}

TEST(e2e, return_constant_assembles_and_runs_without_faulting)
{
	// The very first program to go through the whole pipeline for real (§13's Fase 6 milestone) -
	// no printable result to check (see the header comment on why `main`'s return value has no
	// exit-code channel), just that it assembles and the VM halts cleanly instead of faulting.
	std::string output = compileAssembleAndRun("return_constant", "int main() { return 42; }");
	if (output == "<no ceres binary found>")
	{
		std::printf("  (skipped: no sibling CeresASM checkout found - set CERESC_CERES_PATH)\n");
		return;
	}
	CHECK_EQ(output, std::string());
}

TEST(e2e, function_call_and_arithmetic_produce_the_right_value)
{
	std::string output = compileAssembleAndRun("add",
		"int add(int a, int b) { return a + b; }"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    *term = 48 + add(3, 4);"
		"    return 0;"
		"}");
	if (output == "<no ceres binary found>")
	{
		std::printf("  (skipped: no sibling CeresASM checkout found - set CERESC_CERES_PATH)\n");
		return;
	}
	CHECK_EQ(output, std::string("7"));
}

TEST(e2e, recursive_factorial_produces_the_right_value)
{
	// The architecture plan's own Fase 6 DoD: "un factorial(n) recursivo ... devuelve el valor
	// correcto al ejecutarse con ceres run real."
	std::string output = compileAssembleAndRun("factorial",
		"int factorial(int n) {"
		"    if (n <= 1) return 1;"
		"    return n * factorial(n - 1);"
		"}"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    *term = 48 + factorial(3);" // single digit (6) keeps the raw-MMIO print simple
		"    return 0;"
		"}");
	if (output == "<no ceres binary found>")
	{
		std::printf("  (skipped: no sibling CeresASM checkout found - set CERESC_CERES_PATH)\n");
		return;
	}
	CHECK_EQ(output, std::string("6"));
}

TEST(e2e, global_variables_persist_across_calls)
{
	std::string output = compileAssembleAndRun("global_counter",
		"int counter = 3;"
		"void bump() { counter = counter + 1; }"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    bump();"
		"    bump();"
		"    *term = 48 + counter;" // 3 + 1 + 1 = 5
		"    return 0;"
		"}");
	if (output == "<no ceres binary found>")
	{
		std::printf("  (skipped: no sibling CeresASM checkout found - set CERESC_CERES_PATH)\n");
		return;
	}
	CHECK_EQ(output, std::string("5"));
}

TEST(e2e, a_for_loop_produces_the_right_value)
{
	std::string output = compileAssembleAndRun("for_loop",
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    int product = 1;"
		"    for (int i = 1; i <= 3; i = i + 1) { product = product * i; }" // 1*2*3 = 6
		"    *term = 48 + product;"
		"    return 0;"
		"}");
	if (output == "<no ceres binary found>")
	{
		std::printf("  (skipped: no sibling CeresASM checkout found - set CERESC_CERES_PATH)\n");
		return;
	}
	CHECK_EQ(output, std::string("6"));
}

TEST(e2e, float_arithmetic_and_conversion_produce_the_right_value)
{
	std::string output = compileAssembleAndRun("float_math",
		"float halve(float x) { return x / 2.0; }"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    float r = halve(16.0);"
		"    *term = 48 + (int)r;"
		"    return 0;"
		"}");
	if (output == "<no ceres binary found>")
	{
		std::printf("  (skipped: no sibling CeresASM checkout found - set CERESC_CERES_PATH)\n");
		return;
	}
	CHECK_EQ(output, std::string("8"));
}
