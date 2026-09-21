#include <ceresc/driver/driver.h>
#include <ceresc/driver/options.h>

#include "ceres_tool.h"
#include "framework.h"

#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

// Code that was built before: a `.cobj` object and a `.car` archive on the command line, linked as they are,
// with `--decls` saying what they define. Everything here runs the real `ceres` (assemble, archive, link, run),
// so it needs the same sibling CeresASM checkout the rest of this suite does and SKIPS without one.
//
// `ceres run` exits with what `main` returned, and driver::run passes that through, so a program's
// result is asserted as its exit status.

namespace
{
	namespace fs = std::filesystem;
	using namespace ceresc::testing;

	struct Scratch
	{
		fs::path dir;
		explicit Scratch(const std::string& name)
		{
			dir = fs::temp_directory_path() / "ceresc_prebuilt" / name;
			std::error_code error;
			fs::remove_all(dir, error);
			fs::create_directories(dir);
		}
		~Scratch() { std::error_code error; fs::remove_all(dir, error); }

		fs::path write(const std::string& file, const std::string& text) const
		{
			std::ofstream out(dir / file, std::ios::binary);
			out << text;
			return dir / file;
		}
	};

	// Compiles `file` alone, publishes its declarations, and assembles it to an object: what a library build
	// does, in three steps. Returns the object and the declarations, or nothing if a step failed.
	struct Built { fs::path object; fs::path decls; };

	std::optional<Built> buildObject(const Scratch& scratch, const fs::path& ceresDir, const std::string& name, const std::string& source)
	{
		fs::path c = scratch.write(name + ".c", source);
		ceresc::driver::Options options;
		options.inputPaths = { c.string() };
		options.outputPath = (scratch.dir / (name + ".casm")).string();
		options.emitDeclsPath = (scratch.dir / (name + ".decls.casm")).string();
		if (ceresc::driver::run(options) != 0)
			return std::nullopt;

		fs::path object = scratch.dir / (name + ".cobj");
		fs::path ceres = ceresDir / kCeresExecutableName;
		const std::string command = std::format("{} asm -c {} -o {}", quote(ceres), quote(scratch.dir / (name + ".casm")), quote(object));
		if (runSubprocessCapturingStdout(command, scratch.dir / "asm.log") != 0)
			return std::nullopt;
		return Built{ object, scratch.dir / (name + ".decls.casm") };
	}

	std::optional<fs::path> buildArchive(const Scratch& scratch, const fs::path& ceresDir, const std::string& name, const std::vector<fs::path>& objects)
	{
		fs::path archive = scratch.dir / (name + ".car");
		std::string command = std::format("{} ar {}", quote(ceresDir / kCeresExecutableName), quote(archive));
		for (const fs::path& object : objects)
			command += " " + quote(object);
		if (runSubprocessCapturingStdout(command, scratch.dir / "ar.log") != 0)
			return std::nullopt;
		return archive;
	}

	// Compiles main.c against what was built, links and runs it; returns driver::run's result: the program's
	// exit status.
	int runProgram(const Scratch& scratch, const fs::path& ceresDir, const std::string& source,
		const std::vector<fs::path>& prebuilt, const std::vector<fs::path>& decls, bool clean = false)
	{
		ceresc::driver::Options options;
		options.inputPaths.push_back(scratch.write("main.c", source).string());
		for (const fs::path& p : prebuilt)
			options.inputPaths.push_back(p.string());
		for (const fs::path& d : decls)
			options.declsFiles.push_back(d.string());
		options.outputPath = (scratch.dir / "main.cres").string();
		options.run = true;
		options.clean = clean;
		options.ceresPath = ceresDir.string();
		return ceresc::driver::run(options);
	}

	const char* kLibrary =
		"int counter = 5;\n"
		"int twice(int x) { return x * 2; }\n";
	const char* kMain =
		"extern int counter;\n"
		"extern int twice(int x);\n"
		"int main(void) { return twice(counter); }\n";

	bool skipped(const std::optional<fs::path>& ceresDir)
	{
		if (ceresDir)
			return false;
		std::printf("  (skipped: no sibling CeresASM checkout found - set CERESC_CERES_PATH)\n");
		return true;
	}
}

TEST(prebuilt, a_program_links_against_an_archive_built_earlier)
{
	std::optional<fs::path> ceresDir = findCeresDirectory();
	if (skipped(ceresDir)) return;
	Scratch scratch("archive");
	std::optional<Built> lib = buildObject(scratch, *ceresDir, "lib", kLibrary);
	CHECK(lib.has_value());
	if (!lib) return;
	std::optional<fs::path> archive = buildArchive(scratch, *ceresDir, "lib", { lib->object });
	CHECK(archive.has_value());
	if (!archive) return;

	CHECK_EQ(runProgram(scratch, *ceresDir, kMain, { *archive }, { lib->decls }), 10);   // twice(counter) = 10
}

TEST(prebuilt, an_object_is_linked_as_it_is)
{
	std::optional<fs::path> ceresDir = findCeresDirectory();
	if (skipped(ceresDir)) return;
	Scratch scratch("object");
	std::optional<Built> lib = buildObject(scratch, *ceresDir, "lib", kLibrary);
	CHECK(lib.has_value());
	if (!lib) return;

	CHECK_EQ(runProgram(scratch, *ceresDir, kMain, { lib->object }, { lib->decls }), 10);
}

TEST(prebuilt, without_the_declarations_the_assembler_cannot_name_what_the_object_defines)
{
	std::optional<fs::path> ceresDir = findCeresDirectory();
	if (skipped(ceresDir)) return;
	Scratch scratch("nodecls");
	std::optional<Built> lib = buildObject(scratch, *ceresDir, "lib", kLibrary);
	CHECK(lib.has_value());
	if (!lib) return;

	CHECK(runProgram(scratch, *ceresDir, kMain, { lib->object }, {}) != 10);   // the unit never saw `twice` declared
}

TEST(prebuilt, an_archive_member_nothing_asks_for_is_not_linked)
{
	// Two members. The second defines `extra`, which main defines as well: had it been pulled in the link would
	// report a duplicate symbol. The first is needed, so it is.
	std::optional<fs::path> ceresDir = findCeresDirectory();
	if (skipped(ceresDir)) return;
	Scratch scratch("members");
	std::optional<Built> used = buildObject(scratch, *ceresDir, "used", kLibrary);
	std::optional<Built> spare = buildObject(scratch, *ceresDir, "spare", "int extra(void) { return 1; }\n");
	CHECK(used.has_value());
	CHECK(spare.has_value());
	if (!used || !spare) return;
	std::optional<fs::path> archive = buildArchive(scratch, *ceresDir, "lib", { used->object, spare->object });
	CHECK(archive.has_value());
	if (!archive) return;

	const std::string mainSource =
		"extern int counter;\n"
		"extern int twice(int x);\n"
		"int extra(void) { return 100; }\n"
		"int main(void) { return twice(counter) + extra(); }\n";
	CHECK_EQ(runProgram(scratch, *ceresDir, mainSource, { *archive }, { used->decls, spare->decls }), 110);
}

TEST(prebuilt, clean_removes_what_the_build_made_and_leaves_what_was_given)
{
	std::optional<fs::path> ceresDir = findCeresDirectory();
	if (skipped(ceresDir)) return;
	Scratch scratch("clean");
	std::optional<Built> lib = buildObject(scratch, *ceresDir, "lib", kLibrary);
	CHECK(lib.has_value());
	if (!lib) return;
	std::optional<fs::path> archive = buildArchive(scratch, *ceresDir, "lib", { lib->object });
	CHECK(archive.has_value());
	if (!archive) return;

	// --clean only acts after a run that succeeded, so this main returns 0 when the arithmetic came out right
	const std::string okMain =
		"extern int counter;\n"
		"extern int twice(int x);\n"
		"int main(void) { return twice(counter) != 10; }\n";
	CHECK_EQ(runProgram(scratch, *ceresDir, okMain, { *archive, lib->object }, { lib->decls }, true), 0);
	CHECK(fs::exists(*archive));
	CHECK(fs::exists(lib->object));
	CHECK(fs::exists(lib->decls));
	CHECK(!fs::exists(scratch.dir / "main.cobj"));   // made by this build
	CHECK(!fs::exists(scratch.dir / "main.cres"));
}

TEST(prebuilt, an_object_only_program_needs_no_c_at_all)
{
	// main comes from a .cobj too: there is nothing to compile, and --run links and runs what was given.
	std::optional<fs::path> ceresDir = findCeresDirectory();
	if (skipped(ceresDir)) return;
	Scratch scratch("only");
	std::optional<Built> program = buildObject(scratch, *ceresDir, "prog", "int main(void) { return 7; }\n");
	CHECK(program.has_value());
	if (!program) return;

	ceresc::driver::Options options;
	options.inputPaths = { program->object.string() };
	options.outputPath = (scratch.dir / "prog.cres").string();
	options.run = true;
	options.ceresPath = ceresDir->string();
	CHECK_EQ(ceresc::driver::run(options), 7);
}

TEST(prebuilt, a_missing_object_or_declarations_file_is_named_before_anything_is_compiled)
{
	Scratch scratch("missing");
	ceresc::driver::Options options;
	options.inputPaths = { scratch.write("main.c", kMain).string(), (scratch.dir / "nope.car").string() };
	options.run = false;
	CHECK_EQ(ceresc::driver::run(options), 1);
	CHECK(!fs::exists(scratch.dir / "main.casm"));   // nothing was compiled

	options.inputPaths = { (scratch.dir / "main.c").string() };
	options.declsFiles = { (scratch.dir / "nope.decls.casm").string() };
	CHECK_EQ(ceresc::driver::run(options), 1);
	CHECK(!fs::exists(scratch.dir / "main.casm"));
}

TEST(prebuilt, assembling_a_source_beside_a_given_object_of_the_same_name_would_overwrite_it_and_is_refused)
{
	std::optional<fs::path> ceresDir = findCeresDirectory();
	if (skipped(ceresDir)) return;
	Scratch scratch("clash");
	std::optional<Built> lib = buildObject(scratch, *ceresDir, "util", "int util(void) { return 1; }\n");
	CHECK(lib.has_value());
	if (!lib) return;
	scratch.write("util.casm", "@text\nglobal helper:\n    ret\n");   // would be assembled to util.cobj: the given one

	ceresc::driver::Options options;
	options.inputPaths = { scratch.write("main.c", "int main(void) { return 0; }\n").string(),
		(scratch.dir / "util.casm").string(), lib->object.string() };
	options.run = true;
	options.ceresPath = ceresDir->string();
	CHECK_EQ(ceresc::driver::run(options), 1);
}

TEST(prebuilt, the_declarations_file_a_build_writes_for_itself_cannot_be_one_the_person_handed_in)
{
	std::optional<fs::path> ceresDir = findCeresDirectory();
	if (skipped(ceresDir)) return;
	Scratch scratch("selfdecls");
	scratch.write("a.c", "int a(void) { return 1; }\n");
	scratch.write("b.c", "int b(void) { return 2; }\n");
	fs::path theirs = scratch.write("out.decls.casm", "@text\n");

	ceresc::driver::Options options;
	options.inputPaths = { (scratch.dir / "a.c").string(), (scratch.dir / "b.c").string() };
	options.outputPath = (scratch.dir / "out.cres").string();   // two units: a shared out.decls.casm is written
	options.declsFiles = { theirs.string() };
	CHECK_EQ(ceresc::driver::run(options), 1);
}
