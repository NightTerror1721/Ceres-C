#include <ceresc/driver/driver.h>
#include <ceresc/driver/options.h>
#include <ceresc/support/optimization.h>

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
// EVERY program below runs at -O0, -O1 AND -O2, and all three must print the same thing. That is
// the real test of the optimizations: a golden test proves the output changed, this proves it still
// means the same. It is also what makes -O0 worth keeping (support/optimization.h) - a program that
// disagrees between levels is bisected by turning individual -f switches off until it agrees again.
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

	constexpr std::string_view kNoCeres = "<no ceres binary found>";

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

	// Compiles `source` (a whole .c program) at `level`, assembles it and runs it for real,
	// returning what `ceres run` printed to stdout - or kNoCeres if this environment has no sibling
	// CeresASM checkout to run against (see findCeresDirectory()'s own note).
	std::string compileAssembleAndRun(std::string_view name, std::string_view source,
		ceresc::support::OptimizationLevel level)
	{
		std::optional<fs::path> ceresDir = findCeresDirectory();
		if (!ceresDir)
			return std::string(kNoCeres);

		// One directory per level, so the three builds of a program never overwrite each other's
		// intermediates (and so a failure leaves all three on disk to compare).
		int levelNumber = static_cast<int>(level);
		fs::path workDir = fs::temp_directory_path() / "ceresc_e2e" / std::format("O{}", levelNumber);
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
		options.optimization = ceresc::support::OptimizationOptions::forLevel(level);
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

	// Runs one program at all three optimization levels and checks each printed `expected`. It prints
	// a skip note when there is no `ceres` to run against.
	void runsTheSameAtEveryLevel(std::string_view name, std::string_view source, std::string_view expected)
	{
		using ceresc::support::OptimizationLevel;
		for (OptimizationLevel level : { OptimizationLevel::O0, OptimizationLevel::O1, OptimizationLevel::O2 })
		{
			std::string output = compileAssembleAndRun(name, source, level);
			if (output == kNoCeres)
			{
				std::printf("  (skipped: no sibling CeresASM checkout found - set CERESC_CERES_PATH)\n");
				return;
			}
			// The level is in the message as an "O{n}:" prefix, so a failure says which one disagreed.
			CHECK_EQ(std::format("O{}:{}", static_cast<int>(level), output),
				std::format("O{}:{}", static_cast<int>(level), expected));
		}
	}
}

TEST(e2e, return_constant_assembles_and_runs_without_faulting)
{
	// The very first program to go through the whole pipeline for real (§13's Fase 6 milestone) -
	// no printable result to check (see the header comment on why `main`'s return value has no
	// exit-code channel), just that it assembles and the VM halts cleanly instead of faulting.
	runsTheSameAtEveryLevel("return_constant", "int main() { return 42; }", "");
}

TEST(e2e, function_call_and_arithmetic_produce_the_right_value)
{
	runsTheSameAtEveryLevel("add",
		"int add(int a, int b) { return a + b; }"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    *term = 48 + add(3, 4);"
		"    return 0;"
		"}",
		"7");
}

TEST(e2e, recursive_factorial_produces_the_right_value)
{
	// The architecture plan's own Fase 6 DoD: "un factorial(n) recursivo ... devuelve el valor
	// correcto al ejecutarse con ceres run real."
	runsTheSameAtEveryLevel("factorial",
		"int factorial(int n) {"
		"    if (n <= 1) return 1;"
		"    return n * factorial(n - 1);"
		"}"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    *term = 48 + factorial(3);" // single digit (6) keeps the raw-MMIO print simple
		"    return 0;"
		"}",
		"6");
}

TEST(e2e, global_variables_persist_across_calls)
{
	runsTheSameAtEveryLevel("global_counter",
		"int counter = 3;"
		"void bump() { counter = counter + 1; }"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    bump();"
		"    bump();"
		"    *term = 48 + counter;" // 3 + 1 + 1 = 5
		"    return 0;"
		"}",
		"5");
}

TEST(e2e, a_for_loop_produces_the_right_value)
{
	runsTheSameAtEveryLevel("for_loop",
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    int product = 1;"
		"    for (int i = 1; i <= 3; i = i + 1) { product = product * i; }" // 1*2*3 = 6
		"    *term = 48 + product;"
		"    return 0;"
		"}",
		"6");
}

TEST(e2e, float_arithmetic_and_conversion_produce_the_right_value)
{
	runsTheSameAtEveryLevel("float_math",
		"float halve(float x) { return x / 2.0; }"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    float r = halve(16.0);"
		"    *term = 48 + (int)r;"
		"    return 0;"
		"}",
		"8");
}

// ---- the cases the optimizations could plausibly get wrong -------------------------------------
//
// Everything above would pass with a back end that ignored aliasing entirely. These are the
// fixtures that would not: each one is a place where escape analysis, load forwarding, dead-store
// elimination or the register pool has a real chance to produce a different answer than -O0 does.

TEST(e2e, writing_through_a_pointer_to_a_local_is_visible_through_the_local)
{
	// The basic aliasing case. At -O2 the optimizer is actually allowed to resolve this one
	// completely (the pointer never escapes, so it can see through it and fold the whole thing to a
	// constant) - which is exactly why the answer has to be checked against the -O0 run rather than
	// against the generated code.
	runsTheSameAtEveryLevel("alias_local",
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    int x = 3;"
		"    int* p = &x;"
		"    *p = *p + 2;"
		"    *term = 48 + x;" // 5
		"    return 0;"
		"}",
		"5");
}

TEST(e2e, a_locals_address_passed_to_another_function_still_sees_the_callees_writes)
{
	// The address leaves the function, so nothing may be forwarded, dead-stored or kept in a
	// register across the call - the case frameless-leaf's safety condition is really about.
	runsTheSameAtEveryLevel("escaping_local",
		"void addTo(int* p, int n) { for (int i = 0; i < n; i = i + 1) { *p = *p + 1; } }"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    int total = 2;"
		"    addTo(&total, 3);"
		"    *term = 48 + total;" // 5
		"    return 0;"
		"}",
		"5");
}

TEST(e2e, a_pointer_chosen_at_runtime_writes_to_the_variable_it_actually_points_at)
{
	// Which local `p` names is not decidable at compile time, so neither is which one the store
	// writes. A forwarding pass that guessed would produce a different digit here.
	runsTheSameAtEveryLevel("runtime_alias",
		"int seed = 0;"
		"int pick(int n) { return n; }"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    int a = 1;"
		"    int b = 2;"
		"    int* p = &a;"
		"    if (pick(seed)) { p = &b; }"
		"    *p = *p + 3;"
		"    *term = 48 + a + b;" // a becomes 4, b stays 2
		"    return 0;"
		"}",
		"6");
}

TEST(e2e, more_simultaneously_live_values_than_there_are_registers)
{
	// Ten locals alive at once, against a pool of three general registers in a function that calls
	// something (24-Calling-Convention.md). Whatever cannot stay in a register has to spill to a
	// frame slot and come back intact - this is what catches a slot being shared by two values whose
	// live ranges really do overlap.
	runsTheSameAtEveryLevel("register_pressure",
		"int id(int n) { return n; }"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    int a = 1; int b = 2; int c = 3; int d = 4; int e = 5;"
		"    int f = 6; int g = 7; int h = 8; int i = 9; int j = 10;"
		"    int spill = id(a + b);"
		"    int total = a + b + c + d + e + f + g + h + i + j + spill;" // 55 + 3 = 58
		"    *term = 48 + (total - 53);"                                 // 5
		"    return 0;"
		"}",
		"5");
}

TEST(e2e, a_local_reused_from_a_previous_scopes_slot_does_not_inherit_its_value)
{
	// Slot reuse between disjoint scopes is only correct if the second local is initialized before
	// it is read. Reading a stale value from the shared slot would print the first scope's digit.
	runsTheSameAtEveryLevel("slot_reuse",
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    int result = 0;"
		"    { int a = 9; result = result + a; }"
		"    { int b = 1; result = result - b; }" // 9 - 1 = 8
		"    *term = 48 + result;"
		"    return 0;"
		"}",
		"8");
}

TEST(e2e, a_function_that_makes_a_call_but_needs_no_frame_still_returns_correctly)
{
	// `outer` holds nothing across its call and passes no stack arguments, so frameless-leaf drops
	// its frame even though it is not a leaf - "leaf" is shorthand, the real condition is "needs
	// nothing from a frame". That is only safe because `call`/`ret` put the return address on the
	// hardware stack rather than in the frame, which is the kind of assumption worth executing.
	runsTheSameAtEveryLevel("frameless_caller",
		"int counter = 3;"
		"void inner() { counter = counter + 1; }"
		"void outer() { inner(); }"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    outer();"
		"    outer();"
		"    *term = 48 + counter;" // 3 + 1 + 1 = 5
		"    return 0;"
		"}",
		"5");
}

TEST(e2e, a_byte_written_over_part_of_an_int_local_is_visible_when_the_int_is_read_back)
{
	// A regression. `&x` never leaves the function, so the local looks perfectly non-escaping - but
	// the byte store writes part of it with a value the forwarding pass cannot name, so the int it
	// remembered storing is no longer what a read of `x` produces. -O2 used to print 5 here and -O0
	// printed 1; this fixture is the reason the two levels are run against each other at all.
	runsTheSameAtEveryLevel("narrow_store",
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    int x = 5;"
		"    char* c = (char*)&x;"
		"    *c = 1;"       // overwrites the low byte: x becomes 1 on this little-endian machine
		"    *term = 48 + x;"
		"    return 0;"
		"}",
		"1");
}

TEST(e2e, an_array_written_through_an_index_reads_back_element_by_element)
{
	// Frame slots wide enough to hold a whole object, and an address that really is computed rather
	// than register-resident - the one thing no amount of escape analysis can promote.
	runsTheSameAtEveryLevel("array_sum",
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    int values[4];"
		"    for (int i = 0; i < 4; i = i + 1) { values[i] = i; }"
		"    int total = 0;"
		"    for (int i = 0; i < 4; i = i + 1) { total = total + values[i]; }" // 0+1+2+3 = 6
		"    *term = 48 + total;"
		"    return 0;"
		"}",
		"6");
}

TEST(e2e, every_binary_operator_works_with_a_constant_right_hand_side)
{
	// The immediates peephole hands a constant straight to the instruction for EVERY integer
	// operation, trusting the assembler to have an I-form for each (05-Instruction-Set.md). That is
	// a claim about another project's encoder, so it gets executed rather than read: `n` comes back
	// from a call so it is never a compile-time constant, and only the right-hand sides fold.
	runsTheSameAtEveryLevel("immediate_operators",
		"int seed = 9;"
		"int opaque(int v) { return v; }"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    int n = opaque(seed);"
		"    int r = (n + 2) + (n - 2) + (n * 3) + (n / 3) + (n % 7)"
		"          + (n & 6) + (n | 4) + (n ^ 5) + (n << 1) + (n >> 1);"
		// 11 + 7 + 27 + 3 + 2 + 0 + 13 + 12 + 18 + 4 = 97
		"    *term = 48 + (r - 92);" // 5
		"    return 0;"
		"}",
		"5");
}

TEST(e2e, a_short_circuit_condition_does_not_evaluate_its_right_hand_side)
{
	// Branch simplification and jump threading both rewrite exactly the block structure && and ||
	// lower to. If either got it wrong, `sideEffect()` would run when it must not.
	runsTheSameAtEveryLevel("short_circuit",
		"int calls = 0;"
		"int sideEffect() { calls = calls + 1; return 1; }"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    int guard = 0;"
		"    if (guard && sideEffect()) { calls = calls + 10; }"
		"    if (guard || sideEffect()) { calls = calls + 4; }" // calls: 0 -> 1 -> 5
		"    *term = 48 + calls;"
		"    return 0;"
		"}",
		"5");
}
