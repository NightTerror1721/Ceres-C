#include <ceresc/driver/options.h>
#include <ceresc/support/optimization.h>

#include "framework.h"

#include <initializer_list>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// Command-line parsing (§11), with the optimization controls as the main subject: -O0/-O1/-O2 and
// the per-optimization -f<name>/-fno-<name> switches. What these tests are really protecting is the
// project's promise that the optimizations are ON by default and that EVERY one of them can be
// turned off individually - see support/optimization.h's header comment on why the simplified path
// stays reachable at all.

using namespace ceresc;

namespace
{
	struct ParseResult
	{
		std::optional<driver::Options> options;
		std::string output; // whatever was written to the diagnostics stream (usage, errors)
	};

	ParseResult parse(std::initializer_list<const char*> args)
	{
		std::vector<const char*> argv(args);
		std::ostringstream out;
		std::optional<driver::Options> options = driver::parseOptions(argv, out);
		return ParseResult{ std::move(options), out.str() };
	}

	bool contains(std::string_view haystack, std::string_view needle)
	{
		return haystack.find(needle) != std::string_view::npos;
	}
}

// ---- the basics ---------------------------------------------------------------------------------

TEST(options, an_input_file_on_its_own_is_enough)
{
	ParseResult result = parse({ "main.c" });
	CHECK(result.options.has_value());
	CHECK_EQ(result.options->inputPaths.size(), std::size_t(1));
	CHECK_EQ(result.options->inputPaths.front(), std::string("main.c"));
	CHECK(!result.options->emitAst);
	CHECK(!result.options->emitIr);
	CHECK(!result.options->run);
}

TEST(options, no_arguments_at_all_prints_usage_and_parses_nothing)
{
	ParseResult result = parse({});
	CHECK(!result.options.has_value());
	CHECK(contains(result.output, "usage:"));
}

TEST(options, an_unknown_option_is_rejected_rather_than_ignored)
{
	ParseResult result = parse({ "main.c", "--nonsense" });
	CHECK(!result.options.has_value());
	CHECK(contains(result.output, "--nonsense"));
}

TEST(options, the_ordinary_switches_are_recognized)
{
	ParseResult result = parse({ "main.c", "-o", "out.casm", "--emit-ir", "--run", "--clean", "--ceres-path", "C:/ceres", "-Werror" });
	CHECK(result.options.has_value());
	CHECK_EQ(result.options->outputPath, std::string("out.casm"));
	CHECK(result.options->emitIr);
	CHECK(result.options->run);
	CHECK(result.options->clean);
	CHECK(!result.options->cleanKeepCasm);
	CHECK_EQ(result.options->ceresPath, std::string("C:/ceres"));
	CHECK(result.options->warningsAsErrors);
}

TEST(options, the_library_and_sysroot_switches_are_recognized)
{
	// Both spellings of -L and -l, and --sysroot, in command-line order.
	ParseResult result = parse({ "main.c", "-L", "libs", "-Lmore", "-lceres", "-l", "extra", "--sysroot", "C:/sdk" });
	CHECK(result.options.has_value());
	CHECK_EQ(result.options->libraryDirectories.size(), std::size_t(2));
	CHECK_EQ(result.options->libraryDirectories[0], std::string("libs"));
	CHECK_EQ(result.options->libraryDirectories[1], std::string("more"));
	CHECK_EQ(result.options->libraries.size(), std::size_t(2));
	CHECK_EQ(result.options->libraries[0], std::string("ceres"));
	CHECK_EQ(result.options->libraries[1], std::string("extra"));
	CHECK_EQ(result.options->sysroot, std::string("C:/sdk"));
}

TEST(options, the_size_and_debug_levels_and_the_stats_switch_are_recognized)
{
	ParseResult size = parse({ "main.c", "-Os", "--stats" });
	CHECK(size.options.has_value());
	CHECK(size.options->emitStats);
	CHECK(!size.options->optimization.inlining);       // -Os: no inlining
	CHECK(!size.options->optimization.jumpTables);     //      and no jump tables
	CHECK(size.options->optimization.constantFolding); // but still the O1 passes

	ParseResult debug = parse({ "main.c", "-Og", "-fstats" });
	CHECK(debug.options.has_value());
	CHECK(debug.options->emitStats);
	CHECK(!debug.options->optimization.inlining);       // -Og: no inlining
	CHECK(!debug.options->optimization.localSlotReuse); //       and a slot per scope
	CHECK(debug.options->optimization.jumpTables);      //       jump tables are fine
}

TEST(options, a_library_name_with_no_argument_is_rejected)
{
	ParseResult result = parse({ "main.c", "-l" });
	CHECK(!result.options.has_value());
}

TEST(options, S_and_run_are_opposites_resolved_in_argument_order)
{
	// -S is the default, so on its own it changes nothing...
	ParseResult onlyS = parse({ "main.c", "-S" });
	CHECK(onlyS.options.has_value());
	CHECK(!onlyS.options->run);

	// ...but it is not a no-op: written after --run it takes the assembling back off, which is the
	// case §11 introduces it for ("por si --run se anade despues por costumbre").
	ParseResult runThenS = parse({ "main.c", "--run", "-S" });
	CHECK(runThenS.options.has_value());
	CHECK(!runThenS.options->run);

	// And the other order means what it says too - last one wins, like -O against -f.
	ParseResult sThenRun = parse({ "main.c", "-S", "--run" });
	CHECK(sThenRun.options.has_value());
	CHECK(sThenRun.options->run);
}

TEST(options, version_is_an_ordinary_flag_and_needs_no_input_file)
{
	ParseResult result = parse({ "--version" });
	CHECK(result.options.has_value());
	CHECK(result.options->showVersion);
	CHECK(result.options->inputPaths.empty());
	CHECK(result.output.empty()); // not an error, so nothing is printed to the diagnostics stream
}

TEST(options, version_is_recognized_wherever_it_appears)
{
	// It used to be read off argv[1] by the application entry point, which meant `ceresc main.c
	// --version` silently compiled instead.
	ParseResult result = parse({ "main.c", "--version" });
	CHECK(result.options.has_value());
	CHECK(result.options->showVersion);
	CHECK_EQ(result.options->inputPaths.size(), std::size_t(1));
	CHECK_EQ(result.options->inputPaths.front(), std::string("main.c"));
}

TEST(options, help_prints_the_usage_text_and_parses_nothing)
{
	for (const char* flag : { "--help", "-h" })
	{
		std::vector<const char*> argv{ flag };
		std::ostringstream out;
		std::optional<driver::Options> options = driver::parseOptions(argv, out);
		CHECK(!options.has_value());
		CHECK(contains(out.str(), "usage:"));
	}
}

TEST(options, the_usage_text_documents_every_option_of_the_plan)
{
	std::ostringstream out;
	driver::printUsage(out);
	const std::string text = out.str();
	for (std::string_view option : { "-o ", "--emit-ast", "--emit-ir", "-S", "--run", "--clean", "--clean-keep-casm", "--ceres-path", "-Werror", "--version", "--help" })
		CHECK(contains(text, option));
}

TEST(options, cleanup_modes_require_run_and_the_last_one_wins)
{
	ParseResult withoutRun = parse({ "main.c", "--clean" });
	CHECK(!withoutRun.options.has_value());
	CHECK(contains(withoutRun.output, "require '--run'"));

	ParseResult keepCasm = parse({ "main.c", "--run", "--clean", "--clean-keep-casm" });
	CHECK(keepCasm.options.has_value());
	CHECK(!keepCasm.options->clean);
	CHECK(keepCasm.options->cleanKeepCasm);

	ParseResult removeAll = parse({ "main.c", "--run", "--clean-keep-casm", "--clean" });
	CHECK(removeAll.options.has_value());
	CHECK(removeAll.options->clean);
	CHECK(!removeAll.options->cleanKeepCasm);
}

TEST(options, an_option_that_needs_a_value_and_does_not_get_one_is_an_error)
{
	ParseResult result = parse({ "main.c", "-o" });
	CHECK(!result.options.has_value());
	CHECK(contains(result.output, "-o"));
}

// ---- optimization levels -------------------------------------------------------------------------

TEST(options, optimizations_are_on_by_default)
{
	// The headline requirement: compiling with no -O flag at all optimizes.
	ParseResult result = parse({ "main.c" });
	CHECK(result.options.has_value());
	CHECK(result.options->optimization.constantFolding);
	CHECK(result.options->optimization.framelessLeaf);
	CHECK(result.options->optimization.localSlotReuse);
	CHECK(result.options->optimization.cmpBranchFusion);
	CHECK(result.options->optimization.registerAllocation);
	CHECK(!result.options->optimization.inlining); // ...but inlining waits for -O2
}

TEST(options, O0_turns_every_optimization_off)
{
	ParseResult result = parse({ "main.c", "-O0" });
	CHECK(result.options.has_value());

	// Every flag in the table, not a hand-picked few: -O0 has to mean "the simplified behaviour" for
	// all of them, or a new optimization could quietly stay on at -O0 and break bisection.
	for (const support::OptimizationFlag& flag : support::optimizationFlags())
		CHECK(!(result.options->optimization.*(flag.field)));
}

TEST(options, O2_turns_every_optimization_on)
{
	ParseResult result = parse({ "main.c", "-O2" });
	CHECK(result.options.has_value());
	for (const support::OptimizationFlag& flag : support::optimizationFlags())
		CHECK(result.options->optimization.*(flag.field));
}

TEST(options, O1_is_O2_without_inlining)
{
	ParseResult result = parse({ "main.c", "-O1" });
	CHECK(result.options.has_value());
	for (const support::OptimizationFlag& flag : support::optimizationFlags())
	{
		if (flag.name == "inline")
			CHECK(!(result.options->optimization.*(flag.field)));
		else
			CHECK(result.options->optimization.*(flag.field));
	}
}

TEST(options, bare_O_means_O1)
{
	ParseResult bare = parse({ "main.c", "-O" });
	ParseResult explicitLevel = parse({ "main.c", "-O1" });
	CHECK(bare.options.has_value());
	CHECK(explicitLevel.options.has_value());
	CHECK_EQ(bare.options->optimization.inlining, explicitLevel.options->optimization.inlining);
	CHECK_EQ(bare.options->optimization.constantFolding, explicitLevel.options->optimization.constantFolding);
}

TEST(options, O3_is_accepted_and_behaves_as_O2)
{
	// Nothing above O2 exists yet, and refusing a level a user reasonably types would be unfriendly
	// for no gain - it saturates, the way a C compiler's highest level does.
	ParseResult result = parse({ "main.c", "-O3" });
	CHECK(result.options.has_value());
	CHECK(result.options->optimization.inlining);
}

// ---- individual -f switches -------------------------------------------------------------------------

TEST(options, every_optimization_in_the_table_can_be_turned_off_by_name)
{
	// The other headline requirement, checked exhaustively rather than for the three optimizations
	// that prompted it: each -fno-<name> turns off exactly its own flag and touches nothing else.
	for (const support::OptimizationFlag& flag : support::optimizationFlags())
	{
		std::string argument = "-fno-" + std::string(flag.name);
		std::vector<const char*> argv{ "main.c", "-O2", argument.c_str() };
		std::ostringstream out;
		std::optional<driver::Options> options = driver::parseOptions(argv, out);

		CHECK(options.has_value());
		if (!options)
			continue;
		for (const support::OptimizationFlag& other : support::optimizationFlags())
			CHECK_EQ(options->optimization.*(other.field), other.name != flag.name);
	}
}

TEST(options, every_optimization_in_the_table_can_be_turned_on_by_name)
{
	for (const support::OptimizationFlag& flag : support::optimizationFlags())
	{
		std::string argument = "-f" + std::string(flag.name);
		std::vector<const char*> argv{ "main.c", "-O0", argument.c_str() };
		std::ostringstream out;
		std::optional<driver::Options> options = driver::parseOptions(argv, out);

		CHECK(options.has_value());
		if (!options)
			continue;
		for (const support::OptimizationFlag& other : support::optimizationFlags())
			CHECK_EQ(options->optimization.*(other.field), other.name == flag.name);
	}
}

TEST(options, the_three_optimizations_this_was_built_for_each_have_their_own_switch)
{
	// Named explicitly, because these are the three the simplified counterpart was specified for:
	// frameless-leaf, the frame-slot register windows, and the Cmp+CondJump peephole.
	ParseResult result = parse({ "main.c", "-fno-frameless-leaf", "-fno-local-slot-reuse", "-fno-cmp-branch-fusion" });
	CHECK(result.options.has_value());
	CHECK(!result.options->optimization.framelessLeaf);
	CHECK(!result.options->optimization.localSlotReuse);
	CHECK(!result.options->optimization.cmpBranchFusion);
	CHECK(result.options->optimization.constantFolding); // nothing else disturbed
}

TEST(options, a_later_switch_overrides_an_earlier_level)
{
	ParseResult result = parse({ "main.c", "-O0", "-fconst-fold" });
	CHECK(result.options.has_value());
	CHECK(result.options->optimization.constantFolding);
	CHECK(!result.options->optimization.inlining); // still -O0 for everything else
}

TEST(options, a_later_level_overrides_an_earlier_switch)
{
	// Resolution is strictly in command-line order, the way a C compiler does it - so an -O after an
	// -f wins, and this is the direction that is easy to get backwards.
	ParseResult result = parse({ "main.c", "-fno-const-fold", "-O2" });
	CHECK(result.options.has_value());
	CHECK(result.options->optimization.constantFolding);
}

TEST(options, an_unknown_optimization_name_is_rejected)
{
	ParseResult result = parse({ "main.c", "-fno-such-optimization" });
	CHECK(!result.options.has_value());
	CHECK(contains(result.output, "such-optimization"));
}

TEST(options, the_usage_text_lists_every_optimization_by_name)
{
	// A flag that exists but is undiscoverable may as well not exist. Checking the usage text
	// against the table itself means a newly added optimization cannot be forgotten here.
	ParseResult result = parse({});
	for (const support::OptimizationFlag& flag : support::optimizationFlags())
		CHECK(contains(result.output, flag.name));
}

// ---- several inputs, include paths and defines --------------------------------------------------

TEST(options, several_inputs_are_kept_in_order)
{
	ParseResult result = parse({ "io.c", "main.c", "runtime.casm" });
	CHECK(result.options.has_value());
	CHECK_EQ(result.options->inputPaths.size(), std::size_t(3));
	CHECK_EQ(result.options->inputPaths[0], std::string("io.c"));
	CHECK_EQ(result.options->inputPaths[1], std::string("main.c"));
	CHECK_EQ(result.options->inputPaths[2], std::string("runtime.casm"));
}

TEST(options, an_input_may_be_a_casm_file)
{
	// The driver decides what to do with it by extension - a .casm is assembled and linked, not
	// compiled - but the command line does not care which is which.
	ParseResult result = parse({ "runtime.casm" });
	CHECK(result.options.has_value());
	CHECK_EQ(result.options->inputPaths.size(), std::size_t(1));
}

TEST(options, include_directories_accept_both_spellings_and_keep_their_order)
{
	ParseResult result = parse({ "main.c", "-I", "first", "-Isecond" });
	CHECK(result.options.has_value());
	CHECK_EQ(result.options->includeDirectories.size(), std::size_t(2));
	CHECK_EQ(result.options->includeDirectories[0], std::string("first"));
	CHECK_EQ(result.options->includeDirectories[1], std::string("second"));
}

TEST(options, a_bare_D_defines_the_macro_as_one)
{
	// What every C compiler does, and what makes `-D DEBUG` useful without a value.
	ParseResult result = parse({ "main.c", "-D", "DEBUG" });
	CHECK(result.options.has_value());
	CHECK_EQ(result.options->defines.size(), std::size_t(1));
	CHECK_EQ(result.options->defines[0].first, std::string("DEBUG"));
	CHECK_EQ(result.options->defines[0].second, std::string("1"));
}

TEST(options, a_D_with_a_value_splits_at_the_first_equals)
{
	ParseResult result = parse({ "main.c", "-DWIDTH=320", "-DTEXT=a=b" });
	CHECK(result.options.has_value());
	CHECK_EQ(result.options->defines.size(), std::size_t(2));
	CHECK_EQ(result.options->defines[0].first, std::string("WIDTH"));
	CHECK_EQ(result.options->defines[0].second, std::string("320"));
	// Everything after the first '=' is the replacement, including further '=' signs.
	CHECK_EQ(result.options->defines[1].first, std::string("TEXT"));
	CHECK_EQ(result.options->defines[1].second, std::string("a=b"));
}

TEST(options, E_asks_for_the_preprocessed_source)
{
	ParseResult result = parse({ "main.c", "-E" });
	CHECK(result.options.has_value());
	CHECK(result.options->emitPreprocessed);
}

TEST(options, an_option_that_needs_a_value_at_the_very_end_is_an_error)
{
	for (const char* flag : { "-I", "-D", "--ceres-path" })
	{
		std::vector<const char*> argv{ "main.c", flag };
		std::ostringstream out;
		std::optional<driver::Options> options = driver::parseOptions(argv, out);
		CHECK(!options.has_value());
		CHECK(contains(out.str(), flag));
	}
}

// ---- built code: objects, archives and their declarations ---------------------------------------

TEST(options, an_object_and_an_archive_are_inputs_kept_in_order)
{
	ParseResult result = parse({ "main.c", "lib.car", "extra.cobj", "--run" });
	CHECK(result.options.has_value());
	CHECK_EQ(result.options->inputPaths.size(), std::size_t(3));
	CHECK_EQ(result.options->inputPaths[1], std::string("lib.car"));
	CHECK_EQ(result.options->inputPaths[2], std::string("extra.cobj"));
}

TEST(options, decls_may_be_given_more_than_once_and_keeps_its_order)
{
	ParseResult result = parse({ "main.c", "--decls", "a.decls.casm", "--decls", "b.decls.casm" });
	CHECK(result.options.has_value());
	CHECK_EQ(result.options->declsFiles.size(), std::size_t(2));
	CHECK_EQ(result.options->declsFiles[0], std::string("a.decls.casm"));
	CHECK_EQ(result.options->declsFiles[1], std::string("b.decls.casm"));
}

TEST(options, emit_decls_names_the_file_to_write)
{
	ParseResult result = parse({ "lib.c", "--emit-decls", "lib.decls.casm" });
	CHECK(result.options.has_value());
	CHECK_EQ(result.options->emitDeclsPath, std::string("lib.decls.casm"));
}

TEST(options, decls_and_emit_decls_at_the_very_end_are_errors)
{
	CHECK(!parse({ "main.c", "--decls" }).options.has_value());
	CHECK(!parse({ "main.c", "--emit-decls" }).options.has_value());
}

TEST(options, the_usage_text_mentions_objects_archives_and_declarations)
{
	ParseResult result = parse({});
	CHECK(contains(result.output, "file.cobj"));
	CHECK(contains(result.output, "file.car"));
	CHECK(contains(result.output, "--decls"));
	CHECK(contains(result.output, "--emit-decls"));
}

TEST(options, run_arg_collects_the_extra_arguments_for_ceres_run_in_order)
{
	ParseResult result = parse({ "ceresc", "main.c", "--run", "--run-arg", "--port", "--run-arg", "0=stick.img" });
	CHECK(result.options.has_value());
	if (!result.options) return;
	CHECK_EQ(result.options->runArguments.size(), usize{ 2 });
	CHECK_EQ(result.options->runArguments[0], std::string{ "--port" });
	CHECK_EQ(result.options->runArguments[1], std::string{ "0=stick.img" });
	CHECK(!parse({ "ceresc", "main.c", "--run", "--run-arg" }).options.has_value());
}
