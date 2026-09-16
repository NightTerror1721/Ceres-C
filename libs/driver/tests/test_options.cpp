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
	CHECK_EQ(result.options->inputPath, std::string("main.c"));
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
	ParseResult result = parse({ "main.c", "-o", "out.casm", "--emit-ir", "--run", "--ceres-path", "C:/ceres", "-Werror" });
	CHECK(result.options.has_value());
	CHECK_EQ(result.options->outputPath, std::string("out.casm"));
	CHECK(result.options->emitIr);
	CHECK(result.options->run);
	CHECK_EQ(result.options->ceresPath, std::string("C:/ceres"));
	CHECK(result.options->warningsAsErrors);
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
