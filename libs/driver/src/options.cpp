#include <ceresc/driver/options.h>

#include <format>
#include <string_view>

namespace ceresc::driver
{
	namespace
	{
		void printUsage(std::ostream& out)
		{
			out <<
				"usage: ceresc <file.c> [-o <file.casm>] [--emit-ast] [--emit-ir]\n"
				"               [--run] [--ceres-path <dir>] [-Werror] [-O<level>] [-f<opt>]\n"
				"\n"
				"  <file.c>            the C subset source file to compile\n"
				"  -o <file.casm>      where to write the generated CASM text (default: <file.casm>)\n"
				"  --emit-ast          print the annotated AST (s-expression form) and stop\n"
				"  --emit-ir           print the IR and stop\n"
				"  --run               assemble and run the generated program with `ceres asm`/`ceres run`\n"
				"  --ceres-path <dir>  where to find the `ceres` binary (default: look it up on PATH)\n"
				"  -Werror             treat warnings as errors\n"
				"\n"
				"optimization (on by default, -O1):\n"
				"  -O0                 every optimization off - the simplest, most uniform output\n"
				"  -O1                 everything except inlining (the default)\n"
				"  -O2                 everything, inlining included\n"
				"  -O3                 accepted as an alias for -O2\n"
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
	}

	std::optional<Options> parseOptions(std::span<const char* const> args, std::ostream& diagnosticsOut)
	{
		Options options;
		bool sawInput = false;

		for (usize i = 0; i < args.size(); ++i)
		{
			std::string_view arg = args[i];

			if (arg == "--emit-ast") { options.emitAst = true; continue; }
			if (arg == "--emit-ir") { options.emitIr = true; continue; }
			if (arg == "-S") { continue; } // already the default: stop at .casm text unless --run
			if (arg == "--run") { options.run = true; continue; }
			if (arg == "-Werror") { options.warningsAsErrors = true; continue; }
			if (arg == "--help" || arg == "-h") { printUsage(diagnosticsOut); return std::nullopt; }

			if (arg == "-O0") { options.optimization = support::OptimizationOptions::forLevel(support::OptimizationLevel::O0); continue; }
			if (arg == "-O1" || arg == "-O") { options.optimization = support::OptimizationOptions::forLevel(support::OptimizationLevel::O1); continue; }
			if (arg == "-O2" || arg == "-O3")
			{
				// -O3 is accepted as a synonym for -O2 rather than rejected: there is no third tier
				// here, and failing a build over a habit every other C compiler tolerates helps
				// nobody. If a genuinely more aggressive tier ever exists, this is where it lands.
				options.optimization = support::OptimizationOptions::forLevel(support::OptimizationLevel::O2);
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
				if (i + 1 >= args.size())
				{
					diagnosticsOut << "ceresc: '-o' requires an argument\n";
					return std::nullopt;
				}
				options.outputPath = args[++i];
				continue;
			}
			if (arg == "--ceres-path")
			{
				if (i + 1 >= args.size())
				{
					diagnosticsOut << "ceresc: '--ceres-path' requires an argument\n";
					return std::nullopt;
				}
				options.ceresPath = args[++i];
				continue;
			}

			if (!arg.empty() && arg.front() == '-')
			{
				diagnosticsOut << "ceresc: unrecognized option '" << arg << "'\n";
				printUsage(diagnosticsOut);
				return std::nullopt;
			}

			if (sawInput)
			{
				diagnosticsOut << "ceresc: multiple input files are not supported\n";
				return std::nullopt;
			}
			options.inputPath = arg;
			sawInput = true;
		}

		if (!sawInput)
		{
			printUsage(diagnosticsOut);
			return std::nullopt;
		}
		return options;
	}
}
