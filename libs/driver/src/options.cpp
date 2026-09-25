#include <ceresc/driver/options.h>

#include <format>
#include <string_view>

namespace ceresc::driver
{
	namespace
	{
		void printOptimizationUsage(std::ostream& out)
		{
			out <<
				"  -O0                 every optimization off - the simplest, most uniform output\n"
				"  -O1                 everything except inlining (the default)\n"
				"  -O2                 everything, inlining included\n"
				"  -O3                 accepted as an alias for -O2\n"
				"  -Os                 like -O1 but for size: no inlining, no jump tables\n"
				"  -Og                 like -O1 but for debugging: no inlining, no frame-slot reuse\n"
				"  -fsoft-double       double is a real 64-bit IEEE double, done in software (ceres/f64.h)\n"
				"  -f<opt>/-fno-<opt>  turn one optimization on/off, overriding -O in argument order:\n";

			for (const support::OptimizationFlag& flag : support::optimizationFlags())
				out << std::format("      {:<20}{}\n", flag.name, flag.description);
		}

		// Applies `-f<name>` / `-fno-<name>` to `options`. Returns false for a name that is not one
		// of support::optimizationFlags()' own - the caller reports it and gives up, rather than
		// silently ignoring a flag the user believes took effect.
		bool applyOptimizationFlag(std::string_view argument, support::OptimizationOptions& options)
		{
			std::string_view name = argument.substr(2); // past "-f"
			bool enable = true;
			if (name.starts_with("no-"))
			{
				enable = false;
				name.remove_prefix(3);
			}

			for (const support::OptimizationFlag& flag : support::optimizationFlags())
			{
				if (flag.name == name)
				{
					options.*(flag.field) = enable;
					return true;
				}
			}
			return false;
		}

		// `NAME` or `NAME=value`, as -D takes it. A bare name defines the macro as `1`, which is
		// what every C compiler does and what makes `-D DEBUG` useful on its own.
		std::pair<std::string, std::string> splitDefine(std::string_view text)
		{
			usize equals = text.find('=');
			if (equals == std::string_view::npos)
				return { std::string(text), "1" };
			return { std::string(text.substr(0, equals)), std::string(text.substr(equals + 1)) };
		}
	}

	void printUsage(std::ostream& out)
	{
		out <<
			"usage: ceresc <file.c|file.casm|file.cobj|file.car>... [-o <output>] [-I <dir>] [-D <name>[=<value>]]\n"
			"               [-L <dir>] [-l <name>] [--sysroot <dir>]\n"
			"               [--emit-ast] [--emit-ir] [-E] [-S | --run] [--clean | --clean-keep-casm]\n"
			"               [--decls <file.casm>]... [--emit-decls <file.casm>]\n"
			"               [--ceres-path <dir|file>] [--run-arg <arg>]... [--symtab] [--gc-sections]\n"
			"               [-Werror] [-O<level>] [-f<opt>] [-- <argument>...]\n"
			"       ceresc --version | --help\n"
			"\n"
			"  <file.c>            a C subset source file to compile\n"
			"  <file.casm>         a CeresASM source to assemble and link alongside the C ones\n"
			"  <file.cobj>         an object built before, linked as it is (with --run)\n"
			"  <file.car>          an archive of objects built before; a member is linked only if something needs it\n"
			"  --decls <file>      declarations of what a .cobj or .car defines (from --emit-decls); every generated\n"
			"                      unit imports it. May be given more than once\n"
			"  --emit-decls <file> write the declarations of what this build defines, so it can be used with --decls\n"
			"  -o <output>         the .casm to write (one C input), or the linked program (several)\n"
			"  -I <dir>            a directory to search for #include <...> and #include \"...\"\n"
			"  -D <name>[=<val>]   predefine an object-like macro (a bare name means 1)\n"
			"  -L <dir>            a directory to search for -l libraries (lib<name>.car or .cobj)\n"
			"  -l <name>           link lib<name>, found through -L and --sysroot/lib; only with --run\n"
			"  --sysroot <dir>     <dir>/include joins the include search, <dir>/lib the library search\n"
			"  --emit-ast          print the annotated AST (s-expression form) and stop\n"
			"  --emit-ir           print the IR and stop\n"
			"  -E                  print the preprocessed source and stop\n"
			"  -S                  stop at the CASM text, do not assemble it (the default)\n"
			"  --run               assemble, link and run the program with `ceres asm`/`ceres run`\n"
			"  --clean             after --run, remove generated .casm, .decls.casm, .cobj and .cres files\n"
			"  --clean-keep-casm   after --run, keep generated .casm but remove .decls.casm, .cobj and .cres\n"
			"  --ceres-path <path> where to find `ceres`: its directory, or the executable itself. Without it the\n"
			"                      CERES_PATH environment variable says, and then PATH\n"
			"  --gc-sections       leave out of the program the functions nothing reaches (`ceres link --gc-sections`)\n"
			"  --symtab            link a table of the program's function names into it (`ceres link --symtab`),\n"
			"                      for a backtrace or a fault report that names functions (ceres/backtrace.h);\n"
			"                      only with --run, the build that links\n"
			"  -Werror             treat warnings as errors\n"
			"  --stats, -fstats    after optimizing, report what changed (instruction counts, inlining)\n"
			"  --version           print the version and stop\n"
			"  --help, -h          print this text\n"
			"\n"
			"-S and --run are opposites and resolve in argument order: the last one written wins.\n"
			"\n"
			"optimization (on by default, -O1):\n";

		printOptimizationUsage(out);
	}

	std::optional<Options> parseOptions(std::span<const char* const> args, std::ostream& diagnosticsOut)
	{
		Options options;

		// Every option that takes a separate value accepts both spellings - `-I dir` and `-Idir`,
		// `-D NAME` and `-DNAME`, `-o out` and (for symmetry) nothing joined, since `-oout` is not a
		// spelling anyone uses. Reading a value that is not there is an error rather than a silently
		// ignored flag.
		auto valueFor = [&](std::string_view arg, std::string_view flag, usize& i) -> std::optional<std::string>
		{
			if (arg.size() > flag.size())
				return std::string(arg.substr(flag.size()));
			if (i + 1 >= args.size())
			{
				diagnosticsOut << "ceresc: '" << flag << "' requires an argument\n";
				return std::nullopt;
			}
			return std::string(args[++i]);
		};

		for (usize i = 0; i < args.size(); ++i)
		{
			std::string_view arg = args[i];

			if (arg == "--emit-ast") { options.emitAst = true; continue; }
			if (arg == "--emit-ir") { options.emitIr = true; continue; }
			if (arg == "-E") { options.emitPreprocessed = true; continue; }
			// Opposites, resolved in argument order rather than by precedence - see Options::run.
			if (arg == "-S") { options.run = false; continue; }
			if (arg == "--run") { options.run = true; continue; }
			if (arg == "--clean") { options.clean = true; options.cleanKeepCasm = false; continue; }
			if (arg == "--clean-keep-casm") { options.clean = false; options.cleanKeepCasm = true; continue; }
			if (arg == "-Werror") { options.warningsAsErrors = true; continue; }
			if (arg == "--version") { options.showVersion = true; continue; }
			if (arg == "--help" || arg == "-h") { printUsage(diagnosticsOut); return std::nullopt; }

			if (arg == "--stats" || arg == "-fstats") { options.emitStats = true; continue; }
			if (arg == "--symtab") { options.symbolTable = true; continue; }
			if (arg == "--gc-sections") { options.gcSections = true; continue; }
			if (arg == "-O0") { options.optimization = support::OptimizationOptions::forLevel(support::OptimizationLevel::O0); continue; }
			if (arg == "-O1" || arg == "-O") { options.optimization = support::OptimizationOptions::forLevel(support::OptimizationLevel::O1); continue; }
			if (arg == "-Os") { options.optimization = support::OptimizationOptions::forLevel(support::OptimizationLevel::Os); continue; }
			if (arg == "-Og") { options.optimization = support::OptimizationOptions::forLevel(support::OptimizationLevel::Og); continue; }
			if (arg == "-O2" || arg == "-O3")
			{
				// -O3 is accepted as a synonym for -O2 rather than rejected: there is no third tier
				// here, and failing a build over a habit every other C compiler tolerates helps
				// nobody. If a genuinely more aggressive tier ever exists, this is where it lands.
				options.optimization = support::OptimizationOptions::forLevel(support::OptimizationLevel::O2);
				continue;
			}
			if (arg == "-fsoft-double" || arg == "-fno-soft-double")
			{
				options.softDouble = arg == "-fsoft-double";
				continue;
			}
			if (arg.starts_with("-f"))
			{
				if (!applyOptimizationFlag(arg, options.optimization))
				{
					diagnosticsOut << "ceresc: unrecognized option '" << arg
						<< "' (only the -f<name> optimizations listed below are supported)\n";
					printUsage(diagnosticsOut);
					return std::nullopt;
				}
				continue;
			}

			if (arg == "-o")
			{
				std::optional<std::string> value = valueFor(arg, "-o", i);
				if (!value)
					return std::nullopt;
				options.outputPath = std::move(*value);
				continue;
			}
			if (arg.starts_with("-I"))
			{
				std::optional<std::string> value = valueFor(arg, "-I", i);
				if (!value)
					return std::nullopt;
				options.includeDirectories.push_back(std::move(*value));
				continue;
			}
			if (arg.starts_with("-D"))
			{
				std::optional<std::string> value = valueFor(arg, "-D", i);
				if (!value)
					return std::nullopt;
				options.defines.push_back(splitDefine(*value));
				continue;
			}
			if (arg.starts_with("-L"))
			{
				std::optional<std::string> value = valueFor(arg, "-L", i);
				if (!value)
					return std::nullopt;
				options.libraryDirectories.push_back(std::move(*value));
				continue;
			}
			if (arg == "-l" || (arg.starts_with("-l") && arg.size() > 2))
			{
				// `-lceres` and `-l ceres` are both accepted, the same way -I/-D take either form.
				std::optional<std::string> value = valueFor(arg, "-l", i);
				if (!value)
					return std::nullopt;
				options.libraries.push_back(std::move(*value));
				continue;
			}
			if (arg == "--decls" || arg == "--emit-decls")
			{
				std::optional<std::string> value = valueFor(arg, arg, i);
				if (!value)
					return std::nullopt;
				if (arg == "--decls")
					options.declsFiles.push_back(std::move(*value));
				else
					options.emitDeclsPath = std::move(*value);
				continue;
			}
			if (arg == "--")
			{
				// The rest is the program's own: main(argc, argv) gets them after its path.
				for (++i; i < args.size(); ++i)
					options.programArguments.emplace_back(args[i]);
				break;
			}
			if (arg == "--run-arg")
			{
				std::optional<std::string> value = valueFor(arg, "--run-arg", i);
				if (!value)
					return std::nullopt;
				options.runArguments.push_back(std::move(*value));
				continue;
			}
			if (arg == "--ceres-path")
			{
				std::optional<std::string> value = valueFor(arg, "--ceres-path", i);
				if (!value)
					return std::nullopt;
				options.ceresPath = std::move(*value);
				continue;
			}
			if (arg == "--sysroot")
			{
				std::optional<std::string> value = valueFor(arg, "--sysroot", i);
				if (!value)
					return std::nullopt;
				options.sysroot = std::move(*value);
				continue;
			}

			if (!arg.empty() && arg.front() == '-')
			{
				diagnosticsOut << "ceresc: unrecognized option '" << arg << "'\n";
				printUsage(diagnosticsOut);
				return std::nullopt;
			}

			options.inputPaths.emplace_back(arg);
		}

		// `--version` is a question about the compiler, not a request to compile something, so it
		// is the one flag that stands on its own without an input file.
		if (options.inputPaths.empty() && !options.showVersion)
		{
			printUsage(diagnosticsOut);
			return std::nullopt;
		}
		if ((options.clean || options.cleanKeepCasm) && !options.run)
		{
			diagnosticsOut << "ceresc: '--clean' and '--clean-keep-casm' require '--run'\n";
			return std::nullopt;
		}
		if (!options.programArguments.empty() && !options.run)
		{
			diagnosticsOut << "ceresc: arguments after '--' require '--run': they are the program's\n";
			return std::nullopt;
		}
		if (options.symbolTable && !options.run)
		{
			diagnosticsOut << "ceresc: '--symtab' requires '--run': it is an option of the link\n";
			return std::nullopt;
		}
		return options;
	}
}
