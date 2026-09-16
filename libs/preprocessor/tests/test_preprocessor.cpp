#include <ceresc/preprocessor/preprocessor.h>
#include <ceresc/support/diagnostics.h>
#include <ceresc/support/source_manager.h>

#include "framework.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

// The preprocessor's own suite. Everything here writes real files into a temporary directory and
// runs the real expansion over them: #include resolution is half the feature, and a fixture that
// handed the class a string instead of a path would test the other half twice.

using namespace ceresc;

namespace
{
	namespace fs = std::filesystem;

	// One throwaway directory per test, cleaned up on the way out - so a failing test leaves nothing
	// behind for the next one to trip over, and two tests can use the same file names.
	class TempDirectory
	{
	public:
		TempDirectory()
		{
			static int counter = 0;
			_path = fs::temp_directory_path() / std::format("ceresc_pp_{}", ++counter);
			std::error_code error;
			fs::remove_all(_path, error);
			fs::create_directories(_path, error);
		}
		~TempDirectory()
		{
			std::error_code error;
			fs::remove_all(_path, error);
		}
		TempDirectory(const TempDirectory&) = delete;
		TempDirectory& operator=(const TempDirectory&) = delete;

		const fs::path& path() const noexcept { return _path; }

		std::string write(std::string_view name, std::string_view contents) const
		{
			fs::path file = _path / fs::path(std::string(name));
			fs::create_directories(file.parent_path());
			std::ofstream out(file, std::ios::binary);
			out << contents;
			return file.string();
		}

	private:
		fs::path _path;
	};

	struct Result
	{
		std::string text;
		std::vector<std::string> diagnostics;
		preprocessor::LineMap lineMap;
		bool ok = false;
	};

	Result expand(const std::string& path, const std::vector<std::string>& includeDirectories = {},
		const std::vector<std::pair<std::string, std::string>>& defines = {})
	{
		support::SourceManager sourceManager;
		support::DiagnosticEngine diagnostics;
		preprocessor::Preprocessor preprocessor(sourceManager, diagnostics);
		for (const std::string& directory : includeDirectories)
			preprocessor.addIncludeDirectory(directory);
		for (const auto& [name, value] : defines)
			preprocessor.define(name, value);

		preprocessor::PreprocessedSource expanded = preprocessor.run(path);

		Result result;
		result.text = std::move(expanded.text);
		result.lineMap = std::move(expanded.lineMap);
		result.ok = expanded.ok;
		for (const support::Diagnostic& diagnostic : diagnostics.diagnostics())
			result.diagnostics.push_back(diagnostic.message);
		return result;
	}

	bool contains(std::string_view haystack, std::string_view needle)
	{
		return haystack.find(needle) != std::string_view::npos;
	}

	usize countOf(std::string_view haystack, std::string_view needle)
	{
		usize count = 0;
		for (usize at = haystack.find(needle); at != std::string_view::npos; at = haystack.find(needle, at + 1))
			++count;
		return count;
	}
}

TEST(preprocessor, a_file_with_no_directives_comes_out_unchanged)
{
	TempDirectory dir;
	std::string path = dir.write("main.c", "int main(void)\n{\n    return 0;\n}\n");
	Result result = expand(path);
	CHECK(result.ok);
	CHECK_EQ(result.text, std::string("int main(void)\n{\n    return 0;\n}\n"));
}

TEST(preprocessor, an_include_is_replaced_by_the_files_contents)
{
	TempDirectory dir;
	dir.write("thing.h", "int declared;\n");
	std::string path = dir.write("main.c", "#include \"thing.h\"\nint main(void) { return 0; }\n");

	Result result = expand(path);
	CHECK(result.ok);
	CHECK(contains(result.text, "int declared;"));
	CHECK(contains(result.text, "int main(void)"));
}

TEST(preprocessor, a_quoted_include_looks_next_to_the_including_file_first)
{
	// The whole reason a project whose headers sit beside its sources needs no -I at all.
	TempDirectory dir;
	dir.write("sub/helper.h", "int fromSubdirectory;\n");
	std::string path = dir.write("sub/main.c", "#include \"helper.h\"\n");

	Result result = expand(path);
	CHECK(result.ok);
	CHECK(contains(result.text, "int fromSubdirectory;"));
}

TEST(preprocessor, an_angled_include_uses_the_search_path_only)
{
	TempDirectory dir;
	dir.write("include/lib.h", "int fromSearchPath;\n");
	dir.write("lib.h", "int fromBesideTheSource;\n");
	std::string path = dir.write("main.c", "#include <lib.h>\n");

	Result result = expand(path, { (dir.path() / "include").string() });
	CHECK(result.ok);
	CHECK(contains(result.text, "int fromSearchPath;"));
	CHECK(!contains(result.text, "int fromBesideTheSource;"));
}

TEST(preprocessor, a_missing_include_is_reported_by_name)
{
	TempDirectory dir;
	std::string path = dir.write("main.c", "#include \"nowhere.h\"\n");
	Result result = expand(path);
	CHECK(!result.ok);
	CHECK_EQ(result.diagnostics.size(), usize(1));
	CHECK(contains(result.diagnostics.front(), "nowhere.h"));
}

TEST(preprocessor, pragma_once_stops_a_second_expansion)
{
	// Without #ifndef there is no other way to write an include guard, which is why this is here at
	// all - see the header's own note.
	TempDirectory dir;
	dir.write("guarded.h", "#pragma once\nint onlyOnce;\n");
	std::string path = dir.write("main.c", "#include \"guarded.h\"\n#include \"guarded.h\"\n");

	Result result = expand(path);
	CHECK(result.ok);
	CHECK_EQ(countOf(result.text, "int onlyOnce;"), usize(1));
}

TEST(preprocessor, a_header_with_no_pragma_once_really_is_expanded_twice)
{
	// The other half of the rule above: nothing is de-duplicated on its own, so the guard is doing
	// the work rather than some hidden bookkeeping.
	TempDirectory dir;
	dir.write("plain.h", "int twice;\n");
	std::string path = dir.write("main.c", "#include \"plain.h\"\n#include \"plain.h\"\n");

	Result result = expand(path);
	CHECK(result.ok);
	CHECK_EQ(countOf(result.text, "int twice;"), usize(2));
}

TEST(preprocessor, an_include_cycle_is_reported_instead_of_recursing_forever)
{
	TempDirectory dir;
	dir.write("a.h", "#include \"b.h\"\n");
	dir.write("b.h", "#include \"a.h\"\n");
	std::string path = dir.write("main.c", "#include \"a.h\"\n");

	Result result = expand(path);
	CHECK(!result.ok);
	CHECK(!result.diagnostics.empty());
	CHECK(contains(result.diagnostics.front(), "cycle"));
}

TEST(preprocessor, an_object_like_macro_is_substituted_everywhere_after_its_definition)
{
	TempDirectory dir;
	std::string path = dir.write("main.c",
		"int before = WIDTH;\n"
		"#define WIDTH 320\n"
		"int after = WIDTH;\n");

	Result result = expand(path);
	CHECK(result.ok);
	// Before the #define the name is just a name - substitution is positional, not whole-file.
	CHECK(contains(result.text, "int before = WIDTH;"));
	CHECK(contains(result.text, "int after = 320;"));
}

TEST(preprocessor, a_macro_only_replaces_whole_identifiers)
{
	TempDirectory dir;
	std::string path = dir.write("main.c",
		"#define N 4\n"
		"int N;\n"
		"int Nx;\n"
		"int xN;\n"
		"int aNb;\n");

	Result result = expand(path);
	CHECK(contains(result.text, "int 4;"));
	CHECK(contains(result.text, "int Nx;"));
	CHECK(contains(result.text, "int xN;"));
	CHECK(contains(result.text, "int aNb;"));
}

TEST(preprocessor, a_macro_is_not_substituted_inside_a_literal_or_a_comment)
{
	// Getting this wrong is how a preprocessor rewrites the inside of a printed message.
	TempDirectory dir;
	std::string path = dir.write("main.c",
		"#define NAME ceres\n"
		"char* s = \"NAME\";\n"
		"char c = 'N';\n"
		"// NAME in a comment\n"
		"int NAME;\n");

	Result result = expand(path);
	CHECK(contains(result.text, "\"NAME\""));
	CHECK(contains(result.text, "// NAME in a comment"));
	CHECK(contains(result.text, "int ceres;"));
}

TEST(preprocessor, a_macro_may_expand_into_another_one)
{
	TempDirectory dir;
	std::string path = dir.write("main.c",
		"#define INNER 7\n"
		"#define OUTER INNER\n"
		"int x = OUTER;\n");

	Result result = expand(path);
	CHECK(contains(result.text, "int x = 7;"));
}

TEST(preprocessor, a_macro_defined_in_terms_of_itself_stops_instead_of_looping)
{
	TempDirectory dir;
	std::string path = dir.write("main.c",
		"#define LOOP LOOP + 1\n"
		"int x = LOOP;\n");

	Result result = expand(path);
	CHECK(result.ok); // a warning, not an error - the text is still something the lexer can read
	CHECK(!result.diagnostics.empty());
}

TEST(preprocessor, undef_forgets_a_macro)
{
	TempDirectory dir;
	std::string path = dir.write("main.c",
		"#define N 4\n"
		"int a = N;\n"
		"#undef N\n"
		"int b = N;\n");

	Result result = expand(path);
	CHECK(contains(result.text, "int a = 4;"));
	CHECK(contains(result.text, "int b = N;"));
}

TEST(preprocessor, a_predefined_macro_behaves_like_one_written_in_the_file)
{
	TempDirectory dir;
	std::string path = dir.write("main.c", "int x = LEVEL;\n");
	Result result = expand(path, {}, { { "LEVEL", "3" } });
	CHECK(contains(result.text, "int x = 3;"));
}

TEST(preprocessor, an_unsupported_directive_names_itself)
{
	// The point is that it is a KNOWN gap rather than an unreadable line - #if is the one people
	// reach for first, and a generic "syntax error" would hide that this version simply lacks it.
	TempDirectory dir;
	std::string path = dir.write("main.c", "#ifdef DEBUG\nint x;\n#endif\n");

	Result result = expand(path);
	CHECK(!result.ok);
	CHECK(!result.diagnostics.empty());
	CHECK(contains(result.diagnostics.front(), "ifdef"));
}

TEST(preprocessor, a_macro_with_arguments_says_so_rather_than_complaining_about_the_name)
{
	TempDirectory dir;
	std::string path = dir.write("main.c", "#define MAX(a, b) a\n");
	Result result = expand(path);
	CHECK(!result.ok);
	CHECK(!result.diagnostics.empty());
	CHECK(contains(result.diagnostics.front(), "arguments"));
}

TEST(preprocessor, a_file_with_no_includes_maps_every_line_onto_itself)
{
	// The common case, and the one that makes a diagnostic's line number already right before it is
	// mapped: a directive contributes a blank line rather than nothing, so nothing shifts.
	TempDirectory dir;
	std::string path = dir.write("main.c",
		"#define N 1\n"
		"int a;\n"
		"int b;\n");

	Result result = expand(path);
	for (u32 line = 1; line <= 3; ++line)
	{
		support::SourceLocation mapped = result.lineMap.toOriginal(support::SourceLocation{ {}, line, 1, 0 });
		CHECK_EQ(mapped.line, line);
	}
}

TEST(preprocessor, a_line_after_an_include_maps_back_to_its_own_file_and_line)
{
	// The whole reason the line map exists: without it, this error would be reported at line 4 of a
	// two-line file.
	TempDirectory dir;
	dir.write("three.h", "int one;\nint two;\nint three;\n");
	std::string path = dir.write("main.c", "#include \"three.h\"\nint afterwards;\n");

	Result result = expand(path);
	CHECK(result.ok);

	// Output line 1..3 are the header's own; line 4 is main.c's second line.
	support::SourceLocation header = result.lineMap.toOriginal(support::SourceLocation{ {}, 2, 1, 0 });
	CHECK_EQ(header.line, u32(2));

	support::SourceLocation afterwards = result.lineMap.toOriginal(support::SourceLocation{ {}, 4, 1, 0 });
	CHECK_EQ(afterwards.line, u32(2));
	CHECK(header.sourceId != afterwards.sourceId); // and they are two different files
}
