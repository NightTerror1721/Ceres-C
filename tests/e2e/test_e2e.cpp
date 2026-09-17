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

	// Finding `ceres`, launching it with its stdout captured and reading a file back are shared
	// with tests/examples, so they live in tests/framework/ceres_tool.h rather than twice here.
	using namespace ceresc::testing;

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
