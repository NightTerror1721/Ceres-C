#include <ceresc/driver/options.h>

namespace ceresc::driver
{
	namespace
	{
		void printUsage(std::ostream& out)
		{
			out <<
				"usage: ceresc <file.c> [-o <file.casm>] [--emit-ast] [--emit-ir]\n"
				"               [--run] [--ceres-path <dir>] [-Werror]\n"
				"\n"
				"  <file.c>            the C subset source file to compile\n"
				"  -o <file.casm>      where to write the generated CASM text (default: <file.casm>)\n"
				"  --emit-ast          print the annotated AST (s-expression form) and stop\n"
				"  --emit-ir           print the IR and stop\n"
				"  --run               assemble and run the generated program with `ceres asm`/`ceres run`\n"
				"  --ceres-path <dir>  where to find the `ceres` binary (default: look it up on PATH)\n"
				"  -Werror             treat warnings as errors\n";
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
