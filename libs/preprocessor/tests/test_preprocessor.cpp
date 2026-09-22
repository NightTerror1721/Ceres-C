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
		support::DiagnosticPolicy diagnosticPolicy;
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
		result.diagnosticPolicy = std::move(expanded.diagnosticPolicy);
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

TEST(preprocessor, __has_include_reports_whether_a_target_can_be_included)
{
	TempDirectory dir;
	dir.write("present.h", "int x;\n");
	dir.write("include/angled.h", "int y;\n");
	std::string path = dir.write("main.c",
		"#if __has_include(\"present.h\")\nint quotedPresent;\n#endif\n"
		"#if __has_include(\"absent.h\")\nint quotedAbsent;\n#endif\n"
		"#if __has_include(<angled.h>)\nint angledPresent;\n#endif\n"
		"#if __has_include(<nope.h>)\nint angledAbsent;\n#endif\n");

	Result result = expand(path, { (dir.path() / "include").string() });
	CHECK(result.ok);
	CHECK(contains(result.text, "int quotedPresent;"));
	CHECK(!contains(result.text, "int quotedAbsent;"));
	CHECK(contains(result.text, "int angledPresent;"));
	CHECK(!contains(result.text, "int angledAbsent;"));
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

TEST(preprocessor, a_comma_inside_a_literal_does_not_split_a_macro_argument)
{
	// C only separates arguments at a comma outside any string or character literal. Splitting inside
	// one turned `SECTION("length, copy")` into a two-argument call and a hard error.
	TempDirectory dir;
	std::string path = dir.write("main.c",
		"#define ONE(x) [x]\n"
		"#define TWO(a, b) <a|b>\n"
		"char* s = ONE(\"a, b, c\");\n"
		"char c = ONE(',');\n"
		"char* t = TWO(\"x,y\", \"z)\");\n"
		"char* u = ONE(\"quote \\\", still one\");\n");

	Result result = expand(path);
	CHECK(result.ok);
	CHECK(contains(result.text, "char* s = [\"a, b, c\"];"));
	CHECK(contains(result.text, "char c = [','];"));
	CHECK(contains(result.text, "char* t = <\"x,y\"|\"z)\">;"));
	CHECK(contains(result.text, "[\"quote \\\", still one\"]"));
}

TEST(preprocessor, a_block_comment_spanning_lines_hides_directives_and_macro_names)
{
	TempDirectory dir;
	std::string path = dir.write("main.c",
		"#define NAME value\n"
		"/* #include \"missing.h\"\n"
		"   NAME */\n"
		"int x = NAME;\n");

	Result result = expand(path);
	CHECK(result.ok);
	CHECK(contains(result.text, "NAME */"));
	CHECK(contains(result.text, "int x = value;"));
}

TEST(preprocessor, directive_comments_are_not_part_of_operands_or_macro_bodies)
{
	TempDirectory dir;
	dir.write("thing.h", "int from_header;\n");
	std::string path = dir.write("main.c",
		"#include \"thing.h\" // header\n"
		"#define X 1 // value\n"
		"int a = X, b = 2;\n");

	Result result = expand(path);
	CHECK(result.ok);
	CHECK(contains(result.text, "int from_header;"));
	CHECK(contains(result.text, "int a = 1, b = 2;"));
}

TEST(preprocessor, an_unknown_directive_prefix_is_not_misclassified)
{
	TempDirectory dir;
	std::string path = dir.write("main.c", "#include_next <thing.h>\n");
	Result result = expand(path);
	CHECK(!result.ok);
	CHECK(!result.diagnostics.empty());
	CHECK(contains(result.diagnostics.at(0), "include_next"));
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

TEST(preprocessor, conditional_directives_select_the_active_branch)
{
	TempDirectory dir;
	std::string path = dir.write("main.c",
		"#define LEVEL 2\n"
		"#if defined(LEVEL) && LEVEL * 2 == 4\nint selected;\n"
		"#elif 1\nint wrong;\n#else\nint also_wrong;\n#endif\n");
	Result result = expand(path);
	CHECK(result.ok);
	CHECK(contains(result.text, "int selected;"));
	CHECK(!contains(result.text, "int wrong;"));
	CHECK(!contains(result.text, "int also_wrong;"));
}

TEST(preprocessor, function_like_and_variadic_macros_substitute_arguments)
{
	TempDirectory dir;
	std::string path = dir.write("main.c",
		"#define MAX(a, b) ((a) > (b) ? (a) : (b))\n"
		"#define CALL(f, ...) f(__VA_ARGS__)\n"
		"int x = MAX(2, 3);\nCALL(print, x, 4);\n");
	Result result = expand(path);
	CHECK(result.ok);
	CHECK(contains(result.text, "int x = ((2) > (3) ? (2) : (3));"));
	CHECK(contains(result.text, "print(x, 4);"));
}

TEST(preprocessor, stringification_turns_an_argument_into_a_string_literal)
{
	TempDirectory dir;
	std::string path = dir.write("main.c",
		"#define STR(x) #x\n"
		"const char* a = STR(hello);\n"
		"const char* b = STR(two  words);\n");

	Result result = expand(path);
	CHECK(result.ok);
	CHECK(contains(result.text, "const char* a = \"hello\";"));
	// Leading, trailing and internal whitespace collapse to at most one space.
	CHECK(contains(result.text, "const char* b = \"two words\";"));
}

TEST(preprocessor, stringification_escapes_the_two_characters_a_literal_cannot_hold)
{
	TempDirectory dir;
	std::string path = dir.write("main.c",
		"#define STR(x) #x\n"
		"const char* a = STR(a\\b);\n"
		"const char* b = STR(a\"b);\n");

	Result result = expand(path);
	CHECK(result.ok);
	CHECK(contains(result.text, "\"a\\\\b\""));
	CHECK(contains(result.text, "\"a\\\"b\""));
}

TEST(preprocessor, token_pasting_joins_two_arguments_into_one_token)
{
	TempDirectory dir;
	std::string path = dir.write("main.c",
		"#define CAT(a, b) a ## b\n"
		"int CAT(foo, bar);\n"
		"int CAT(x, 42);\n");

	Result result = expand(path);
	CHECK(result.ok);
	CHECK(contains(result.text, "int foobar;"));
	CHECK(contains(result.text, "int x42;"));
}

TEST(preprocessor, token_pasting_puts_a_fresh_number_on_a_name)
{
	// The whole reason `##` matters here: without it there is no way to build a name from
	// __COUNTER__. The indirection through CAT is what lets __COUNTER__ expand to a number
	// BEFORE the paste - `p ## __COUNTER__` directly would paste it onto the name first.
	TempDirectory dir;
	std::string path = dir.write("main.c",
		"#define CAT_(a, b) a ## b\n"
		"#define CAT(a, b) CAT_(a, b)\n"
		"#define UNIQUE(p) CAT(p, __COUNTER__)\n"
		"int UNIQUE(buf);\n"
		"int UNIQUE(buf);\n");

	Result result = expand(path);
	CHECK(result.ok);
	CHECK(contains(result.text, "int buf0;"));
	CHECK(contains(result.text, "int buf1;"));
}

TEST(preprocessor, token_pasting_works_in_an_object_like_macro)
{
	TempDirectory dir;
	std::string path = dir.write("main.c",
		"#define GLUE a ## b\n"
		"int GLUE;\n");

	Result result = expand(path);
	CHECK(result.ok);
	CHECK(contains(result.text, "int ab;"));
}

TEST(preprocessor, a_macro_body_may_span_lines_with_a_continuation_backslash)
{
	// The feature this whole suite grew for: a backslash immediately before the newline splices the
	// next physical line onto this one, so a #define can be laid out over more than one line.
	TempDirectory dir;
	std::string path = dir.write("main.c",
		"#define SUM(a, b) \\\n"
		"    ((a) + (b))\n"
		"int x = SUM(2, 3);\n");

	Result result = expand(path);
	CHECK(result.ok);
	CHECK(contains(result.text, "int x = ((2) + (3));"));
}

TEST(preprocessor, an_object_like_macro_body_may_span_lines)
{
	TempDirectory dir;
	std::string path = dir.write("main.c",
		"#define VALUE \\\n"
		"    42\n"
		"int x = VALUE;\n");

	Result result = expand(path);
	CHECK(result.ok);
	CHECK(contains(result.text, "int x = 42;"));
}

TEST(preprocessor, a_backslash_newline_splices_ordinary_code_lines)
{
	// Line continuation is not a macro feature - it is phase 2 of translation and applies to every
	// line, so two halves of a statement may sit on separate physical lines.
	TempDirectory dir;
	std::string path = dir.write("main.c", "int x = 1 + \\\n    2;\n");

	Result result = expand(path);
	CHECK(result.ok);
	CHECK_EQ(countOf(result.text, "\n"), usize{ 1 });
	CHECK(contains(result.text, "int x = 1 + "));
	CHECK(contains(result.text, "2;"));
	CHECK(!contains(result.text, "\\"));
}

TEST(preprocessor, a_directive_may_span_lines)
{
	// The splice happens before a line is recognised as a directive, so #if works across lines too.
	TempDirectory dir;
	std::string path = dir.write("main.c",
		"#define N 4\n"
		"#if N == \\\n"
		"    4\n"
		"int selected;\n"
		"#endif\n");

	Result result = expand(path);
	CHECK(result.ok);
	CHECK(contains(result.text, "int selected;"));
}

TEST(preprocessor, a_continuation_backslash_survives_crlf_line_endings)
{
	// CRLF puts a \r between the backslash and the newline; the splice has to look past it.
	TempDirectory dir;
	std::string path = dir.write("main.c",
		"#define VALUE \\\r\n"
		"    42\r\n"
		"int x = VALUE;\r\n");

	Result result = expand(path);
	CHECK(result.ok);
	CHECK(contains(result.text, "int x = 42;"));
}

TEST(preprocessor, error_and_warning_are_reported_only_in_active_branches)
{
	TempDirectory dir;
	std::string path = dir.write("main.c", "#if 0\n#error hidden\n#endif\n#warning visible\n#error visible failure\n");
	Result result = expand(path);
	CHECK(!result.ok);
	CHECK_EQ(result.diagnostics.size(), usize(2));
	CHECK(contains(result.diagnostics[0], "visible"));
	CHECK(contains(result.diagnostics[1], "visible failure"));
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

// ---- predefined macros -------------------------------------------------------------------------

TEST(preprocessor, line_and_file_name_the_original_file_rather_than_the_expansion)
{
	TempDirectory dir;
	dir.write("two.h", "int a = __LINE__;\nint b = __LINE__;\n");
	std::string path = dir.write("main.c", "#include \"two.h\"\nint c = __LINE__;\n");

	Result result = expand(path);
	CHECK(result.ok);
	CHECK(contains(result.text, "int a = 1;"));
	CHECK(contains(result.text, "int b = 2;"));
	// Line 2 of main.c, even though it is line 3 of the expanded buffer - which is the whole point.
	CHECK(contains(result.text, "int c = 2;"));
}

TEST(preprocessor, file_is_the_file_the_line_was_written_in_not_the_one_that_included_it)
{
	TempDirectory dir;
	dir.write("named.h", "const char* inHeader = __FILE__;\nint level = __INCLUDE_LEVEL__;\n");
	std::string path = dir.write("main.c",
		"#include \"named.h\"\nconst char* inMain = __FILE__;\nconst char* base = __BASE_FILE__;\nint depth = __INCLUDE_LEVEL__;\n");

	Result result = expand(path);
	CHECK(result.ok);
	CHECK(contains(result.text, "named.h\""));
	CHECK(contains(result.text, "main.c\""));
	CHECK(contains(result.text, "int level = 1;"));
	CHECK(contains(result.text, "int depth = 0;"));
	// __BASE_FILE__ is the .c whatever file asks, which is what makes it different from __FILE__.
	CHECK(countOf(result.text, "main.c\"") == 2);
}

TEST(preprocessor, date_and_time_are_string_literals_of_the_right_shape)
{
	TempDirectory dir;
	std::string path = dir.write("main.c", "const char* d = __DATE__;\nconst char* t = __TIME__;\n");

	Result result = expand(path);
	CHECK(result.ok);

	// "Mmm dd yyyy" and "hh:mm:ss" - the shapes C names, checked by length and punctuation rather
	// than by content, which changes every second this suite runs.
	usize date = result.text.find("const char* d = \"");
	CHECK(date != std::string::npos);
	CHECK_EQ(result.text.substr(date + 17, 11).size(), usize(11));
	CHECK_EQ(result.text[date + 17 + 11], '"');

	usize time = result.text.find("const char* t = \"");
	CHECK(time != std::string::npos);
	CHECK_EQ(result.text[time + 17 + 2], ':');
	CHECK_EQ(result.text[time + 17 + 5], ':');
	CHECK_EQ(result.text[time + 17 + 8], '"');
}

TEST(preprocessor, counter_hands_out_a_new_number_every_time)
{
	TempDirectory dir;
	std::string path = dir.write("main.c", "int a = __COUNTER__;\nint b = __COUNTER__;\nint c = __COUNTER__;\n");

	Result result = expand(path);
	CHECK(result.ok);
	CHECK(contains(result.text, "int a = 0;"));
	CHECK(contains(result.text, "int b = 1;"));
	CHECK(contains(result.text, "int c = 2;"));
}

TEST(preprocessor, the_predefined_macros_are_ordinary_entries_in_the_macro_table)
{
	TempDirectory dir;
	std::string path = dir.write("main.c",
		"#if __STDC__ == 1 && __STDC_HOSTED__ == 0 && defined(__CERESC__)\n"
		"int conforming;\n"
		"#endif\n"
		"#ifdef __LINE__\n"
		"int hasLine;\n"
		"#endif\n"
		"#undef __FILE__\n"
		"#ifndef __FILE__\n"
		"int fileIsGone;\n"
		"#endif\n");

	Result result = expand(path);
	CHECK(result.ok);
	CHECK(contains(result.text, "int conforming;"));
	CHECK(contains(result.text, "int hasLine;"));
	CHECK(contains(result.text, "int fileIsGone;"));
}

TEST(preprocessor, a_command_line_define_goes_in_on_top_of_a_predefined_macro)
{
	// Not an error, and not ignored: naming one on the command line is the program's own decision.
	TempDirectory dir;
	std::string path = dir.write("main.c", "int hosted = __STDC_HOSTED__;\n");

	Result result = expand(path, {}, { { "__STDC_HOSTED__", "1" } });
	CHECK(result.ok);
	CHECK(contains(result.text, "int hosted = 1;"));
}

TEST(preprocessor, a_predefined_macro_is_not_substituted_inside_a_string_or_a_comment)
{
	TempDirectory dir;
	std::string path = dir.write("main.c", "const char* s = \"__LINE__\"; // __FILE__\n");

	Result result = expand(path);
	CHECK(result.ok);
	CHECK(contains(result.text, "\"__LINE__\""));
	CHECK(contains(result.text, "// __FILE__"));
}

// ---- #pragma warning ---------------------------------------------------------------------------

TEST(preprocessor, a_warning_pragma_records_what_it_asked_for_against_the_line_it_governs_from)
{
	TempDirectory dir;
	std::string path = dir.write("main.c",
		"int before;\n"
		"#pragma warning(disable: 2001)\n"
		"int after;\n");

	Result result = expand(path);
	CHECK(result.ok);
	CHECK(result.diagnostics.empty()); // a pragma it understands says nothing

	const support::DiagnosticPolicy& policy = result.diagnosticPolicy;
	CHECK(policy.actionFor(support::DiagnosticId::CappedTypeWidth, 1) == support::DiagnosticAction::Default);
	CHECK(policy.actionFor(support::DiagnosticId::CappedTypeWidth, 3) == support::DiagnosticAction::Ignored);

	// And it still leaves its blank line behind, so nothing below it moved.
	CHECK_EQ(countOf(result.text, "\n"), usize{ 3 });
}

TEST(preprocessor, every_verb_the_warning_pragma_takes)
{
	TempDirectory dir;
	std::string path = dir.write("main.c",
		"#pragma warning(disable: 2001)\n"
		"#pragma warning(error: 2001)\n"
		"#pragma warning(enable: 2001)\n"
		"#pragma warning(default: 2001)\n");

	Result result = expand(path);
	CHECK(result.ok);
	CHECK(result.diagnostics.empty());

	const support::DiagnosticPolicy& policy = result.diagnosticPolicy;
	using support::DiagnosticAction;
	CHECK(policy.actionFor(support::DiagnosticId::CappedTypeWidth, 1) == DiagnosticAction::Ignored);
	CHECK(policy.actionFor(support::DiagnosticId::CappedTypeWidth, 2) == DiagnosticAction::Error);
	CHECK(policy.actionFor(support::DiagnosticId::CappedTypeWidth, 3) == DiagnosticAction::Warning);
	CHECK(policy.actionFor(support::DiagnosticId::CappedTypeWidth, 4) == DiagnosticAction::Default);
}

TEST(preprocessor, a_warning_pragma_takes_a_list_and_a_wildcard_and_the_printed_spelling)
{
	TempDirectory dir;
	std::string path = dir.write("main.c",
		"#pragma warning(disable: 2001, 3001)\n"
		"#pragma warning(default: W2001)\n"
		"#pragma warning(error: all)\n");

	Result result = expand(path);
	CHECK(result.ok);
	CHECK(result.diagnostics.empty());

	const support::DiagnosticPolicy& policy = result.diagnosticPolicy;
	using support::DiagnosticAction;
	// Line 1 turned both off; line 2 named one of them again, by the code as it PRINTS.
	CHECK(policy.actionFor(support::DiagnosticId::CappedTypeWidth, 2) == DiagnosticAction::Default);
	CHECK(policy.actionFor(support::DiagnosticId::ConstWithoutInitializer, 2) == DiagnosticAction::Ignored);
	// And `all` catches both, whatever either was.
	CHECK(policy.actionFor(support::DiagnosticId::CappedTypeWidth, 3) == DiagnosticAction::Error);
	CHECK(policy.actionFor(support::DiagnosticId::ConstWithoutInitializer, 3) == DiagnosticAction::Error);
}

TEST(preprocessor, push_and_pop_put_back_exactly_what_was_in_force)
{
	TempDirectory dir;
	std::string path = dir.write("main.c",
		"#pragma warning(disable: 2001)\n"
		"#pragma warning(push)\n"
		"#pragma warning(error: 2001)\n"
		"#pragma warning(pop)\n"
		"int after;\n");

	Result result = expand(path);
	CHECK(result.ok);
	CHECK(result.diagnostics.empty());

	using support::DiagnosticAction;
	CHECK(result.diagnosticPolicy.actionFor(support::DiagnosticId::CappedTypeWidth, 3) == DiagnosticAction::Error);
	CHECK(result.diagnosticPolicy.actionFor(support::DiagnosticId::CappedTypeWidth, 5) == DiagnosticAction::Ignored);
}

TEST(preprocessor, a_warning_pragma_it_cannot_read_is_a_warning_rather_than_an_error)
{
	// A pragma is advice. Not understanding a piece of it must not stop a program compiling - which
	// is the same rule `#pragma once` has always followed for an unknown pragma.
	TempDirectory dir;
	std::string path = dir.write("main.c",
		"#pragma warning disable: 2001\n"
		"#pragma warning(bogus: 2001)\n"
		"#pragma warning(disable)\n"
		"#pragma warning(disable: nonsense)\n"
		"#pragma warning(pop)\n");

	Result result = expand(path);
	CHECK(result.ok); // every one of them is survivable
	CHECK_EQ(result.diagnostics.size(), usize{ 5 });
	CHECK(contains(result.diagnostics[0], "parenthesized body"));
	CHECK(contains(result.diagnostics[1], "expected 'disable'"));
	CHECK(contains(result.diagnostics[2], "needs a ':'"));
	CHECK(contains(result.diagnostics[3], "expected a warning number or 'all'"));
	CHECK(contains(result.diagnostics[4], "no matching '#pragma warning(push)'"));
}

TEST(preprocessor, a_number_that_is_not_a_warning_is_named_as_such)
{
	TempDirectory dir;
	std::string path = dir.write("main.c",
		"#pragma warning(disable: 3023)\n"   // E3023 - an error
		"#pragma warning(disable: 9999)\n"); // nothing at all

	Result result = expand(path);
	CHECK(result.ok);
	CHECK_EQ(result.diagnostics.size(), usize{ 2 });
	CHECK(contains(result.diagnostics[0], "does not name a warning"));
	CHECK(contains(result.diagnostics[1], "does not name a warning"));
}

TEST(preprocessor, the_pragma_governs_the_preprocessors_own_warnings_too)
{
	// The one phase that cannot leave this to the DiagnosticEngine, because its diagnostics point
	// at the original file rather than at the expanded text the policy is indexed by.
	TempDirectory dir;
	std::string loud = dir.write("loud.c", "#pragma frobnicate\n");
	CHECK_EQ(expand(loud).diagnostics.size(), usize{ 1 });

	std::string quiet = dir.write("quiet.c",
		"#pragma warning(disable: 1004)\n"
		"#pragma frobnicate\n");
	CHECK(expand(quiet).diagnostics.empty());
}

TEST(preprocessor, a_pragma_in_a_header_governs_the_file_that_included_it)
{
	// Nothing scopes a pragma to the file it was written in - the policy is one list over the whole
	// expanded text, exactly as in C. `push`/`pop` is what a header that means to be polite uses.
	TempDirectory dir;
	dir.write("quiet.h", "#pragma warning(disable: 2001)\n");
	std::string path = dir.write("main.c", "#include \"quiet.h\"\nint after;\n");

	Result result = expand(path);
	CHECK(result.ok);
	CHECK(result.diagnosticPolicy.actionFor(support::DiagnosticId::CappedTypeWidth, 2)
		== support::DiagnosticAction::Ignored);
}
