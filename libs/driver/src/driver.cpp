#include <ceresc/driver/driver.h>

#include <ceresc/driver/ceres_locator.h>

#include <ceresc/ast/ast_printer.h>
#include <ceresc/codegen/codegen.h>
#include <ceresc/ir/ir_builder.h>
#include <ceresc/ir/ir_optimizer.h>
#include <ceresc/ir/ir_printer.h>
#include <ceresc/lexer/lexer.h>
#include <ceresc/parser/parser.h>
#include <ceresc/preprocessor/preprocessor.h>
#include <ceresc/sema/sema.h>
#include <ceresc/support/arena.h>
#include <ceresc/support/diagnostics.h>
#include <ceresc/support/source_manager.h>
#include <ceresc/support/string_pool.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <unordered_set>
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

		bool isCasmInput(const std::string& path)
		{
			std::string extension = fs::path(path).extension().string();
			std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return extension == ".casm";
		}

		fs::path withExtension(const fs::path& path, std::string_view extension)
		{
			fs::path result = path;
			result.replace_extension(extension);
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

		bool writeTextFile(const fs::path& path, std::string_view text)
		{
			std::ofstream output(path, std::ios::binary);
			if (!output)
			{
				std::cerr << "ceresc: cannot write '" << path.string() << "'\n";
				return false;
			}
			output << text;
			output.close();
			if (!output)
			{
				std::cerr << "ceresc: failed to write '" << path.string() << "'\n";
				return false;
			}
			return true;
		}

		void removeGeneratedFile(const fs::path& path)
		{
			std::error_code error;
			if (!fs::remove(path, error) && error)
				std::cerr << "ceresc: cannot remove generated file '" << path.string() << "': " << error.message() << '\n';
		}

		// One C file, from source text to CASM text. Everything about a translation unit that the
		// driver needs afterwards lives here, so compiling several of them is a loop over this.
		struct CompiledUnit
		{
			fs::path sourcePath;
			fs::path casmPath;
			std::string casmText;
			std::vector<codegen::ExternalDeclaration> exports;
		};

		// The diagnostics printer. Threads a cursor through the SAME DiagnosticEngine every phase
		// shares, so printing "everything currently in the list" more than once cannot reprint an
		// earlier phase's diagnostics; and maps every location back through the preprocessor's line
		// map first, so an error inside a header names the header and its own line rather than a
		// line of the expanded text nobody can find.
		class DiagnosticPrinter
		{
		public:
			DiagnosticPrinter(const support::DiagnosticEngine& diagnostics, const support::SourceManager& sourceManager) noexcept :
				_diagnostics(diagnostics), _sourceManager(sourceManager)
			{}

			void setLineMap(const preprocessor::LineMap* lineMap) noexcept { _lineMap = lineMap; }

			void flush(std::ostream& out)
			{
				std::span<const support::Diagnostic> all = _diagnostics.diagnostics();
				for (usize i = _printed; i < all.size(); ++i)
				{
					const support::Diagnostic& diagnostic = all[i];
					support::SourceLocation location = diagnostic.location;
					if (_lineMap && !_lineMap->empty())
						location = _lineMap->toOriginal(location);
					const support::SourceBuffer* buffer = _sourceManager.getBuffer(location.sourceId);
					// `error[E3023]:` rather than `error:` - the code is what a program's
					// `#pragma warning(disable: ...)` names, and what searching for an explanation
					// of a message would search for (docs/11-Diagnostics.md). Printed only when
					// there is one, so a diagnostic with no id reads exactly as it always did.
					std::string code = support::diagnosticCode(diagnostic.id);
					out << (buffer ? buffer->name() : std::string_view("<unknown>")) << ':'
						<< location.line << ':' << location.column << ": "
						<< severityName(diagnostic.severity);
					if (!code.empty())
						out << '[' << code << ']';
					out << ": " << diagnostic.message << '\n';
				}
				_printed = all.size();
			}

		private:
			const support::DiagnosticEngine& _diagnostics;
			const support::SourceManager& _sourceManager;
			const preprocessor::LineMap* _lineMap = nullptr;
			usize _printed = 0;
		};

		// The CASM declarations file every generated unit imports. An object cannot reference a name
		// it has never seen declared - the assembler chooses an opcode from the shape of an operand,
		// long before anything has an address (25-Separate-Compilation.md) - so this is what lets a
		// call or a global read cross a file boundary.
		//
		// It is a declarations file, never assembled into an object of its own: an `import` in a unit
		// being assembled with -c contributes no bytes, only names and shapes. That is also why a
		// unit importing declarations of symbols it defines itself is not a redefinition.
		// The addresses only the linker can know (CeresASM 12-Labels-and-Symbols.md). A C unit may
		// legitimately say `extern char __heap_start;` to reach one, but declaring it here would
		// make the assembler refuse the whole program ("is defined by the linker, so a program
		// cannot declare it"). An object needs no declaration to name one: the link resolves it
		// like any other name it does not define.
		bool isLinkerSymbol(std::string_view name)
		{
			static constexpr std::string_view names[] = {
				"__text_start", "__text_end", "__rodata_start", "__rodata_end", "__data_start",
				"__data_end", "__bss_start", "__bss_end", "__heap_start" };
			for (std::string_view candidate : names)
				if (name == candidate)
					return true;
			return false;
		}

		std::string buildDeclarationsFile(const std::vector<CompiledUnit>& units)
		{
			std::vector<codegen::ExternalDeclaration> functions;
			std::vector<codegen::ExternalDeclaration> variables;
			std::unordered_set<std::string> seen;

			for (const CompiledUnit& unit : units)
			{
				for (const codegen::ExternalDeclaration& declaration : unit.exports)
				{
					if (!declaration.isFunction && !declaration.isDefinition && isLinkerSymbol(declaration.name))
						continue;

					// One entry per NAME across the whole program: the same function is normally
					// declared by every unit that calls it and defined by one, and CASM would report
					// the second declaration as a redefinition.
					auto [it, inserted] = seen.insert(declaration.name);
					if (inserted)
						(declaration.isFunction ? functions : variables).push_back(declaration);
					else if (declaration.isDefinition)
					{
						auto& bucket = declaration.isFunction ? functions : variables;
						for (codegen::ExternalDeclaration& existing : bucket)
							if (existing.name == declaration.name && (!existing.isDefinition || declaration.hasInitializer))
							{
								existing = declaration;
								break;
							}
					}
				}
			}

			std::string text =
				"// Generated by ceresc. Declarations only - no code and no storage.\n"
				"//\n"
				"// Every .casm this build generates imports this file, which is what lets one unit name a\n"
				"// function or a global another one defines. A hand-written .casm can import it too, to call\n"
				"// into the C side; see docs/07-CASM-Interop.md.\n";

			// Grouped by section so the file reads as a list of what lives where, and because a
			// declaration's section has to match its definition's for the assembler to give it the
			// same read-only flag.
			for (std::string_view section : { "@data", "@bss", "@rodata" })
			{
				bool wroteHeader = false;
				for (const codegen::ExternalDeclaration& declaration : variables)
				{
					if (declaration.section != section)
						continue;
					if (!wroteHeader)
					{
						text += "\n";
						text += section;
						text += "\n";
						wroteHeader = true;
					}
					text += std::format("global let {}: {}\n", declaration.name, declaration.typeText);
				}
			}

			if (!functions.empty())
			{
				text += "\n@text\n";
				for (const codegen::ExternalDeclaration& declaration : functions)
					text += std::format("global {}:\n", declaration.name);
			}
			return text;
		}
	}

	int run(const Options& options)
	{
		support::SourceManager sourceManager;
		support::DiagnosticEngine diagnostics;
		diagnostics.setWarningsAsErrors(options.warningsAsErrors);
		DiagnosticPrinter printer(diagnostics, sourceManager);

		std::vector<std::string> cInputs, casmInputs;
		for (const std::string& input : options.inputPaths)
			(isCasmInput(input) ? casmInputs : cInputs).push_back(input);

		if (cInputs.empty() && casmInputs.empty())
		{
			std::cerr << "ceresc: no input files\n";
			return 1;
		}

		// A `.casm` on its own is a legitimate thing to ask for - `ceresc runtime.casm --run` just
		// assembles and runs it - but there is nothing for the compiler to do with it.
		if (cInputs.empty() && !options.run)
		{
			std::cerr << "ceresc: nothing to compile (only .casm inputs were given; add --run to assemble and run them)\n";
			return 1;
		}

		// Where `ceres` is, found before any work is done: a build that compiles for a minute and then cannot
		// launch its assembler has wasted the minute (ceres_locator.h has the rules).
		fs::path ceresBinary;
		if (options.run)
		{
			CeresLookup lookup = locateCeres(options.ceresPath, CeresEnvironment::fromProcess());
			if (!lookup.found())
			{
				std::cerr << lookup.error;
				return 1;
			}
			ceresBinary = lookup.executable;
		}

		bool singleOutput = (cInputs.size() == 1 && casmInputs.empty() && !options.run);

		std::vector<CompiledUnit> units;
		units.reserve(cInputs.size());

		for (const std::string& inputPath : cInputs)
		{
			// ---- preprocess ------------------------------------------------------------------------
			preprocessor::Preprocessor preprocess(sourceManager, diagnostics);
			for (const std::string& directory : options.includeDirectories)
				preprocess.addIncludeDirectory(directory);
			for (const auto& [name, value] : options.defines)
				preprocess.define(name, value);

			preprocessor::PreprocessedSource expanded = preprocess.run(inputPath);
			// Preprocessor diagnostics already point at their original files; only later phases need
			// remapping. Dropping the map first also drops the PREVIOUS unit's, which was a pointer
			// into a PreprocessedSource that died with the last turn of this loop.
			printer.setLineMap(nullptr);
			diagnostics.setPolicy(nullptr); // the previous unit's, and its PreprocessedSource is gone
			printer.flush(std::cerr);
			if (!expanded.ok)
				return 1;
			printer.setLineMap(&expanded.lineMap);
			if (options.emitPreprocessed)
			{
				if (cInputs.size() > 1)
					std::cout << "// ---- " << inputPath << " ----\n";
				std::cout << expanded.text;
				printer.flush(std::cerr);
				continue;
			}

			// The expanded text is registered as its own buffer so the lexer's own locations are
			// self-consistent; the line map is what turns them back into the originals for printing.
			support::SourceId sourceId = sourceManager.registerBuffer(inputPath, std::string(expanded.text));
			const support::SourceBuffer* buffer = sourceManager.getBuffer(sourceId);
			// Now that the expanded text has an id, the map can say which buffer its output lines
			// are lines of - and refuse to remap a location that is already original. The warning
			// policy the file's own pragmas built is indexed by the same lines, and governs every
			// phase from here down; the preprocessor already applied it to its own diagnostics,
			// which point at their original files instead.
			expanded.lineMap.setExpandedSourceId(sourceId);
			expanded.diagnosticPolicy.setControlledSourceId(sourceId);
			diagnostics.setPolicy(&expanded.diagnosticPolicy);

			// ---- front end -------------------------------------------------------------------------
			support::Arena arena;
			support::StringPool pool;
			lexer::Lexer lexer(buffer->buffer(), sourceId, diagnostics, pool);
			parser::Parser parser(lexer, arena, diagnostics);
			ast::TranslationUnit* unit = parser.parseTranslationUnit();

			if (diagnostics.hasErrors())
			{
				printer.flush(std::cerr);
				return 1;
			}

			sema::Sema sema(arena, diagnostics);
			bool semaOk = sema.check(*unit);
			printer.flush(std::cerr);
			if (!semaOk)
				return 1; // no point generating code for a program that does not type-check - see driver.h

			if (options.emitAst)
			{
				std::cout << ast::AstPrinter{}.print(*unit);
				continue;
			}

			// ---- middle and back end ---------------------------------------------------------------
			ir::IrBuilder builder(arena, diagnostics, options.optimization);
			ir::IrModule module = builder.build(*unit);
			// Optimized IR is what --emit-ir shows too: the point of that flag is to see what the back
			// end will actually be handed, not an intermediate nobody compiles. -O0 leaves it untouched.
			ir::optimize(module, arena, options.optimization);

			if (options.emitIr)
			{
				std::cout << ir::IrPrinter{}.print(module);
				printer.flush(std::cerr);
				if (diagnostics.hasErrors())
					return 1;
				continue;
			}

			codegen::CodeGen codeGen(sourceManager, diagnostics, options.optimization);
			// The same map the diagnostics printer uses, for the same reason: a trailing comment
			// naming a line of the expanded text is a reference nobody can follow.
			codeGen.setLineMap(&expanded.lineMap);
			CompiledUnit compiled;
			compiled.sourcePath = inputPath;
			compiled.casmPath = singleOutput && !options.outputPath.empty()
				? fs::path(options.outputPath)
				: withExtension(fs::path(inputPath), ".casm");
			compiled.exports = codeGen.collectExternalDeclarations(*unit);
			compiled.casmText = codeGen.generate(*unit, module);
			printer.flush(std::cerr);
			if (diagnostics.hasErrors())
				return 1;

			units.push_back(std::move(compiled));
		}

		if (options.emitAst || options.emitIr || options.emitPreprocessed)
			return diagnostics.hasErrors() ? 1 : 0;

		// ---- write the units, and the declarations they share ---------------------------------------
		//
		// Only when there is more than one thing to link: a lone translation unit resolves every name
		// it uses by itself, and a program that needs no declarations file should not have one sitting
		// next to it.
		bool needsDeclarations = (units.size() + casmInputs.size()) > 1;
		fs::path declarationsPath;
		if (needsDeclarations)
		{
			fs::path programPath = options.outputPath.empty()
				? withExtension(fs::path(options.inputPaths.front()), ".cres")
				: fs::path(options.outputPath);
			declarationsPath = withExtension(programPath, ".decls.casm");
			if (!writeTextFile(declarationsPath, buildDeclarationsFile(units)))
				return 1;
		}

		for (CompiledUnit& unit : units)
		{
			std::string text;
			if (needsDeclarations)
			{
				// A relative import, resolved against the importing file's own directory, so moving
				// the build output somewhere else keeps working.
				fs::path relative = fs::relative(declarationsPath, unit.casmPath.parent_path());
				if (relative.empty())
					relative = declarationsPath;
				std::string spelled = relative.generic_string();
				text = std::format("import \"{}\"\n\n", spelled);
			}
			text += unit.casmText;
			if (!writeTextFile(unit.casmPath, text))
				return 1;
		}

		if (!options.run)
			return 0;

		// ---- assemble, link and run ------------------------------------------------------------------
		//
		std::vector<fs::path> casmFiles;
		for (const CompiledUnit& unit : units)
			casmFiles.push_back(unit.casmPath);
		for (const std::string& input : casmInputs)
			casmFiles.push_back(fs::path(input));

		fs::path programPath = options.outputPath.empty()
			? withExtension(casmFiles.front(), ".cres")
			: fs::path(options.outputPath);
		if (programPath.extension() != ".cres")
			programPath.replace_extension(".cres");

		// One object per unit, then one link. The whole-program path (`ceres asm` over a single file)
		// would be shorter for the one-file case, but having exactly one shape here means the
		// one-file and many-file builds cannot drift apart - and `ceres link` reports a missing
		// `main`, a duplicate symbol and an out-of-range reference with the same messages either way.
		std::vector<std::string> objectPaths;
		for (const fs::path& casmFile : casmFiles)
		{
			fs::path objectPath = withExtension(casmFile, ".cobj");
			int result = runSubprocess(ceresBinary, { "asm", "-c", casmFile.string(), "-o", objectPath.string() });
			if (result != 0)
			{
				if (result < 0)
					std::cerr << "ceresc: could not launch `ceres asm`\n";
				else
					std::cerr << "ceresc: `ceres asm` failed on '" << casmFile.string() << "' (exit code " << result << ")\n";
				return result < 0 ? 1 : result;
			}
			objectPaths.push_back(objectPath.string());
		}

		std::vector<std::string> linkArgs{ "link" };
		linkArgs.insert(linkArgs.end(), objectPaths.begin(), objectPaths.end());
		linkArgs.push_back("-o");
		linkArgs.push_back(programPath.string());
		int linkResult = runSubprocess(ceresBinary, linkArgs);
		if (linkResult != 0)
		{
			if (linkResult < 0)
				std::cerr << "ceresc: could not launch `ceres link`\n";
			else
				std::cerr << "ceresc: `ceres link` failed (exit code " << linkResult << ")\n";
			return linkResult < 0 ? 1 : linkResult;
		}

		int runResult = runSubprocess(ceresBinary, { "run", programPath.string() });
		if (runResult < 0)
			std::cerr << "ceresc: could not launch `ceres run`\n";
		if (runResult == 0 && (options.clean || options.cleanKeepCasm))
		{
			for (const fs::path& casmFile : casmFiles)
				removeGeneratedFile(withExtension(casmFile, ".cobj"));
			removeGeneratedFile(programPath);
			if (needsDeclarations)
				removeGeneratedFile(declarationsPath);
			if (options.clean)
			{
				for (const CompiledUnit& unit : units)
					removeGeneratedFile(unit.casmPath);
			}
		}
		return runResult < 0 ? 1 : runResult;
	}
}
