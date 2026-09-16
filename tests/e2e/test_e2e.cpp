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

// ---- bugs this suite pins, but does not fix ---------------------------------------------------
//
// Found while building examples/ for Fase 8 (§13). All three are front/back-end gaps from earlier
// phases, not integration problems, so they are recorded here rather than worked around: the
// marker fails today by design and turns the run RED the moment the behaviour becomes correct,
// which is the signal to delete it. docs/06-Known-Limitations.md describes each one in prose.
//
// The helper below fails on purpose when there is no `ceres` to run against, too. An ordinary TEST
// skips in that case; a TEST_KNOWN_FAILURE that skipped would report "unexpectedly passed" and go
// red on a checkout with no sibling CeresASM, which says nothing about the bug.

namespace
{
	void pinnedBugIsFixedWhenThisPasses(std::string_view name, std::string_view source, std::string_view correct)
	{
		using ceresc::support::OptimizationLevel;

		if (!findCeresDirectory())
		{
			CHECK(findCeresDirectory().has_value()); // see the note above: never silently "passes"
			return;
		}
		for (OptimizationLevel level : { OptimizationLevel::O0, OptimizationLevel::O1, OptimizationLevel::O2 })
		{
			std::string output = compileAssembleAndRun(name, source, level);
			CHECK_EQ(std::format("O{}:{}", static_cast<int>(level), output),
				std::format("O{}:{}", static_cast<int>(level), correct));
		}
	}
}

TEST_KNOWN_FAILURE(e2e, a_negative_signed_char_read_back_from_memory_keeps_its_sign,
	"narrow loads never sign-extend: `ldrb`/`ldrh` are unsigned, and -O0 disagrees with -O1/-O2")
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
	pinnedBugIsFixedWhenThisPasses("signed_char_roundtrip",
		"int opaque(int v) { return v; }"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    signed char small = (signed char)opaque(-3);"
		"    *term = 48 + (small < 0);" // '1' if it is really -3, '0' if it came back as 253
		"    return 0;"
		"}",
		"1");
}

TEST_KNOWN_FAILURE(e2e, a_cast_to_a_narrower_integer_type_truncates,
	"(char)/(short) casts are no-ops: the value keeps all 32 bits at every optimization level")
{
	// Unlike the one above, this is wrong the same way at all three levels: the conversion is
	// dropped entirely rather than lowered to a truncation.
	pinnedBugIsFixedWhenThisPasses("narrowing_cast",
		"int opaque(int v) { return v; }"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    int wide = opaque(0x101);"                 // 257: its low byte is 1
		"    *term = 48 + ((int)(char)wide == 1);"      // compared, not printed - see the note above
		"    return 0;"
		"}",
		"1");
}

TEST_KNOWN_FAILURE(e2e, converting_an_int_to_bool_normalizes_to_zero_or_one,
	"an int assigned to a bool keeps its value instead of becoming 0/1")
{
	pinnedBugIsFixedWhenThisPasses("bool_normalization",
		"int opaque(int v) { return v; }"
		"int main() {"
		"    char* term = (char*)0xFF000004;"
		"    bool flag = opaque(42);"
		"    *term = 48 + flag;" // '1', since any non-zero value converts to true
		"    return 0;"
		"}",
		"1");
}
