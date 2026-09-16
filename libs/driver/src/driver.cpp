#include <ceresc/driver/driver.h>

#include <ceresc/ast/ast_printer.h>
#include <ceresc/codegen/codegen.h>
#include <ceresc/ir/ir_builder.h>
#include <ceresc/ir/ir_optimizer.h>
#include <ceresc/ir/ir_printer.h>
#include <ceresc/lexer/lexer.h>
#include <ceresc/parser/parser.h>
#include <ceresc/sema/sema.h>
#include <ceresc/support/arena.h>
#include <ceresc/support/diagnostics.h>
#include <ceresc/support/source_manager.h>
#include <ceresc/support/string_pool.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace ceresc::driver
{
	namespace
	{
		namespace fs = std::filesystem;

		std::string_view severityName(support::DiagnosticSeverity severity) noexcept
		{
			return severity == support::DiagnosticSeverity::Error ? "error" : "warning";
		}

		// Prints diagnostics from `diagnostics` starting at `printedSoFar` (an in/out cursor,
		// updated to the new total) - the SAME DiagnosticEngine is threaded through every phase
		// (lexer/parser/sema/codegen), so printing "everything currently in the list" more than
		// once would reprint an earlier phase's diagnostics every time a later phase is checked.
		void printNewDiagnostics(const support::DiagnosticEngine& diagnostics, const support::SourceManager& sourceManager,
			usize& printedSoFar, std::ostream& out)
		{
			std::span<const support::Diagnostic> all = diagnostics.diagnostics();
			for (usize i = printedSoFar; i < all.size(); ++i)
			{
				const support::Diagnostic& diagnostic = all[i];
				const support::SourceBuffer* buffer = sourceManager.getBuffer(diagnostic.location.sourceId);
				out << (buffer ? buffer->name() : std::string_view("<unknown>")) << ':'
					<< diagnostic.location.line << ':' << diagnostic.location.column << ": "
					<< severityName(diagnostic.severity) << ": " << diagnostic.message << '\n';
			}
			printedSoFar = all.size();
		}

		fs::path defaultOutputPath(const fs::path& input)
		{
			fs::path result = input;
			result.replace_extension(".casm");
			return result;
		}

		int runSubprocess(const fs::path& executable, const std::vector<std::string>& args)
		{
#if defined(_WIN32)
			auto quoteWindowsArg = [](std::string_view arg)
			{
				std::string result{"\""};
				usize backslashes = 0;
				for (char c : arg)
				{
					if (c == '\\') { ++backslashes; continue; }
					if (c == '\"') result.append(backslashes * 2 + 1, '\\');
					else result.append(backslashes, '\\');
					result += c;
					backslashes = 0;
				}
				result.append(backslashes * 2, '\\');
				result += '\"';
				return result;
			};

			std::string commandLine = quoteWindowsArg(executable.string());
			for (const std::string& arg : args)
			{
				commandLine += ' ';
				commandLine += quoteWindowsArg(arg);
			}
			std::vector<char> mutableCommand(commandLine.begin(), commandLine.end());
			mutableCommand.push_back('\0');
			STARTUPINFOA startupInfo{};
			startupInfo.cb = sizeof(startupInfo);
			PROCESS_INFORMATION processInfo{};
			if (!CreateProcessA(executable.string().c_str(), mutableCommand.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startupInfo, &processInfo))
				return -1;
			WaitForSingleObject(processInfo.hProcess, INFINITE);
			DWORD exitCode = 1;
			GetExitCodeProcess(processInfo.hProcess, &exitCode);
			CloseHandle(processInfo.hThread);
			CloseHandle(processInfo.hProcess);
			return static_cast<int>(exitCode);
#else
			std::vector<std::string> allArgs;
			allArgs.reserve(args.size() + 1);
			allArgs.push_back(executable.string());
			allArgs.insert(allArgs.end(), args.begin(), args.end());
			std::vector<char*> argv;
			argv.reserve(allArgs.size() + 1);
			for (std::string& arg : allArgs)
				argv.push_back(arg.data());
			argv.push_back(nullptr);

			pid_t pid = fork();
			if (pid < 0)
				return -1;
			if (pid == 0)
			{
				execvp(argv[0], argv.data());
				_Exit(127);
			}
			int status = 0;
			if (waitpid(pid, &status, 0) < 0)
				return -1;
			if (WIFEXITED(status))
				return WEXITSTATUS(status);
			return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
#endif
		}
	}

	int run(const Options& options)
	{
		std::ifstream input(options.inputPath, std::ios::binary);
		if (!input)
		{
			std::cerr << "ceresc: cannot open '" << options.inputPath << "'\n";
			return 1;
		}
		std::string content{ std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };

		support::SourceManager sourceManager;
		support::SourceId sourceId = sourceManager.registerBuffer(options.inputPath, std::move(content));
		support::Arena arena;
		support::DiagnosticEngine diagnostics;
		diagnostics.setWarningsAsErrors(options.warningsAsErrors);
		support::StringPool pool;
		usize printedSoFar = 0;

		const support::SourceBuffer* buffer = sourceManager.getBuffer(sourceId);
		lexer::Lexer lexer(buffer->buffer(), sourceId, diagnostics, pool);
		parser::Parser parser(lexer, arena, diagnostics);
		ast::TranslationUnit* unit = parser.parseTranslationUnit();

		if (diagnostics.hasErrors())
		{
			printNewDiagnostics(diagnostics, sourceManager, printedSoFar, std::cerr);
			return 1;
		}

		sema::Sema sema(arena, diagnostics);
		bool semaOk = sema.check(*unit);
		printNewDiagnostics(diagnostics, sourceManager, printedSoFar, std::cerr);
		if (!semaOk)
			return 1; // no point generating code for a program that does not type-check - see driver.h
		if (options.emitAst)
		{
			std::cout << ast::AstPrinter{}.print(*unit);
			return 0;
		}

		ir::IrBuilder builder(arena, diagnostics, options.optimization);
		ir::IrModule module = builder.build(*unit);
		// Optimized IR is what --emit-ir shows too: the point of that flag is to see what the back
		// end will actually be handed, not an intermediate nobody compiles. -O0 leaves it untouched.
		ir::optimize(module, arena, options.optimization);

		if (options.emitIr)
		{
			std::cout << ir::IrPrinter{}.print(module);
			printNewDiagnostics(diagnostics, sourceManager, printedSoFar, std::cerr);
			return diagnostics.hasErrors() ? 1 : 0;
		}

		codegen::CodeGen codeGen(sourceManager, diagnostics, options.optimization);
		std::string casmText = codeGen.generate(*unit, module);
		printNewDiagnostics(diagnostics, sourceManager, printedSoFar, std::cerr);
		if (diagnostics.hasErrors())
			return 1;

		fs::path outputPath = options.outputPath.empty() ? defaultOutputPath(options.inputPath) : fs::path(options.outputPath);
		std::ofstream output(outputPath, std::ios::binary);
		if (!output)
		{
			std::cerr << "ceresc: cannot write '" << outputPath.string() << "'\n";
			return 1;
		}
		output << casmText;
		output.close();
		if (!output)
		{
			std::cerr << "ceresc: failed to write '" << outputPath.string() << "'\n";
			return 1;
		}

		if (!options.run)
			return 0;

		// Bare "ceres" (relying on PATH + PATHEXT resolution) when no directory was given; the
		// explicit filename otherwise - joining a directory with a bare "ceres" on a case-
		// insensitive filesystem can resolve to an unrelated same-named directory instead of the
		// executable (as CeresASM's own checkout has at its root: `Ceres/`, the library tree).
#if defined(_WIN32)
		constexpr std::string_view kCeresExecutableName = "ceres.exe";
#else
		constexpr std::string_view kCeresExecutableName = "ceres";
#endif
		fs::path ceresBinary = options.ceresPath.empty() ? fs::path("ceres") : (fs::path(options.ceresPath) / kCeresExecutableName);
		fs::path cresPath = outputPath;
		cresPath.replace_extension(".cres");

		int asmResult = runSubprocess(ceresBinary, { "asm", outputPath.string(), "-o", cresPath.string() });
		if (asmResult != 0)
		{
			if (asmResult < 0)
				std::cerr << "ceresc: could not launch `ceres asm`\n";
			else
				std::cerr << "ceresc: `ceres asm` failed (exit code " << asmResult << ")\n";
			return asmResult < 0 ? 1 : asmResult;
		}

		int runResult = runSubprocess(ceresBinary, { "run", cresPath.string() });
		if (runResult < 0)
			std::cerr << "ceresc: could not launch `ceres run`\n";
		return runResult < 0 ? 1 : runResult;
	}
}
