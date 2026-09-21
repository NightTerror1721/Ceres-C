#include <ceresc/driver/ceres_locator.h>

#include "framework.h"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

// Where `--run` finds `ceres`: --ceres-path, then CERES_PATH, then PATH, most specific first. The lookup takes
// its environment as an argument, so every combination is tried here against real (empty) files in a temporary
// tree, without touching the environment the tests themselves run in.

using namespace ceresc;
namespace fs = std::filesystem;

namespace
{
#if defined(_WIN32)
	constexpr const char* kName = "ceres.exe";
	constexpr char kSep = ';';
#else
	constexpr const char* kName = "ceres";
	constexpr char kSep = ':';
#endif

	// A scratch directory tree removed when it goes out of scope.
	class Tree
	{
	public:
		Tree()
		{
			static std::atomic<int> counter{ 0 };
			_root = fs::temp_directory_path() / ("ceresc_locator_test_" + std::to_string(counter++) + "_" + std::to_string(std::rand()));
			fs::create_directories(_root);
		}
		~Tree() { std::error_code error; fs::remove_all(_root, error); }

		const fs::path& root() const { return _root; }

		// A directory holding a `ceres` (an empty file is enough: nothing here runs it).
		fs::path withCeres(const std::string& name)
		{
			fs::path directory = _root / name;
			fs::create_directories(directory);
			std::ofstream(directory / kName) << "x";
#if !defined(_WIN32)
			fs::permissions(directory / kName, fs::perms::owner_all);
#endif
			return directory;
		}

		fs::path withoutCeres(const std::string& name)
		{
			fs::path directory = _root / name;
			fs::create_directories(directory);
			return directory;
		}

	private:
		fs::path _root;
	};

	driver::CeresEnvironment env(std::string path = "", std::optional<std::string> ceresPath = std::nullopt)
	{
		driver::CeresEnvironment environment;
		environment.path = std::move(path);
		environment.ceresPath = std::move(ceresPath);
		return environment;
	}

	bool contains(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }
}

// ---- --ceres-path --------------------------------------------------------------------------------

TEST(ceres_locator, a_directory_given_with_ceres_path_is_searched_for_the_executable)
{
	Tree tree;
	fs::path dir = tree.withCeres("a");
	driver::CeresLookup lookup = driver::locateCeres(dir.string(), env());
	CHECK(lookup.found());
	CHECK_EQ(lookup.executable.string(), (dir / kName).lexically_normal().string());
	CHECK_EQ(lookup.source, std::string("--ceres-path"));
}

TEST(ceres_locator, the_executable_itself_may_be_given)
{
	Tree tree;
	fs::path dir = tree.withCeres("a");
	driver::CeresLookup lookup = driver::locateCeres((dir / kName).string(), env());
	CHECK(lookup.found());
	CHECK_EQ(lookup.executable.string(), (dir / kName).lexically_normal().string());
}

TEST(ceres_locator, the_flag_wins_over_the_environment_and_the_path)
{
	Tree tree;
	fs::path flag = tree.withCeres("flag");
	fs::path variable = tree.withCeres("variable");
	fs::path onPath = tree.withCeres("onpath");
	driver::CeresLookup lookup = driver::locateCeres(flag.string(), env(onPath.string(), variable.string()));
	CHECK_EQ(lookup.executable.string(), (flag / kName).lexically_normal().string());
	CHECK_EQ(lookup.source, std::string("--ceres-path"));
}

TEST(ceres_locator, a_relative_flag_comes_back_as_an_absolute_path)
{
	Tree tree;
	fs::path dir = tree.withCeres("rel");
	fs::path saved = fs::current_path();
	fs::current_path(tree.root());
	driver::CeresLookup lookup = driver::locateCeres("rel", env());
	fs::current_path(saved);
	CHECK(lookup.found());
	CHECK(lookup.executable.is_absolute());
	CHECK_EQ(fs::weakly_canonical(lookup.executable).string(), fs::weakly_canonical(dir / kName).string());
}

TEST(ceres_locator, a_flag_that_names_nothing_is_an_error_and_does_not_fall_through)
{
	Tree tree;
	fs::path onPath = tree.withCeres("onpath");     // a good one is right there on PATH
	fs::path empty = tree.withoutCeres("empty");
	driver::CeresLookup lookup = driver::locateCeres(empty.string(), env(onPath.string()));
	CHECK(!lookup.found());
	CHECK(contains(lookup.error, "--ceres-path"));
	CHECK(contains(lookup.error, empty.string()));
}

// ---- CERES_PATH ----------------------------------------------------------------------------------

TEST(ceres_locator, ceres_path_is_used_when_there_is_no_flag_and_wins_over_path)
{
	Tree tree;
	fs::path variable = tree.withCeres("variable");
	fs::path onPath = tree.withCeres("onpath");
	driver::CeresLookup lookup = driver::locateCeres("", env(onPath.string(), variable.string()));
	CHECK_EQ(lookup.executable.string(), (variable / kName).lexically_normal().string());
	CHECK_EQ(lookup.source, std::string("CERES_PATH"));
}

TEST(ceres_locator, ceres_path_may_name_the_executable_too)
{
	Tree tree;
	fs::path variable = tree.withCeres("variable");
	driver::CeresLookup lookup = driver::locateCeres("", env("", (variable / kName).string()));
	CHECK(lookup.found());
	CHECK_EQ(lookup.source, std::string("CERES_PATH"));
}

TEST(ceres_locator, an_empty_ceres_path_counts_as_not_set)
{
	Tree tree;
	fs::path onPath = tree.withCeres("onpath");
	driver::CeresLookup lookup = driver::locateCeres("", env(onPath.string(), std::string("")));
	CHECK(lookup.found());
	CHECK_EQ(lookup.source, std::string("PATH"));
}

TEST(ceres_locator, a_ceres_path_that_holds_no_ceres_is_an_error_not_a_step_down_to_path)
{
	Tree tree;
	fs::path onPath = tree.withCeres("onpath");
	fs::path stale = tree.withoutCeres("stale");
	driver::CeresLookup lookup = driver::locateCeres("", env(onPath.string(), stale.string()));
	CHECK(!lookup.found());
	CHECK(contains(lookup.error, "CERES_PATH"));
	CHECK(contains(lookup.error, stale.string()));
	CHECK(contains(lookup.error, "unset CERES_PATH"));   // and says how to get out of it
}

// ---- PATH ----------------------------------------------------------------------------------------

TEST(ceres_locator, path_gives_the_first_directory_that_holds_ceres)
{
	Tree tree;
	fs::path none = tree.withoutCeres("none");
	fs::path first = tree.withCeres("first");
	fs::path second = tree.withCeres("second");
	std::string path = none.string() + kSep + first.string() + kSep + second.string();
	driver::CeresLookup lookup = driver::locateCeres("", env(path));
	CHECK_EQ(lookup.executable.string(), (first / kName).lexically_normal().string());
	CHECK_EQ(lookup.source, std::string("PATH"));
}

TEST(ceres_locator, path_skips_empty_entries_and_directories_that_do_not_exist)
{
	Tree tree;
	fs::path good = tree.withCeres("good");
	std::string path = std::string(1, kSep) + (tree.root() / "missing").string() + kSep + kSep + good.string();
	driver::CeresLookup lookup = driver::locateCeres("", env(path));
	CHECK_EQ(lookup.executable.string(), (good / kName).lexically_normal().string());
}

TEST(ceres_locator, a_quoted_path_entry_is_unquoted)
{
	Tree tree;
	fs::path good = tree.withCeres("with space");
	driver::CeresLookup lookup = driver::locateCeres("", env("\"" + good.string() + "\""));
	CHECK_EQ(lookup.executable.string(), (good / kName).lexically_normal().string());
}

TEST(ceres_locator, a_directory_that_happens_to_be_called_ceres_is_not_the_executable)
{
	// CeresASM's own checkout has a directory `Ceres/` at its root: on a case-insensitive file system a
	// search for a bare `ceres` can land on it. The executable is a regular file.
	Tree tree;
	fs::path dir = tree.withoutCeres("root");
	fs::create_directories(dir / kName);   // a directory with the executable's name
	driver::CeresLookup lookup = driver::locateCeres("", env(dir.string()));
	CHECK(!lookup.found());
}

TEST(ceres_locator, nothing_anywhere_lists_where_it_looked)
{
	Tree tree;
	fs::path a = tree.withoutCeres("a");
	fs::path b = tree.withoutCeres("b");
	driver::CeresLookup lookup = driver::locateCeres("", env(a.string() + kSep + b.string()));
	CHECK(!lookup.found());
	CHECK(contains(lookup.error, "--ceres-path"));
	CHECK(contains(lookup.error, "CERES_PATH"));
	CHECK(contains(lookup.error, "PATH"));
	CHECK(contains(lookup.error, "2 directories"));
}

// ---- the real environment ------------------------------------------------------------------------

TEST(ceres_locator, the_process_environment_is_read_from_ceres_path_and_path)
{
#if defined(_WIN32)
	_putenv_s("CERES_PATH", "C:/somewhere/ceres");
#else
	setenv("CERES_PATH", "/somewhere/ceres", 1);
#endif
	driver::CeresEnvironment environment = driver::CeresEnvironment::fromProcess();
	CHECK(environment.ceresPath.has_value());
	CHECK(contains(*environment.ceresPath, "somewhere"));
	CHECK(!environment.path.empty());   // whatever the machine has: tests are launched with a PATH
#if defined(_WIN32)
	_putenv_s("CERES_PATH", "");        // empty removes it on Windows
#else
	unsetenv("CERES_PATH");
#endif
	CHECK(!driver::CeresEnvironment::fromProcess().ceresPath.has_value() ||
		driver::CeresEnvironment::fromProcess().ceresPath->empty());
}
