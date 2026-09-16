#include <ceresc/preprocessor/preprocessor.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace ceresc::preprocessor
{
	namespace
	{
		namespace fs = std::filesystem;

		// The most times one line is rescanned for macros. A macro whose replacement mentions itself
		// would otherwise never stop; with a limit it simply stops being rewritten, which is what
		// every real preprocessor arranges (by a different mechanism) too.
		constexpr int kMaxMacroPasses = 16;

		constexpr int kMaxIncludeDepth = 64;

		bool isIdentifierStart(char c) noexcept
		{
			return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
		}

		bool isIdentifierChar(char c) noexcept
		{
			return isIdentifierStart(c) || (c >= '0' && c <= '9');
		}

		std::string_view trim(std::string_view text) noexcept
		{
			usize begin = 0;
			while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r'))
				++begin;
			usize end = text.size();
			while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r'))
				--end;
			return text.substr(begin, end - begin);
		}

		// A canonical spelling of `path`, so the same header reached two different ways ("h/x.h" and
		// "./h/x.h") counts as the same file for `#pragma once`. Falls back to the path as written
		// when the filesystem cannot answer, which is the conservative direction: the worst case is
		// the file being expanded twice, not a wrong file being used.
		std::string canonicalPath(const std::string& path)
		{
			std::error_code error;
			fs::path canonical = fs::weakly_canonical(fs::path(path), error);
			return error ? path : canonical.string();
		}
	}

	support::SourceLocation LineMap::toOriginal(support::SourceLocation location) const noexcept
	{
		if (_entries.empty() || location.line == 0 || location.line > _entries.size())
			return location;
		const LineMapEntry& entry = _entries[location.line - 1];
		return support::SourceLocation{ entry.sourceId, entry.sourceLine, location.column, location.offset };
	}

	PreprocessedSource Preprocessor::run(const std::string& path)
	{
		PreprocessedSource result;
		std::vector<std::string> includeStack;
		result.ok = expandFile(path, result, includeStack);
		return result;
	}

	std::string Preprocessor::resolveInclude(std::string_view target, bool angled, const std::string& includingFile) const
	{
		std::error_code error;
		if (!angled)
		{
			// `"..."` looks next to the file doing the including first. That is what makes a project
			// whose headers sit beside its sources work with no -I at all.
			fs::path relative = fs::path(includingFile).parent_path() / fs::path(std::string(target));
			if (fs::is_regular_file(relative, error))
				return relative.string();
		}
		for (const std::string& directory : _includeDirectories)
		{
			fs::path candidate = fs::path(directory) / fs::path(std::string(target));
			if (fs::is_regular_file(candidate, error))
				return candidate.string();
		}
		// Last resort for `"..."`: the path exactly as written, relative to the working directory.
		if (!angled && fs::is_regular_file(fs::path(std::string(target)), error))
			return std::string(target);
		return {};
	}

	std::string Preprocessor::expandMacros(std::string_view line, support::SourceLocation location)
	{
		if (_macros.empty())
			return std::string(line);

		std::string current(line);
		for (int pass = 0; pass < kMaxMacroPasses; ++pass)
		{
			std::string next;
			next.reserve(current.size());
			bool changed = false;

			for (usize i = 0; i < current.size();)
			{
				char c = current[i];

				// Anything inside a literal or a comment is text, not a name. Getting this wrong is
				// how a preprocessor "helpfully" rewrites the inside of a printed message.
				if (c == '"' || c == '\'')
				{
					char quote = c;
					next += c;
					++i;
					while (i < current.size())
					{
						next += current[i];
						if (current[i] == '\\' && i + 1 < current.size())
						{
							next += current[i + 1];
							i += 2;
							continue;
						}
						if (current[i] == quote)
						{
							++i;
							break;
						}
						++i;
					}
					continue;
				}
				if (c == '/' && i + 1 < current.size() && current[i + 1] == '/')
				{
					next.append(current, i, std::string::npos);
					break;
				}
				if (c == '/' && i + 1 < current.size() && current[i + 1] == '*')
				{
					usize end = current.find("*/", i + 2);
					usize stop = (end == std::string::npos) ? current.size() : end + 2;
					next.append(current, i, stop - i);
					i = stop;
					continue;
				}

				if (!isIdentifierStart(c))
				{
					next += c;
					++i;
					continue;
				}

				usize start = i;
				while (i < current.size() && isIdentifierChar(current[i]))
					++i;
				std::string_view name(current.data() + start, i - start);
				auto it = _macros.find(std::string(name));
				if (it == _macros.end())
				{
					next.append(name);
					continue;
				}
				next += it->second;
				changed = true;
			}

			current = std::move(next);
			if (!changed)
				return current;
		}

		_diagnostics.warning(location, "gave up expanding macros on this line after {} passes - is a macro defined in terms of itself?",
			kMaxMacroPasses);
		return current;
	}

	bool Preprocessor::expandFile(const std::string& path, PreprocessedSource& out, std::vector<std::string>& includeStack)
	{
		std::string canonical = canonicalPath(path);

		if (std::find(_pragmaOnce.begin(), _pragmaOnce.end(), canonical) != _pragmaOnce.end())
			return true; // already contributed once, and asked not to again

		if (std::find(includeStack.begin(), includeStack.end(), canonical) != includeStack.end())
		{
			// Reported against the file that closed the loop rather than as a stack overflow. A
			// header that includes itself, directly or through a chain, has no expansion at all.
			_diagnostics.error({}, "include cycle: '{}' includes itself", path);
			return false;
		}
		if (includeStack.size() >= kMaxIncludeDepth)
		{
			_diagnostics.error({}, "#include nested more than {} deep, starting at '{}'", kMaxIncludeDepth, path);
			return false;
		}

		std::ifstream input(path, std::ios::binary);
		if (!input)
		{
			_diagnostics.error({}, "cannot open '{}'", path);
			return false;
		}
		std::string contents{ std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };

		// Registered even though the lexer will never read it: a diagnostic about a line of this
		// file needs the SourceManager to know its name.
		support::SourceId sourceId = _sourceManager.registerBuffer(path, std::move(contents));
		const support::SourceBuffer* buffer = _sourceManager.getBuffer(sourceId);
		if (!buffer)
			return false;

		includeStack.push_back(canonical);
		bool ok = true;
		u32 sourceLine = 0;

		// `< size`, not `<= size`: a file that ends with a newline has no line after it, and an
		// extra empty one would shift every line number following an #include by one - which is
		// exactly the kind of off-by-one the line map exists to prevent.
		std::string_view text = buffer->buffer();
		usize position = 0;
		while (position < text.size())
		{
			usize newline = text.find('\n', position);
			bool lastLine = (newline == std::string_view::npos);
			std::string_view line = text.substr(position, (lastLine ? text.size() : newline) - position);
			position = lastLine ? text.size() : newline + 1;
			++sourceLine;

			support::SourceLocation here{ sourceId, sourceLine, 1, 0 };
			std::string_view trimmed = trim(line);

			if (!trimmed.empty() && trimmed.front() == '#')
			{
				std::string_view directive = trim(trimmed.substr(1));
				// Every directive contributes a BLANK line rather than nothing, so the expanded text
				// keeps one line per source line for the files that have no includes at all - the
				// common case, where the line map then reads as the identity and a diagnostic's line
				// is already right even before it is mapped.
				auto keepLineNumbering = [&]()
				{
					out.lineMap.append(static_cast<u32>(out.lineMap.entries().size() + 1), sourceId, sourceLine);
					out.text += '\n';
				};

				if (directive.starts_with("include"))
				{
					std::string_view target = trim(directive.substr(7));
					bool angled = !target.empty() && target.front() == '<';
					char closing = angled ? '>' : '"';
					if (target.size() < 2 || (target.front() != '<' && target.front() != '"') || target.back() != closing)
					{
						_diagnostics.error(here, "#include expects \"file.h\" or <file.h>");
						ok = false;
						keepLineNumbering();
						continue;
					}
					std::string_view name = target.substr(1, target.size() - 2);
					std::string resolved = resolveInclude(name, angled, path);
					if (resolved.empty())
					{
						_diagnostics.error(here, "cannot find include file '{}'", name);
						ok = false;
						keepLineNumbering();
						continue;
					}
					// No blank line of its own: the included file's own lines take this one's place.
					ok = expandFile(resolved, out, includeStack) && ok;
					continue;
				}

				if (directive.starts_with("define"))
				{
					std::string_view rest = trim(directive.substr(6));
					usize nameEnd = 0;
					while (nameEnd < rest.size() && isIdentifierChar(rest[nameEnd]))
						++nameEnd;
					if (nameEnd == 0 || !isIdentifierStart(rest[0]))
					{
						_diagnostics.error(here, "#define expects a name");
						ok = false;
					}
					else if (nameEnd < rest.size() && rest[nameEnd] == '(')
					{
						// Worth its own message: a function-like macro is a real feature this version
						// does not have, not a typo in the name.
						_diagnostics.error(here, "macros with arguments are not supported in this version");
						ok = false;
					}
					else
					{
						std::string name(rest.substr(0, nameEnd));
						std::string replacement(trim(rest.substr(nameEnd)));
						_macros[name] = replacement;
					}
					keepLineNumbering();
					continue;
				}

				if (directive.starts_with("undef"))
				{
					std::string_view name = trim(directive.substr(5));
					_macros.erase(std::string(name));
					keepLineNumbering();
					continue;
				}

				if (directive.starts_with("pragma"))
				{
					std::string_view rest = trim(directive.substr(6));
					if (rest == "once")
						_pragmaOnce.push_back(canonical);
					else
						_diagnostics.warning(here, "ignoring unknown pragma '{}'", rest);
					keepLineNumbering();
					continue;
				}

				if (directive.empty())
				{
					keepLineNumbering(); // a bare '#' is a null directive, and legal
					continue;
				}

				// Named rather than skipped: the point is that it is a KNOWN gap, not that the line
				// was unreadable. #if/#ifdef in particular are the ones people reach for first.
				std::string_view word = directive.substr(0, directive.find_first_of(" \t"));
				_diagnostics.error(here, "'#{}' is not supported in this version - only #include, #define, #undef and #pragma once are",
					word);
				ok = false;
				keepLineNumbering();
				continue;
			}

			out.lineMap.append(static_cast<u32>(out.lineMap.entries().size() + 1), sourceId, sourceLine);
			out.text += expandMacros(line, here);
			out.text += '\n';
		}

		includeStack.pop_back();
		return ok;
	}
}
