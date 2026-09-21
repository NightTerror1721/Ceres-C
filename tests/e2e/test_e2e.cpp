#include <ceresc/driver/driver.h>
#include <ceresc/driver/options.h>
#include <ceresc/support/optimization.h>

#include "ceres_tool.h"
#include "framework.h"

#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

// The end-to-end suite (§12 of the architecture plan): compile a real .c file, invoke `ceres asm`
// and `ceres run` as real subprocesses (no mocks - libs/driver is the only library allowed to
// spawn one, and it does exactly this same thing for `ceresc --run`), and check the ACTUAL
// computed value, not just a successful exit.
//
// `ceres run` exits with the status `main` returned (the system control device carries it, see
// CeresASM's 07-IO-Devices-and-Ports.md), and with 1 on a VM fault. Most fixtures here return 0 and
// poke CeresASM's TerminalDevice MMIO register directly (a raw pointer cast + dereferenced store -
// this project has no printf yet), and this suite reads the resulting byte back from `ceres run`'s
// own captured stdout. A fixture that returns something else says so through `expectedStatus`.
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

	// Finding `ceres`, launching it with its stdout captured and reading a file back are shared
	// with tests/examples, so they live in tests/framework/ceres_tool.h rather than twice here.
	using namespace ceresc::testing;

	// Compiles `source` (a whole .c program) at `level`, assembles it and runs it for real,
	// returning what `ceres run` printed to stdout - or kNoCeres if this environment has no sibling
	// CeresASM checkout to run against (see findCeresDirectory()'s own note).
	std::string compileAssembleAndRun(std::string_view name, std::string_view source,
		ceresc::support::OptimizationLevel level, int expectedStatus = 0)
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
		options.inputPaths.push_back(cPath.string());
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
		CHECK_EQ(runResult, expectedStatus);
		return readFile(outputPath);
	}

	// Runs one program at all three optimization levels and checks each printed `expected`. It prints
	// a skip note when there is no `ceres` to run against.
	void runsTheSameAtEveryLevel(std::string_view name, std::string_view source, std::string_view expected,
		int expectedStatus = 0)
	{
		using ceresc::support::OptimizationLevel;
		for (OptimizationLevel level : { OptimizationLevel::O0, OptimizationLevel::O1, OptimizationLevel::O2 })
		{
			std::string output = compileAssembleAndRun(name, source, level, expectedStatus);
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
	// nothing printed, just that it assembles and the VM halts cleanly instead of faulting. `main`'s
	// return value is the process's exit status, so 42 is what `ceres run` exits with.
	runsTheSameAtEveryLevel("return_constant", "int main() { return 42; }", "", 42);
}

TEST(e2e, null_is_a_valid_function_pointer_to_assign_compare_and_put_in_a_table)
{
	runsTheSameAtEveryLevel("null_function_pointer",
		"static int hits;"
		"static void bump(void) { hits = hits + 1; }"
		"static void (*table[3])(void) = { bump, ((void*)0), bump };"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    void (*h)(void) = ((void*)0);"
		"    int first = h == 0;"
		"    h = bump;"
		"    h();"
		"    int i;"
		"    for (i = 0; i < 3; i++) {"
		"        void (*f)(void) = table[i];"
		"        if (f != 0) f();"
		"    }"
		"    *term = 48 + first;"
		"    *term = 48 + hits;"
		"    h = i ? h : ((void*)0);"
		"    *term = 48 + (h == bump);"
		"    h = 0 ? h : ((void*)0);"
		"    *term = 48 + (h == 0);"
		"    return 0;"
		"}",
		"1311");
}

TEST(e2e, a_function_whose_frame_outgrows_a_16_bit_displacement_still_assembles_and_runs)
{
	// At -O0 every local and every temporary has its own slot, so a function with thousands of
	// locals has a frame of well over 64 KiB: past what a load or store displacement reaches, and
	// past what `enter Frame` can take as an immediate. The types are mixed on purpose - bytes and
	// halfwords sit at odd offsets, and the compiler's own idea of where a slot is (used for the far
	// ones) has to agree with the assembler's (used for the near ones), or two slots would overlap
	// and the chain below would break.
	const char* types[] = { "int", "char", "short", "int", "unsigned char" };
	std::string body = "int main() { char* term = (char*)0xFF000004; int v0 = 1;";
	int value = 1;
	int middle = 0;
	const int count = 7000;
	for (int i = 1; i <= count; ++i)
	{
		body += std::format("{} v{} = (v{} + 1) % 50;\n", types[i % 5], i, i - 1);
		value = (value + 1) % 50;
		if (i == count / 2)
			middle = value;
	}
	body += std::format("*term = 48 + v{} / 10; *term = 48 + v{} % 10; *term = 48 + v{} / 10; *term = 48 + v{} % 10; return 0; }}",
		count / 2, count / 2, count, count);
	runsTheSameAtEveryLevel("huge_frame", body,
		std::format("{}{}{}{}", middle / 10, middle % 10, value / 10, value % 10));
}

TEST(e2e, arithmetic_on_constants_fills_a_static_initializer)
{
	// 6, then table[0] = 2, table[1] = 8, table[2] + 5 = 3, the count of the table = 3, the top two
	// bits of the wrapped unsigned = 3, the float = 3, and the struct's two fields, 3 and 6.
	runsTheSameAtEveryLevel("constant_initializers",
		"enum { A = 2, B = 3 };"
		"struct P { int x; int y; };"
		"static int product = A * B;"
		"static const int table[] = { 1 + 1, A << 2, B - 5 };"
		"static int count = sizeof(table) / sizeof(table[0]);"
		"static unsigned int big = 0xFFFFFFFFu - 1;"
		"static float three = 1 * 3;"
		"static struct P p = { A + 1, B * 2 };"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    *term = 48 + product;"
		"    *term = 48 + table[0];"
		"    *term = 48 + table[1];"
		"    *term = 48 + table[2] + 5;"
		"    *term = 48 + count;"
		"    *term = 48 + (big >> 30);"
		"    *term = 48 + (int)three;"
		"    *term = 48 + p.x;"
		"    *term = 48 + p.y;"
		"    return 0;"
		"}",
		"628333336");
}

TEST(e2e, the_comma_operator_runs_its_left_side_first_and_yields_its_right_side)
{
	runsTheSameAtEveryLevel("comma_operator",
		"int calls;"
		"static int next(void) { calls = calls + 1; return calls; }"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    int a; int b; int i; int j; int sum;"
		"    a = (b = 4, b + 3);"
		"    *term = 48 + a;"
		"    *term = 48 + b;"
		"    a = (next(), next(), next());"
		"    *term = 48 + a;"
		"    *term = 48 + calls;"
		"    sum = 0;"
		"    for (i = 0, j = 5; i < j; i++, j--) sum = sum + j - i;"
		"    *term = 48 + sum;"
		"    *term = 48 + i;"
		"    *term = 48 + j;"
		"    i = 0;"
		"    while (i = i + 1, i < 4) { }"
		"    *term = 48 + i;"
		"    return (a = 1, 0);"
		"}",
		"74339324");
}

TEST(e2e, the_value_main_returns_is_the_exit_status_of_the_run)
{
	// Through every optimization level, and past the bits a byte holds: only the low eight survive,
	// as with a POSIX exit status. Falling off the end of `main` or returning nothing is status 0.
	runsTheSameAtEveryLevel("status_seven", "int main() { return 7; }", "", 7);
	runsTheSameAtEveryLevel("status_from_a_call",
		"static int compute(int a, int b) { return a * b + 1; }"
		"int main() { return compute(6, 8); }", "", 49);
	runsTheSameAtEveryLevel("status_masked", "int main() { return 0x1FF; }", "", 255);
	runsTheSameAtEveryLevel("status_zero_falling_off", "int main() { }", "", 0);
	runsTheSameAtEveryLevel("status_void_main", "void main() { }", "", 0);
}

TEST(e2e, main_hands_its_status_to_exit_when_the_unit_declares_it)
{
	// A unit that declares `void exit(int)` is a hosted one: returning from `main` is `exit(main())`.
	// Here `exit` is a stand-in that prints a byte and shuts the machine down with the status it
	// was given, so both the call and the status it carries are checked.
	runsTheSameAtEveryLevel("main_calls_exit",
		"void exit(int status);"
		"void exit(int status) {"
		"    char* term = (char*)0xFF000004;"
		"    *term = 48 + status;"
		"    unsigned int* control = (unsigned int*)0xFFFF0000;"
		"    *control = (status << 8) | 1;"
		"}"
		"int main() { return 5; }",
		"5", 5);
	runsTheSameAtEveryLevel("main_calls_exit_with_zero_when_it_returns_nothing",
		"void exit(int status);"
		"void exit(int status) {"
		"    char* term = (char*)0xFF000004;"
		"    *term = 48 + status;"
		"    unsigned int* control = (unsigned int*)0xFFFF0000;"
		"    *control = (status << 8) | 1;"
		"}"
		"void main() { }",
		"0", 0);
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

TEST(e2e, an_extern_array_of_unknown_size_is_completed_by_its_definition_in_the_same_unit)
{
	// 16 bytes once the definition has given it four elements, and the last of them is 4
	runsTheSameAtEveryLevel("extern_array_completed",
		"extern int t[];\n"
		"int last(void) { return t[3]; }\n"
		"int t[4] = { 1, 2, 3, 4 };\n"
		"int main() { return last() + (int)sizeof(t); }\n",
		"", 20);
}

TEST(e2e, static_assertions_and_func_compile_away_and_name_the_function)
{
	// 'w' + 'm' + 'n' = 119 + 109 + 110 = 338, which the exit status keeps as 82
	runsTheSameAtEveryLevel("static_assert_func",
		"enum { LIMIT = 8 };\n"
		"struct Header { unsigned char kind; unsigned char pad[3]; unsigned int length; _Static_assert(sizeof(int) == 4, \"a word\"); };\n"
		"_Static_assert(sizeof(struct Header) == 8, \"the header is two words\");\n"
		"_Static_assert(LIMIT * 2 == 16);\n"
		"static const char* who(void) { return __func__; }\n"
		"int main(void)\n"
		"{\n"
		"    _Static_assert(sizeof(char) == 1, \"a char is a byte\");\n"
		"    const char* a = who();\n"
		"    const char* b = __FUNCTION__;\n"
		"    return a[0] + b[0] + b[3];\n"
		"}\n",
		"", 82);
}

TEST(e2e, names_that_are_reserved_words_of_the_assembler_work_at_every_level)
{
	// at(1) + half + word + interrupt(2, 3) + r5() + global + s.at + s.half + assert(1)
	//   = 2 + 20 + 3 + 6 + 2 + 4 + 1 + 2 + 1
	runsTheSameAtEveryLevel("reserved_names",
		"int at(int x) { return x + 1; }\n"
		"int half = 20;\n"
		"static int word = 3;\n"
		"int interrupt(int a, int b) { return a * b; }\n"
		"int r5(void) { return 2; }\n"
		"struct S { int at; int half; };\n"
		"int assert(int c) { return c; }\n"
		"int main(void)\n"
		"{\n"
		"    static int global = 4;\n"
		"    struct S s;\n"
		"    s.at = 1; s.half = 2;\n"
		"    return at(1) + half + word + interrupt(2, 3) + r5() + global + s.at + s.half + assert(1);\n"
		"}\n",
		"", 41);
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

TEST(e2e, a_static_string_pointer_holds_the_literals_address)
{
	// `char* msg = "hi";` at file scope: msg is a pointer whose initializer is the literal's
	// address, which is not known until the link. Reading through it must reach the bytes.
	runsTheSameAtEveryLevel("static_string_pointer",
		"char* msg = \"hi\";"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    *term = msg[0];"
		"    *term = msg[1];"
		"    return 0;"
		"}",
		"hi");
}

TEST(e2e, an_array_of_static_string_pointers_holds_each_literals_address)
{
	runsTheSameAtEveryLevel("static_string_pointer_array",
		"char* names[2] = { \"a\", \"b\" };"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    *term = names[0][0];"
		"    *term = names[1][0];"
		"    return 0;"
		"}",
		"ab");
}

TEST(e2e, a_struct_pointer_field_holds_the_literals_address)
{
	runsTheSameAtEveryLevel("struct_string_pointer",
		"struct S { char* s; };"
		"struct S s = { \"hi\" };"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    *term = s.s[0];"
		"    *term = s.s[1];"
		"    return 0;"
		"}",
		"hi");
}

TEST(e2e, a_static_pointer_to_a_global_holds_its_address)
{
	runsTheSameAtEveryLevel("static_pointer_to_global",
		"int g = 5;"
		"int* p = &g;"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    *term = 48 + *p;"
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

TEST(e2e, a_short_circuit_boolean_used_as_a_value_is_right_at_every_level)
{
	// `&&`, `||`, `!` and `?:` as VALUES - not conditions - each lower to one temporary defined in
	// two branch blocks and read in the join (materializeBoolean / visit(TernaryExpr&)). At -O0
	// that value round-trips through a frame slot; at -O1/-O2 the cross-block register pass keeps
	// it in a register. The three levels have to agree, which is what catches a miscompile here.
	runsTheSameAtEveryLevel("boolean_values",
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    int a = 1; int b = 0;"
		"    *term = 48 + (a && b) + (a || b) + (!b) + (a ? 2 : 0);" // 0 + 1 + 1 + 2 = 4
		"    return 0;"
		"}",
		"4");
}

// ---- composite memory: arrays, pointers and structs (Fase 7) -----------------------------------
//
// The phase's own deliverables and exit criterion: `suma_array`, a function that fills and reads a
// `struct Point`, and a program that sorts ten integers. Each one runs at all three optimization
// levels like everything above, which is what makes the indexed addressing modes and the struct ABI
// answerable questions rather than claims about the generated text.

TEST(e2e, suma_array_adds_up_an_array_passed_by_pointer)
{
	// The prior audit's own example, and the phase's first deliverable: the array's address crosses
	// a call boundary and the callee walks it by index.
	runsTheSameAtEveryLevel("suma_array",
		"int suma_array(int* values, int count) {"
		"    int total = 0;"
		"    for (int i = 0; i < count; i = i + 1) { total = total + values[i]; }"
		"    return total;"
		"}"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    int values[5] = { 0, 1, 1, 1, 2 };"
		"    *term = 48 + suma_array(values, 5);" // 5
		"    return 0;"
		"}",
		"5");
}

TEST(e2e, a_function_fills_a_struct_point_through_a_pointer_and_the_caller_reads_it_back)
{
	// The phase's second deliverable. `&p` escapes into `fill`, so nothing about `p` may be kept in
	// a register across the call, and the two field writes have to land at the offsets sema computed.
	runsTheSameAtEveryLevel("struct_point",
		"struct Point { int x; int y; };"
		"void fill(struct Point* p, int x, int y) { p->x = x; p->y = y; }"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    struct Point p;"
		"    fill(&p, 2, 3);"
		"    *term = 48 + p.x * 2 + p.y - 2;" // 2*2 + 3 - 2 = 5
		"    return 0;"
		"}",
		"5");
}

TEST(e2e, selection_sort_of_ten_integers_produces_the_expected_output_byte_for_byte)
{
	// The phase's exit criterion, chosen because one program exercises indexing, comparison and a
	// swap at once - and printing all ten digits checks the whole array, not just that it ran.
	runsTheSameAtEveryLevel("selection_sort",
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    int a[10] = { 5, 3, 9, 1, 7, 0, 8, 2, 6, 4 };"
		"    for (int i = 0; i < 9; i = i + 1) {"
		"        int min = i;"
		"        for (int j = i + 1; j < 10; j = j + 1) {"
		"            if (a[j] < a[min]) { min = j; }"
		"        }"
		"        int t = a[i]; a[i] = a[min]; a[min] = t;"
		"    }"
		"    for (int i = 0; i < 10; i = i + 1) { *term = 48 + a[i]; }"
		"    return 0;"
		"}",
		"0123456789");
}

TEST(e2e, a_two_dimensional_array_indexes_by_row_stride)
{
	// `m[i][j]` is two scalings, and the row's own address is a decayed sub-array rather than a
	// pointer loaded from anywhere - the case a single-dimension index rule would get wrong.
	runsTheSameAtEveryLevel("matrix",
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    int m[2][3] = { { 1, 2, 3 }, { 4, 5, 6 } };"
		"    *term = 48 + m[1][2];" // 6
		"    return 0;"
		"}",
		"6");
}

TEST(e2e, pointer_arithmetic_scales_by_the_pointee_size_in_both_directions)
{
	// `p + 2` advances eight bytes, and `q - a` divides the byte distance back down to elements -
	// the two halves of the same scaling rule, which cancel out here only if both are right.
	runsTheSameAtEveryLevel("pointer_arithmetic",
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    int a[4] = { 1, 2, 3, 4 };"
		"    int* p = a;"
		"    p = p + 2;"
		"    int* q = &a[3];"
		"    *term = 48 + *p + (q - a) - 4;" // 3 + 3 - 4 = 2
		"    return 0;"
		"}",
		"2");
}

TEST(e2e, an_array_of_structs_strides_by_the_whole_struct)
{
	runsTheSameAtEveryLevel("struct_array",
		"struct Point { int x; int y; };"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    struct Point pts[3];"
		"    for (int i = 0; i < 3; i = i + 1) { pts[i].x = i; pts[i].y = i * 2; }"
		"    *term = 48 + pts[2].x + pts[2].y - 1;" // 2 + 4 - 1 = 5
		"    *term = 48 + pts[0].y;" // 0, unless the stride overlapped pts[1]
		"    return 0;"
		"}",
		"50");
}

TEST(e2e, a_partially_initialized_aggregate_has_zeros_everywhere_the_list_did_not_reach)
{
	// C's rule, and there is no memset to call - the zeros are stores lowerInitializerInto() emits
	// itself (libs/ir). A struct's later fields count too, not just an array's tail.
	runsTheSameAtEveryLevel("partial_initializers",
		"struct Three { int a; int b; int c; };"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    int a[4] = { 9 };"
		"    struct Three t = { 4 };"
		"    *term = 48 + a[0] + a[1] + a[2] + a[3] + t.a + t.b + t.c - 8;" // 9 + 4 - 8 = 5
		"    return 0;"
		"}",
		"5");
}

TEST(e2e, a_char_array_initialized_from_a_string_literal_holds_its_own_copy_of_the_bytes)
{
	// Not a pointer into .rodata: the array owns the bytes, so writing one is allowed and the
	// terminating zero is really there.
	runsTheSameAtEveryLevel("char_array_init",
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    char s[6] = \"hi\";"
		"    s[2] = 33;"  // '!' - overwrites the terminator the literal put at index 2
		"    s[3] = 0;"
		"    for (int i = 0; s[i] != 0; i = i + 1) { *term = s[i]; }"
		"    return 0;"
		"}",
		"hi!");
}

TEST(e2e, aggregate_globals_live_in_data_and_bss_and_keep_their_values)
{
	// One of each shape generateAggregateGlobal() can emit: an initialized scalar array, a string
	// one, an initialized struct's word image, and an uninitialized struct in @bss.
	runsTheSameAtEveryLevel("aggregate_globals",
		"int primes[4] = { 2, 3, 5, 7 };"
		"char name[4] = \"ok\";"
		"int grid[2][3] = { { 1, 2, 3 }, { 4, 5, 6 } };"
		"struct Point { int x; int y; };"
		"struct Point start = { 1, 2 };"
		"struct Point cursor;"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    cursor.x = 4;"
		"    for (int i = 0; name[i] != 0; i = i + 1) { *term = name[i]; }"
		"    *term = 48 + primes[0] + primes[3] + start.x + start.y + cursor.x + cursor.y + grid[1][0] - 15;" // 2+7+1+2+4+0+4-15 = 5
		"    return 0;"
		"}",
		"ok5");
}

TEST(e2e, a_struct_wider_than_a_word_survives_a_round_trip_through_a_call_by_value)
{
	// The hidden-destination-pointer return and the caller-made-copy argument, both at once: `build`
	// returns twenty bytes and `total` takes them back by value. If either end disagreed about the
	// ABI the digit would be wrong rather than the program crashing, which is why the value matters.
	runsTheSameAtEveryLevel("struct_by_value",
		"struct Big { int a; int b; int c; int d; int e; };"
		"struct Big build(int n) {"
		"    struct Big g;"
		"    g.a = n; g.b = n + 1; g.c = n + 2; g.d = n + 3; g.e = n + 4;"
		"    return g;"
		"}"
		"int total(struct Big g) { return g.a + g.b + g.c + g.d + g.e; }"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    struct Big g = build(0);"
		"    *term = 48 + total(g) - 5;" // 0+1+2+3+4 = 10, minus 5
		"    return 0;"
		"}",
		"5");
}

TEST(e2e, a_by_value_struct_argument_is_a_copy_the_callee_cannot_write_back_through)
{
	// The whole point of passing the address of a COPY rather than of the caller's object: the
	// callee's writes must not be visible afterwards.
	runsTheSameAtEveryLevel("struct_by_value_is_a_copy",
		"struct Pair { int a; int b; };"
		"int clobber(struct Pair p) { p.a = 99; p.b = 99; return p.a - p.b; }"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    struct Pair p;"
		"    p.a = 5; p.b = 0;"
		"    clobber(p);"
		"    *term = 48 + p.a + p.b;" // still 5, not 198
		"    return 0;"
		"}",
		"5");
}

TEST(e2e, a_struct_that_fits_one_word_comes_back_in_the_return_register)
{
	// The other side of the size rule: 1/2/4 bytes travel in ret0 with no hidden parameter at all.
	runsTheSameAtEveryLevel("small_struct_return",
		"struct One { int x; };"
		"struct One make(int n) { struct One s; s.x = n; return s; }"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    struct One s = make(5);"
		"    *term = 48 + s.x;"
		"    return 0;"
		"}",
		"5");
}

TEST(e2e, a_three_byte_struct_round_trips_without_touching_a_fourth_byte)
{
	// Three bytes is on the indirect side of the rule precisely so no word store writes a byte the
	// object does not own - the guard byte after it proves nothing did.
	runsTheSameAtEveryLevel("three_byte_struct",
		"struct Three { char a; char b; char c; };"
		"struct Three make() { struct Three t; t.a = 1; t.b = 2; t.c = 2; return t; }"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    struct Pack { struct Three t; char guard; };"
		"    struct Pack p;"
		"    p.guard = 7;"
		"    p.t = make();"
		"    *term = 48 + p.t.a + p.t.b + p.t.c;" // 1 + 2 + 2 = 5
		"    *term = 48 + p.guard - 2;"           // 7 - 2 = 5, unless the copy overran into it
		"    return 0;"
		"}",
		"55");
}

TEST(e2e, a_whole_struct_assignment_copies_every_field)
{
	runsTheSameAtEveryLevel("struct_assignment",
		"struct Pair { int a; int b; };"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    struct Pair x; struct Pair y;"
		"    x.a = 2; x.b = 3;"
		"    y.a = 0; y.b = 0;"
		"    y = x;"
		"    *term = 48 + y.a + y.b;" // 5
		"    return 0;"
		"}",
		"5");
}

TEST(e2e, a_nested_struct_field_is_reached_through_two_constant_offsets)
{
	runsTheSameAtEveryLevel("nested_struct",
		"struct Inner { int a; int b; };"
		"struct Outer { struct Inner in; int c; };"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    struct Outer o = { { 1, 2 }, 2 };"
		"    *term = 48 + o.in.a + o.in.b + o.c;" // 1 + 2 + 2 = 5
		"    return 0;"
		"}",
		"5");
}

// ---- integer conversions: width and signedness -------------------------------------------------
//
// These three were pinned as known failures while building examples/ for Fase 8 (§13) and fixed
// afterwards; they stay as ordinary tests because each one is a rule the back end can silently stop
// honouring. All three run at every optimization level like everything above, which is the point:
// the first of them used to give DIFFERENT answers at -O0 and -O1, since the bug only appeared when
// a value round-tripped through memory.

TEST(e2e, a_negative_signed_char_read_back_from_memory_keeps_its_sign)
{
	// §10's IR->CASM table says every load is unsigned "en v1", which was consistent with §14's
	// original "char/short siempre unsigned" decision - but that decision was superseded on
	// 2026-09-14 and signed char/short are in scope now, while the table was never revisited. The
	// two levels disagree, which is the part that makes it a bug rather than a documented limit:
	// at -O0 the value round-trips through a one-byte slot and comes back zero-extended (156),
	// at -O1/-O2 it stays in a register and keeps its sign (-100).
	//
	// The result is checked through a COMPARISON rather than by printing the value: the terminal
	// register is one byte wide, so `*term = 48 - small` would print the same character whether
	// `small` came back as -3 or as 253 - the store truncates the difference away. Asking whether
	// it is negative does not.
	runsTheSameAtEveryLevel("signed_char_roundtrip",
		"int opaque(int v) { return v; }"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    signed char small = (signed char)opaque(-3);"
		"    *term = 48 + (small < 0);" // '1' if it is really -3, '0' if it came back as 253
		"    return 0;"
		"}",
		"1");
}

TEST(e2e, a_cast_to_a_narrower_integer_type_truncates)
{
	// Unlike the one above, this is wrong the same way at all three levels: the conversion is
	// dropped entirely rather than lowered to a truncation.
	runsTheSameAtEveryLevel("narrowing_cast",
		"int opaque(int v) { return v; }"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    int wide = opaque(0x101);"                 // 257: its low byte is 1
		"    *term = 48 + ((int)(char)wide == 1);"      // compared, not printed - see the note above
		"    return 0;"
		"}",
		"1");
}

TEST(e2e, converting_an_int_to_bool_normalizes_to_zero_or_one)
{
	runsTheSameAtEveryLevel("bool_normalization",
		"int opaque(int v) { return v; }"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    bool flag = opaque(42);"
		"    *term = 48 + flag;" // '1', since any non-zero value converts to true
		"    return 0;"
		"}",
		"1");
}

TEST(e2e, an_argument_is_converted_to_its_parameters_type_before_the_call)
{
	// The conversion has to happen at the CALL, not inside the callee: a narrow parameter the
	// optimizer keeps in the register it arrived in is narrowed nowhere else.
	runsTheSameAtEveryLevel("argument_conversion",
		"int opaque(int v) { return v; }"
		"int widen(signed char c) { return c; }"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    *term = 48 + (widen(opaque(0x1FD)) == -3);" // 0x1FD as a signed char is -3
		"    return 0;"
		"}",
		"1");
}

TEST(e2e, an_unsigned_narrow_value_reads_back_as_a_large_positive_number)
{
	// The other half of the same rule: `unsigned char` must NOT sign-extend, so 200 stays 200
	// rather than becoming -56.
	runsTheSameAtEveryLevel("unsigned_narrow",
		"int opaque(int v) { return v; }"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    unsigned char big = (unsigned char)opaque(200);"
		"    unsigned short wide = (unsigned short)opaque(60000);"
		"    *term = 48 + (big == 200) + (wide == 60000) + (big > 0) + (wide > 0) + 1;" // 48 + 5
		"    return 0;"
		"}",
		"5");
}

TEST(e2e, a_narrow_value_survives_a_round_trip_through_a_struct_field)
{
	// Fields are narrow storage too, and their loads pick the same signed/unsigned form.
	runsTheSameAtEveryLevel("narrow_fields",
		"struct Mixed { signed char a; unsigned char b; short c; unsigned short d; };"
		"int opaque(int v) { return v; }"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    struct Mixed m;"
		"    m.a = (signed char)opaque(-1);"
		"    m.b = (unsigned char)opaque(255);"
		"    m.c = (short)opaque(-2);"
		"    m.d = (unsigned short)opaque(65535);"
		"    *term = 48 + (m.a == -1) + (m.b == 255) + (m.c == -2) + (m.d == 65535) + 1;" // 48 + 5
		"    return 0;"
		"}",
		"5");
}

TEST(e2e, a_narrow_local_kept_in_a_register_reads_back_exactly_as_a_frame_field_would)
{
	// A `char`, a `short` and a `bool` all fit in a register, so none of them needs a frame field -
	// and none of them may change meaning by not having one. The narrow parameters are the
	// interesting half: the prologue narrows each one into its register, which is what the
	// `strb`/`strh` into a field used to do on the way past.
	runsTheSameAtEveryLevel("narrow_locals_in_registers",
		"int wide_char(char c) { char x = c; return (int)x; }"
		"int wide_uchar(unsigned char c) { unsigned char x = c; return (int)x; }"
		"int wide_short(short s) { short x = s; return (int)x; }"
		"int wrapped(short s) { short x = s; x = x + 1; return (int)x; }"
		"int flag(bool b) { bool x = b; return x ? 1 : 0; }"
		"int opaque(int v) { return v; }"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    *term = 48 + (wide_char((char)opaque(200)) == -56)"
		"               + (wide_uchar((unsigned char)opaque(200)) == 200)"
		"               + (wide_short((short)opaque(-300)) == -300)"
		"               + (wrapped((short)opaque(32767)) == -32768)"
		"               + (flag(true) == 1) + (flag(false) == 0) + 1;" // 48 + 7
		"    return 0;"
		"}",
		"7");
}

TEST(e2e, a_register_parameter_computes_the_same_answer_as_an_ordinary_one)
{
	// `register` is a request about placement and nothing else, so the two spellings of the same
	// function have to agree - at every level, including -O0, where the keyword says nothing at all
	// because register allocation itself is off.
	runsTheSameAtEveryLevel("register_parameter",
		"int scale(register int a, register int b) { register int t = a * b; return t + a; }"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    *term = 48 + scale(2, 3) - 2;" // 2*3 + 2 = 8, minus 2 = 6
		"    return 0;"
		"}",
		"6");
}

TEST(e2e, a_capped_wide_type_computes_as_the_32_bit_type_it_really_is)
{
	// `long long` and `double` name widths this machine does not have, so they are the 32-bit types
	// under another spelling (docs/06-Known-Limitations.md). What that has to mean at run time is
	// that mixing the two spellings changes nothing at all.
	runsTheSameAtEveryLevel("capped_wide_types",
		// And the program says once, with a pragma, that it knows - which is what that pragma is
		// for and keeps this suite's output about what it ran rather than about what it wrote.
		"#pragma warning(disable: 2001)\n"
		"long long widen(long long v) { return v + 1; }"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    long long a = widen(3);"
		"    unsigned long long b = 2;"
		"    double d = 1.5;"
		"    long double e = 0.5;"
		"    *term = 48 + (int)(a + (long long)b) + (int)(d + e) - 4;" // 4 + 2 + 2 - 4 = 4
		"    return 0;"
		"}",
		"4");
}

TEST(e2e, adjacent_string_literals_print_as_one_string)
{
	// C joins them before the grammar sees them, which is what makes a long string writable over
	// several lines and what `__DATE__ " " __TIME__` relies on.
	runsTheSameAtEveryLevel("string_concatenation",
		"void put(char c) { char* t = (char*)0xFF000004; *t = c; }"
		"void putstr(char* s) { for (int i = 0; s[i] != 0; i++) put(s[i]); }"
		"int main() {"
		"    char joined[6] = \"ab\" \"cd\";"
		"    putstr(\"he\""
		"           \"llo\");"          /* over two lines, as a long string is written */
		"    putstr(joined);"
		"    put((char)(48 + (int)sizeof(\"a\" \"bc\")));" // 4
		"    return 0;"
		"}",
		"helloabcd4");
}

// ---- storage classes and const ------------------------------------------------------------------

TEST(e2e, a_static_local_keeps_its_value_between_calls)
{
	// The one thing `static` on a local really means: storage that outlives the call. It becomes a
	// file-scope object under a name carrying its function's, so two functions may each have one.
	runsTheSameAtEveryLevel("static_local",
		"int next() { static int counter = 0; counter = counter + 1; return counter; }"
		"int other() { static int counter = 10; counter = counter + 1; return counter; }"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    next(); next();"
		"    *term = 48 + next();"   // '3'
		"    *term = 48 + other() - 10;" // '1' - a different object with the same C name
		"    return 0;"
		"}",
		"31");
}

TEST(e2e, a_static_local_is_initialized_once_at_load_time_not_on_every_call)
{
	runsTheSameAtEveryLevel("static_local_init",
		"int tick() { static int n = 5; n = n + 1; return n; }"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    tick(); tick();"
		"    *term = 48 + tick() - 3;" // 5+3 = 8, minus 3
		"    return 0;"
		"}",
		"5");
}

TEST(e2e, a_static_function_is_callable_and_simply_not_published)
{
	runsTheSameAtEveryLevel("static_function",
		"static int hidden(int n) { return n + 2; }"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    *term = 48 + hidden(3);"
		"    return 0;"
		"}",
		"5");
}

TEST(e2e, an_inline_function_computes_the_same_answer_whether_or_not_it_was_inlined)
{
	// `inline` raises the inliner's size limit; at -O0 nothing is inlined at all. Both have to
	// agree, which is the whole point of running every level.
	runsTheSameAtEveryLevel("inline_function",
		"inline int square(int n) { return n * n; }"
		"int opaque(int v) { return v; }"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    *term = 48 + square(opaque(2)) + 1;" // 4 + 1
		"    return 0;"
		"}",
		"5");
}

TEST(e2e, a_const_global_lives_in_rodata_and_is_still_readable)
{
	// @rodata is where the machine itself enforces the qualifier, so the interesting part is that
	// an ordinary read of it still works from there.
	runsTheSameAtEveryLevel("const_global",
		"const int limit = 5;"
		"const char greeting[3] = \"ok\";"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    for (int i = 0; greeting[i] != 0; i = i + 1) { *term = greeting[i]; }"
		"    *term = 48 + limit;"
		"    return 0;"
		"}",
		"ok5");
}

TEST(e2e, a_ternary_result_never_takes_the_register_of_a_parameter_still_read_after_it)
{
	// A leaf keeps its parameters in r0-r3. The ternary's result is a temporary that lives across
	// several blocks, and the pass that places those used to start from a fresh register pool - so it
	// could land in r3, the register of `out`, which is only read once the loop is over. The digits came
	// out as garbage at -O1 and -O2 and correct at -O0, which is why this runs at every level.
	runsTheSameAtEveryLevel("ternary_temp_vs_param",
		"static int utoa_base(unsigned int v, int base, int upper, char* out) {"
		"    char tmp[34];"
		"    int n = 0;"
		"    if (v == 0) { tmp[0] = 48; n = 1; }"
		"    while (v != 0) {"
		"        int d = (int)(v % (unsigned int)base);"
		"        tmp[n] = (char)(d < 10 ? 48 + d : (upper ? 65 : 97) + (d - 10));"
		"        n++;"
		"        v /= (unsigned int)base;"
		"    }"
		"    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];"
		"    out[n] = 0;"
		"    return n;"
		"}"
		"int main() {"
		"    char buf[40];"
		"    char* term = (char*)0xFF000004;"
		"    int n = utoa_base(4294967295u, 16, 0, buf);"
		"    for (int i = 0; i < n; i++) *term = buf[i];"
		"    return 0;"
		"}",
		"ffffffff");
}

TEST(e2e, anonymous_struct_union_and_enum_compute_the_right_values)
{
	runsTheSameAtEveryLevel("anonymous_types",
		"enum { One = 1, Two, Three };"
		"typedef struct { int x; char y; } Pair;"
		"typedef union { int i; char c; } Word;"
		"typedef enum { Low = 10, High } Level;"
		"struct { int total; } acc;"
		"int main() {"
		"    Pair p; Word w; Level l;"
		"    p.x = Two; p.y = Three; w.i = 0; w.c = One; l = High;"
		"    acc.total = p.x + p.y + w.i + (l - Low) + sizeof(Pair) - 8;"   // 2 + 3 + 1 + 1 + 0
		"    char* term = (char*)0xFF000004;"
		"    *term = 48 + acc.total;"
		"    return 0;"
		"}",
		"7");
}

TEST(e2e, a_float_global_with_a_whole_value_assembles)
{
	// std::format prints 1.0f as "1", which CASM reads as an integer and refuses for an f32: any
	// global float holding a whole number (scalar or array element) used to fail to assemble.
	runsTheSameAtEveryLevel("float_globals",
		"float one = 1.0f;"
		"float ten = 10;"
		"float neg = -3.0f;"
		"static const float table[4] = { 1.0f, 10.0f, 100.0f, 2.5f };"
		"int main() {"
		"    float total = one + ten + neg + table[0] + table[1] + table[2] + table[3];"   // 1+10-3+1+10+100+2.5
		"    char* term = (char*)0xFF000004;"
		"    *term = 48 + (int)(total / 25.0f);"                                             // 121.5 / 25 = 4
		"    return 0;"
		"}",
		"4");
}

TEST(e2e, an_array_sized_by_its_initializer_has_the_right_size_and_contents)
{
	runsTheSameAtEveryLevel("inferred_array_size",
		"static const int primes[] = { 2, 3, 5, 7, 11 };"
		"char greeting[] = \"hey\";"
		"struct P { int x; int y; };"
		"struct P points[] = { {1, 2}, {3, 4}, {5, 6} };"
		"int grid[][2] = { {1, 2}, {3, 4} };"
		"int main() {"
		"    int local[] = { 10, 20, 30, 40 };"
		"    char word[] = \"ab\";"
		"    int sum = 0; int i;"
		"    for (i = 0; i < sizeof(primes) / sizeof(primes[0]); i = i + 1) sum = sum + primes[i];"          // 28
		"    sum = sum + sizeof(greeting) + greeting[2] - 'y';"                                              // + 4 + 0
		"    sum = sum + sizeof(points) / sizeof(points[0]) + points[2].y;"                                  // + 3 + 6
		"    sum = sum + sizeof(grid) / sizeof(grid[0]) + grid[1][0];"                                       // + 2 + 3
		"    sum = sum + sizeof(local) / 4 + local[3] + sizeof(word);"                                       // + 4 + 40 + 3
		"    char* term = (char*)0xFF000004;"
		"    *term = 'A' + (sum - 28 - 4 - 9 - 5 - 47);"                                                     // sum = 28+4+9+5+47 = 93 -> 'A'
		"    return 0;"
		"}",
		"A");
}

TEST(e2e, an_array_size_written_as_arithmetic_is_computed)
{
	runsTheSameAtEveryLevel("array_size_arithmetic",
		"int a[3 * 4]; char b[16 + 16]; int c[1 << 3][2 + 1];"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    *term = 48 + (sizeof(a) / 4 - 10) + (sizeof(b) - 30) + (sizeof(c) / 4 - 22);"   // 12-10 + 32-30 + 24-22 = 6
		"    return 0;"
		"}",
		"6");
}

TEST(e2e, a_frame_of_only_a_byte_keeps_the_stack_word_aligned)
{
	// g's only frame slot is its fifth parameter, a char that arrives on the stack and is needed by
	// every call it makes. That frame used to be ONE byte, so sp was left odd and leaf()'s first word
	// store faulted (AlignmentFault) - the program never reached its output.
	runsTheSameAtEveryLevel("byte_only_frame",
		"int sink[2];"
		"void leaf(int x, int y, int z, char c) { int t[4]; t[0] = x; t[1] = y; t[2] = z; t[3] = c; sink[0] = t[0] + t[3]; }"
		"void g(int a, int b, int c, int d, char e) { leaf(a, b, c, e); leaf(b, c, d, e); leaf(c, d, a, e); }"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    g(1, 2, 3, 4, 5);"
		"    *term = 48 + sink[0] + 9;"                // the last leaf: t[0] = c = 3 and t[3] = e = 5, so sink[0] = 8
		"    return 0;"
		"}",
		"A");
}

TEST(e2e, a_ternary_with_a_pointer_and_zero_yields_the_pointer_or_null)
{
	runsTheSameAtEveryLevel("ternary_pointer_zero",
		"char* pick(int c, char* p) { return c ? p : 0; }"
		"char* pick2(int c, char* p) { return c ? 0 : p; }"
		"int main() {"
		"    char text[4]; text[0] = 'k';"
		"    char* a = pick(1, text); char* b = pick(0, text); char* c = pick2(1, text); char* d = pick2(0, text);"
		"    char* term = (char*)0xFF000004;"
		"    *term = 48 + (a == text) + (b == 0) * 2 + (c == 0) * 4 + (d == text) * 8;"   // 1 + 2 + 4 + 8 = 15 -> '?'
		"    return 0;"
		"}",
		"?");
}

TEST(e2e, a_const_struct_is_copied_by_value)
{
	runsTheSameAtEveryLevel("const_struct_copy",
		"struct P { int x; int y; };"
		"static const struct P origin = { 3, 4 };"
		"int sum(const struct P* p) { struct P copy = *p; copy.x = copy.x + 10; return copy.x + copy.y + p->x; }"
		"int main() {"
		"    struct P local; local = origin; local.y = 1;"
		"    char* term = (char*)0xFF000004;"
		"    *term = 48 + (sum(&origin) - 17) + local.y + origin.y - 5;"   // (13+4+3=20) - 17 + 1 + 4 - 5 = 3
		"    return 0;"
		"}",
		"3");
}

TEST(e2e, a_static_object_can_be_initialized_with_the_address_of_another)
{
	runsTheSameAtEveryLevel("static_address_initializer",
		"struct S { int w; const int* px; };"
		"static const int cells[3] = { 4, 5, 6 };"
		"static int counter;"
		"static const struct S s = { 3, cells };"
		"static int* cp = &counter;"
		"static const struct S* table[2] = { &s, &s };"
		"int main() {"
		"    static int local[2]; static int* lp = local; lp[1] = 7;"
		"    *cp = 2;"
		"    char* term = (char*)0xFF000004;"
		"    *term = 48 + s.px[1] - 5 + counter + (table[1]->w - 3) + (local[1] - 7);"   // 5 - 5 + 2 + 0 + 0 -> '2'
		"    return 0;"
		"}",
		"2");
}

TEST(e2e, a_cast_constant_can_initialize_a_static_object)
{
	runsTheSameAtEveryLevel("static_cast_initializer",
		"static const char* names[3] = { \"a\", ((void*)0), \"c\" };"
		"static int* none = (int*)0;"
		"static char big[8];"
		"static char* bytes = (char*)big;"
		"static float three = (float)3;"
		"char plain = (char)65;"
		"int main() {"
		"    bytes[2] = 4;"
		"    char* term = (char*)0xFF000004;"
		"    *term = 48 + (names[0][0] == 'a') + (names[1] == 0) * 2 + (names[2][0] == 'c') + (none == 0) + big[2] - 4 + (plain == 'A') + (three > 2.5f);"   // 1 + 2 + 1 + 1 + 0 + 1 + 1 = 7
		"    return 0;"
		"}",
		"7");
}

TEST(e2e, a_static_function_named_only_by_a_data_initializer_is_kept)
{
	// Nothing calls `twice` or `plus_one` by name: their addresses sit in the initializers of a table and of
	// a static local, and that is the only reason unused-function elimination must leave them alone.
	runsTheSameAtEveryLevel("static_function_in_table",
		"static int twice(int a) { return a * 2; }"
		"static int plus_one(int a) { return a + 1; }"
		"static int minus_one(int a) { return a - 1; }"
		"struct Ops { int (*apply)(int); int (*undo)(int); };"
		"static const struct Ops ops = { twice, minus_one };"
		"static int (*table[2])(int) = { plus_one, twice };"
		"int main() {"
		"    static int (*local)(int) = plus_one;"
		"    char* term = (char*)0xFF000004;"
		"    *term = 48 + ops.apply(2) + table[0](1) + table[1](1) + local(0) + ops.undo(1) - 8;"   // 4 + 2 + 2 + 1 + 0 - 8 + 48 -> '1'
		"    return 0;"
		"}",
		"1");
}

TEST(e2e, a_variadic_function_sums_its_argument_tail)
{
	runsTheSameAtEveryLevel("variadic_sum",
		"int sum(int count, ...) {"
		"    __builtin_va_list ap;"
		"    int total = 0;"
		"    int i;"
		"    __builtin_va_start(ap, count);"
		"    for (i = 0; i < count; i = i + 1) total = total + __builtin_va_arg(ap, int);"
		"    __builtin_va_end(ap);"
		"    return total;"
		"}"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    *term = 48 + sum(4, 1, 2, 0, 3);" // single digit (6) keeps the raw-MMIO print simple
		"    return 0;"
		"}",
		"6");
}

TEST(e2e, a_variadic_function_finds_its_tail_past_stack_passed_fixed_parameters)
{
	// Six fixed parameters, so two of them arrive on the stack before the tail even begins - the
	// case where the tail's starting offset is not simply [fp + 8].
	runsTheSameAtEveryLevel("variadic_spilled_fixed",
		"int pick(int a, int b, int c, int d, int e, int f, ...) {"
		"    __builtin_va_list ap;"
		"    int v = 0;"
		"    int i;"
		"    __builtin_va_start(ap, f);"
		"    for (i = 0; i < 3; i = i + 1) v = v + __builtin_va_arg(ap, int);"
		"    __builtin_va_end(ap);"
		"    return v + a + b + c + d + e + f;"
		"}"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    *term = 48 + pick(1, 1, 1, 1, 1, 1, 1, 1, 0);" // 6 fixed + 2 variadic = 8
		"    return 0;"
		"}",
		"8");
}

TEST(e2e, a_float_travels_through_the_tail_and_va_copy_rereads_it)
{
	// `float` is passed as the f32 it already is - the machine has no f64 for C's own float-to-
	// double promotion to target (docs/09-Variadic-Convention.md).
	runsTheSameAtEveryLevel("variadic_float_and_copy",
		"int check(int n, ...) {"
		"    __builtin_va_list ap;"
		"    __builtin_va_list copy;"
		"    float f;"
		"    int i;"
		"    __builtin_va_start(ap, n);"
		"    f = __builtin_va_arg(ap, float);"
		"    i = __builtin_va_arg(ap, int);"
		"    __builtin_va_copy(copy, ap);"
		"    int total = (int)f + i + __builtin_va_arg(copy, int);"
		"    __builtin_va_end(copy);"
		"    __builtin_va_end(ap);"
		"    return total + n;"
		"}"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    *term = 48 + check(0, 2.0, 3, 4);" // 2 + 3 + 4 = 9
		"    return 0;"
		"}",
		"9");
}

TEST(e2e, a_va_list_can_be_handed_to_another_function)
{
	// The vprintf pattern: the worker is not variadic itself, it just consumes a cursor it was
	// given - which works because a __builtin_va_list is an ordinary pointer.
	runsTheSameAtEveryLevel("variadic_forwarded_list",
		"int vsum(int count, __builtin_va_list ap) {"
		"    int total = 0;"
		"    int i;"
		"    for (i = 0; i < count; i = i + 1) total = total + __builtin_va_arg(ap, int);"
		"    return total;"
		"}"
		"int sum(int count, ...) {"
		"    __builtin_va_list ap;"
		"    __builtin_va_start(ap, count);"
		"    int r = vsum(count, ap);"
		"    __builtin_va_end(ap);"
		"    return r;"
		"}"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    *term = 48 + sum(3, 2, 2, 1);" // 5
		"    return 0;"
		"}",
		"5");
}
