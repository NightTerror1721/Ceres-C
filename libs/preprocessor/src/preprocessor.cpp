#include <ceresc/preprocessor/preprocessor.h>

#include <algorithm>
#include <charconv>
#include <optional>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>

namespace ceresc::preprocessor
{
	// Shorthand for the ids these messages are classified by - every error() and warning()
	// call below names one. See support/diagnostic_id.h.
	using DiagId = support::DiagnosticId;

	namespace
	{
		namespace fs = std::filesystem;
		constexpr int kMaxMacroPasses = 32;
		constexpr usize kMaxExpandedLineBytes = 1024 * 1024;
		constexpr int kMaxIncludeDepth = 64;

		bool isIdentifierStart(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
		bool isIdentifierChar(char c) noexcept { return isIdentifierStart(c) || (c >= '0' && c <= '9'); }
		std::string_view trim(std::string_view text) noexcept
		{
			usize begin = 0, end = text.size();
			while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r')) ++begin;
			while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) --end;
			return text.substr(begin, end - begin);
		}
		std::string canonicalPath(const std::string& path)
		{
			std::error_code error;
			fs::path canonical = fs::weakly_canonical(fs::path(path), error);
			return error ? path : canonical.string();
		}
		// True when a physical line ends in a backslash immediately before its newline, which is
		// C's line continuation: the next physical line is spliced onto this one. A lone trailing
		// `\r` (CRLF) sits after the backslash and does not count.
		bool continuesWithBackslash(std::string_view line) noexcept
		{
			if (line.empty())
				return false;
			usize back = line.size() - 1;
			if (line[back] == '\r' && back != 0)
				--back;
			return line[back] == '\\';
		}
		// The same line with the continuation backslash (and any CRLF `\r` after it) removed.
		std::string_view stripContinuation(std::string_view line) noexcept
		{
			usize end = line.size();
			if (end != 0 && line[end - 1] == '\r')
				--end;
			if (end != 0 && line[end - 1] == '\\')
				--end;
			return line.substr(0, end);
		}
		std::string_view withoutDirectiveComment(std::string_view text)
		{
			usize a = text.find("//"), b = text.find("/*"), comment = std::min(a, b);
			return trim(comment == std::string_view::npos ? text : text.substr(0, comment));
		}
		bool isDirective(std::string_view directive, std::string_view name) noexcept
		{
			return directive.starts_with(name) && (directive.size() == name.size() || !isIdentifierChar(directive[name.size()]));
		}
		// `text` as a C string literal. __FILE__ and friends expand to one, and a Windows path is
		// full of backslashes - which the lexer would read as escapes if they were passed through.
		std::string asStringLiteral(std::string_view text)
		{
			std::string result = "\"";
			for (char c : text)
			{
				if (c == '\\' || c == '"')
					result += '\\';
				result += c;
			}
			result += '"';
			return result;
		}
		// An argument run through `#`: its text as a string literal. Leading and trailing
		// whitespace is dropped, a run of internal whitespace becomes one space, and the two
		// characters a literal cannot hold raw are escaped - C's stringification, apart from the
		// whitespace inside an argument having to be on one physical line to be here at all.
		std::string stringify(std::string_view text)
		{
			text = trim(text);
			std::string result = "\"";
			bool betweenTokens = false;
			for (char c : text)
			{
				if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
				{
					betweenTokens = true;
					continue;
				}
				if (betweenTokens)
				{
					result += ' ';
					betweenTokens = false;
				}
				if (c == '\\' || c == '"')
					result += '\\';
				result += c;
			}
			result += '"';
			return result;
		}
		// `##` joins the two tokens on either side into one: the operator, and any whitespace
		// around it, is deleted - `a ## b` becomes `ab`. The result is still text rather than a
		// token, so `##` has no notion of what a valid pasted token is and pastes anything.
		std::string paste(std::string text)
		{
			std::string result;
			for (usize i = 0; i < text.size();)
			{
				if (text[i] == '#' && i + 1 < text.size() && text[i + 1] == '#')
				{
					while (!result.empty() && (result.back() == ' ' || result.back() == '\t'))
						result.pop_back();
					i += 2;
					while (i < text.size() && (text[i] == ' ' || text[i] == '\t'))
						++i;
					continue;
				}
				result += text[i++];
			}
			return result;
		}
		// The local calendar time this run started, which is what __DATE__ and __TIME__ have to
		// agree about: C requires every expansion of either within one translation unit to give the
		// same answer, so the clock is read once rather than per use.
		std::tm localNow()
		{
			std::time_t now = std::time(nullptr);
			std::tm parts{};
#if defined(_WIN32)
			localtime_s(&parts, &now);
#else
			localtime_r(&now, &parts);
#endif
			return parts;
		}

		class IfExpressionParser
		{
		public:
			IfExpressionParser(std::string_view text, const std::unordered_map<std::string, Preprocessor::Macro>& macros,
				support::DiagnosticEngine& diagnostics, support::SourceLocation location) : _text(text), _macros(macros), _diagnostics(diagnostics), _location(location) {}
			bool parse(i64& value) { value = logicalOr(); skip(); if (_ok && _pos != _text.size()) fail("invalid token in #if expression"); return _ok; }
		private:
			std::string_view _text; const std::unordered_map<std::string, Preprocessor::Macro>& _macros;
			support::DiagnosticEngine& _diagnostics; support::SourceLocation _location; usize _pos = 0; bool _ok = true;
			void skip() { while (_pos < _text.size() && (_text[_pos] == ' ' || _text[_pos] == '\t')) ++_pos; }
			bool take(std::string_view token) { skip(); if (_text.substr(_pos).starts_with(token)) { _pos += token.size(); return true; } return false; }
			void fail(std::string_view message) { if (_ok) _diagnostics.error(DiagId::IfExpressionSyntax, _location, "{}", message); _ok = false; }
			i64 primary()
			{
				skip();
				if (take("(")) { i64 v = logicalOr(); if (!take(")")) fail("expected ')' in #if expression"); return v; }
				if (_text.substr(_pos).starts_with("defined") && (_pos + 7 == _text.size() || !isIdentifierChar(_text[_pos + 7])))
				{
					_pos += 7; skip(); bool paren = take("("); skip(); usize start = _pos;
					while (_pos < _text.size() && isIdentifierChar(_text[_pos])) ++_pos;
					if (start == _pos) { fail("defined expects an identifier"); return 0; }
					std::string name(_text.substr(start, _pos - start));
					if (paren && !take(")")) fail("defined expects ')'");
					return _macros.contains(name) ? 1 : 0;
				}
				if (_pos < _text.size() && isIdentifierStart(_text[_pos])) { while (_pos < _text.size() && isIdentifierChar(_text[_pos])) ++_pos; return 0; }
				usize start = _pos; while (_pos < _text.size() && (std::isalnum(static_cast<unsigned char>(_text[_pos])) || _text[_pos] == 'x' || _text[_pos] == 'X')) ++_pos;
				if (start == _pos) { fail("expected integer in #if expression"); return 0; }
				u64 value = 0; std::string_view number = _text.substr(start, _pos - start); int base = 10;
				if (number.size() > 2 && number[0] == '0' && (number[1] == 'x' || number[1] == 'X')) { base = 16; number.remove_prefix(2); }
				auto result = std::from_chars(number.data(), number.data() + number.size(), value, base);
				if (result.ec != std::errc{} || result.ptr != number.data() + number.size()) fail("invalid integer in #if expression");
				return static_cast<i64>(value);
			}
			i64 unary() { if (take("!")) return !unary(); if (take("~")) return ~unary(); if (take("+")) return unary(); if (take("-")) return -unary(); return primary(); }
			i64 multiplicative() { i64 v = unary(); for (;;) { if (take("*")) v *= unary(); else if (take("/")) { i64 r = unary(); if (!r) fail("division by zero in #if expression"); else v /= r; } else if (take("%")) { i64 r = unary(); if (!r) fail("division by zero in #if expression"); else v %= r; } else return v; } }
			i64 additive() { i64 v = multiplicative(); for (;;) { if (take("+")) v += multiplicative(); else if (take("-")) v -= multiplicative(); else return v; } }
			i64 shift() { i64 v = additive(); for (;;) { if (take("<<")) v <<= additive(); else if (take(">>")) v >>= additive(); else return v; } }
			i64 relational() { i64 v = shift(); for (;;) { if (take("<=")) v = v <= shift(); else if (take(">=")) v = v >= shift(); else if (take("<")) v = v < shift(); else if (take(">")) v = v > shift(); else return v; } }
			i64 equality() { i64 v = relational(); for (;;) { if (take("==")) v = v == relational(); else if (take("!=")) v = v != relational(); else return v; } }
			i64 bitAnd() { i64 v = equality(); for (;;) { skip(); if (_text.substr(_pos).starts_with("&&") || !take("&")) return v; v &= equality(); } }
			i64 bitXor() { i64 v = bitAnd(); while (take("^")) v ^= bitAnd(); return v; }
			i64 bitOr() { i64 v = bitXor(); for (;;) { skip(); if (_text.substr(_pos).starts_with("||") || !take("|")) return v; v |= bitXor(); } }
			i64 logicalAnd() { i64 v = bitOr(); while (take("&&")) v = (v && bitOr()); return v; }
			i64 logicalOr() { i64 v = logicalAnd(); while (take("||")) v = (v || logicalAnd()); return v; }
		};
	}

	void Preprocessor::definePredefinedMacros()
	{
		auto object = [&](std::string name, std::string replacement)
		{
			Macro macro;
			macro.replacement = std::move(replacement);
			_macros[std::move(name)] = std::move(macro);
		};
		auto builtin = [&](std::string name, Builtin kind)
		{
			Macro macro;
			macro.builtin = kind;
			_macros[std::move(name)] = std::move(macro);
		};

		// __STDC__ says "this is an ANSI C compiler, not K&R", which is the question a program
		// testing it is actually asking. __STDC_VERSION__ is deliberately NOT defined: C89 does not
		// define it either, and naming a later revision would claim conformance to one this
		// compiler does not implement - docs/02-Grammar.md is the contract instead.
		object("__STDC__", "1");
		// Freestanding, and not in the borderline sense: there is no <stdio.h>, no <stdlib.h> and no
		// standard library at all for an implementation to be hosted by.
		object("__STDC_HOSTED__", "0");
		builtin("__LINE__", Builtin::Line);
		builtin("__FILE__", Builtin::File);

		// One clock reading for the whole run, because C requires every expansion of __DATE__ or
		// __TIME__ within a translation unit to agree - a file that takes a second to preprocess
		// must not straddle a tick.
		std::tm now = localNow();
		static constexpr std::string_view kMonths[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
			"Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
		std::string_view month = (now.tm_mon >= 0 && now.tm_mon < 12) ? kMonths[now.tm_mon] : "Jan";
		// "Mmm dd yyyy", with the day SPACE-padded rather than zero-padded - asctime's format, which
		// is the one C names.
		object("__DATE__", asStringLiteral(std::format("{} {:2} {}", month, now.tm_mday, now.tm_year + 1900)));
		object("__TIME__", asStringLiteral(std::format("{:02}:{:02}:{:02}", now.tm_hour, now.tm_min, now.tm_sec)));

		// Past the standard, and each one here because something in this project wants it rather
		// than to copy a list: which compiler this is, which .c the unit started from, how deep in
		// headers a line sits, and a fresh number per expansion for building a name that cannot
		// collide with another the same macro made.
		object("__CERESC__", "1");
		builtin("__BASE_FILE__", Builtin::BaseFile);
		builtin("__INCLUDE_LEVEL__", Builtin::IncludeLevel);
		builtin("__COUNTER__", Builtin::Counter);
	}
	std::string Preprocessor::expandBuiltin(Builtin builtin, support::SourceLocation location)
	{
		switch (builtin)
		{
			case Builtin::Line:
				// `location` is a position in the ORIGINAL file (expandFile builds it from the line
				// it is reading), not in the expanded text - so this needs no help from the line map.
				return std::to_string(location.line);
			case Builtin::File:
			{
				const auto* buffer = _sourceManager.getBuffer(location.sourceId);
				return asStringLiteral(buffer ? buffer->name() : std::string_view(_basePath));
			}
			case Builtin::BaseFile:
				return asStringLiteral(_basePath);
			case Builtin::IncludeLevel:
				return std::to_string(_includeLevel);
			case Builtin::Counter:
				return std::to_string(_counter++);
			case Builtin::None:
				break;
		}
		return {};
	}
	PreprocessedSource Preprocessor::run(const std::string& path)
	{
		_macros.clear();
		_basePath = path;
		_includeLevel = 0;
		_counter = 0;
		_warningPragmaDepth = 0;
		definePredefinedMacros();
		// Assignment rather than emplace: the command line's own -D goes in on TOP of a predefined
		// macro of the same name, so `-D __STDC_HOSTED__=1` is a decision a program gets to make.
		for (const auto& [name, replacement] : _predefines)
		{
			Macro macro;
			macro.replacement = replacement;
			_macros[name] = std::move(macro);
		}
		_pragmaOnce.clear(); PreprocessedSource result; std::vector<std::string> stack; result.ok = expandFile(path, result, stack); return result;
	}
	std::string Preprocessor::resolveInclude(std::string_view target, bool angled, const std::string& includingFile) const
	{
		std::error_code error;
		// generic_string() rather than string(): the resolved path becomes the header's NAME in the
		// SourceManager, which is what a diagnostic about it prints and what __FILE__ expands to -
		// and a path joined natively on Windows mixes both separators in one name.
		if (!angled) { fs::path relative = fs::path(includingFile).parent_path() / std::string(target); if (fs::is_regular_file(relative, error)) return relative.generic_string(); }
		for (const std::string& directory : _includeDirectories) { fs::path candidate = fs::path(directory) / std::string(target); if (fs::is_regular_file(candidate, error)) return candidate.generic_string(); }
		if (!angled && fs::is_regular_file(fs::path(std::string(target)), error))
			return std::string(target);
		return {};
	}
	bool Preprocessor::evaluateIfExpression(std::string_view expression, support::SourceLocation location,
		std::string_view includingFile, i64& value)
	{
		// `defined(NAME)` is recognized before ordinary substitution so NAME remains an identifier
		// even when it is itself an object-like macro. `__has_include("x")` is answered here too:
		// it needs the file system, not an expression, so it is folded to 1 or 0 before the
		// expression parser ever sees it.
		std::string protectedExpression;
		for (usize i = 0; i < expression.size();)
		{
			// The leading boundary matters as much as the trailing one: without it, an identifier
			// that merely ENDS in `__has_include` (or `defined`) would match partway through.
			if ((i == 0 || !isIdentifierChar(expression[i - 1])) &&
				expression.substr(i).starts_with("__has_include") &&
				(i + 13 == expression.size() || !isIdentifierChar(expression[i + 13])))
			{
				usize p = i + 13;
				while (p < expression.size() && (expression[p] == ' ' || expression[p] == '\t')) ++p;
				if (p < expression.size() && expression[p] == '(')
				{
					++p;
					while (p < expression.size() && (expression[p] == ' ' || expression[p] == '\t')) ++p;
					if (p < expression.size() && (expression[p] == '"' || expression[p] == '<'))
					{
						char open = expression[p];
						char close = open == '<' ? '>' : '"';
						usize end = p + 1;
						while (end < expression.size() && expression[end] != close) ++end;
						if (end < expression.size())
						{
							std::string target(expression.substr(p + 1, end - p - 1));
							bool angled = open == '<';
							protectedExpression += resolveInclude(target, angled, std::string(includingFile)).empty() ? '0' : '1';
							p = end + 1;
							while (p < expression.size() && (expression[p] == ' ' || expression[p] == '\t')) ++p;
							if (p < expression.size() && expression[p] == ')')
							{
								i = p + 1;
								continue;
							}
							_diagnostics.error(DiagId::IncludeExpectsTarget, location, "'__has_include' expects a ')' after its target");
							// One mistake, one diagnostic: abandon the rest of the directive rather
							// than re-scanning part of it as expression text.
							i = expression.size();
							continue;
						}
					}
				}
				_diagnostics.error(DiagId::IncludeExpectsTarget, location, "'__has_include' expects \"file\" or <file>");
				protectedExpression += '0';
				i = expression.size();
				continue;
			}
			if ((i == 0 || !isIdentifierChar(expression[i - 1])) &&
				expression.substr(i).starts_with("defined") &&
				(i + 7 == expression.size() || !isIdentifierChar(expression[i + 7])))
			{
				usize p = i + 7;
				while (p < expression.size() && (expression[p] == ' ' || expression[p] == '\t')) ++p;
				bool paren = p < expression.size() && expression[p] == '(';
				if (paren) ++p;
				while (p < expression.size() && (expression[p] == ' ' || expression[p] == '\t')) ++p;
				usize start = p; while (p < expression.size() && isIdentifierChar(expression[p])) ++p;
				if (start != p)
				{
					std::string name(expression.substr(start, p - start));
					while (p < expression.size() && (expression[p] == ' ' || expression[p] == '\t')) ++p;
					if (!paren || (p < expression.size() && expression[p] == ')'))
					{
						protectedExpression += _macros.contains(name) ? '1' : '0';
						i = paren ? p + 1 : p;
						continue;
					}
				}
			}
			protectedExpression += expression[i++];
		}
		bool comments = false; std::string expanded = expandMacros(protectedExpression, location, comments);
		IfExpressionParser parser(expanded, _macros, _diagnostics, location); return parser.parse(value);
	}
	std::string Preprocessor::expandMacros(std::string_view line, support::SourceLocation location, bool& inBlockComment)
	{
		std::string current(line);
		for (int pass = 0; pass < kMaxMacroPasses; ++pass)
		{
			bool commentState = pass == 0 && inBlockComment, changed = false; std::string next;
			for (usize i = 0; i < current.size();)
			{
				char c = current[i];
				if (c == '"' || c == '\'') { char quote = c; next += c; ++i; while (i < current.size()) { next += current[i]; if (current[i] == '\\' && i + 1 < current.size()) { next += current[++i]; } else if (current[i] == quote) { ++i; break; } ++i; } continue; }
				if (c == '/' && i + 1 < current.size() && current[i + 1] == '/') { next.append(current, i, std::string::npos); break; }
				if (commentState || (c == '/' && i + 1 < current.size() && current[i + 1] == '*')) { usize end = current.find("*/", i + (commentState ? 0 : 2)); usize stop = end == std::string::npos ? current.size() : end + 2; next.append(current, i, stop - i); i = stop; commentState = end == std::string::npos; continue; }
				if (!isIdentifierStart(c)) { next += c; ++i; continue; }
				usize start = i; while (i < current.size() && isIdentifierChar(current[i])) ++i; std::string name(current.substr(start, i - start));
				auto found = _macros.find(name); if (found == _macros.end()) { next += name; continue; }
				const Macro& macro = found->second;
				if (macro.builtin != Builtin::None) { next += expandBuiltin(macro.builtin, location); changed = true; continue; }
				if (!macro.functionLike) { next += macro.replacement.find("##") == std::string::npos ? macro.replacement : paste(macro.replacement); changed = true; continue; }
				usize call = i; while (call < current.size() && (current[call] == ' ' || current[call] == '\t')) ++call;
				if (call == current.size() || current[call] != '(') { next += name; continue; }
				usize pos = call + 1, argStart = pos, depth = 0; std::vector<std::string> args; bool closed = false;
				for (; pos < current.size(); ++pos) { char ch = current[pos]; if (ch == '"' || ch == '\'') { usize close = pos + 1; while (close < current.size() && current[close] != ch) { if (current[close] == '\\') ++close; ++close; } if (close < current.size()) { pos = close; continue; } } if (ch == '(') ++depth; else if (ch == ')' && depth-- == 0) { args.emplace_back(trim(current.substr(argStart, pos - argStart))); closed = true; ++pos; break; } else if (ch == ',' && depth == 0) { args.emplace_back(trim(current.substr(argStart, pos - argStart))); argStart = pos + 1; } }
				if (closed && args.size() == 1 && args.front().empty())
					args.clear();
				if (!closed || (!args.empty() && macro.parameters.empty() && !macro.variadic)) { _diagnostics.error(DiagId::MalformedMacroInvocation, location, "malformed invocation of macro '{}'", name); next += name; continue; }
				usize fixed = macro.parameters.size(); if ((!macro.variadic && args.size() != fixed) || (macro.variadic && args.size() < fixed)) { _diagnostics.error(DiagId::MacroArgumentCount, location, "macro '{}' expects {} argument(s), got {}", name, fixed + (macro.variadic ? 1 : 0), args.size()); next += name; continue; }
				std::string replacement = macro.replacement;
				// `raw[param]` is the argument verbatim. A bare occurrence - not `#`-stringified, not
				// `##`-pasted - substitutes the argument with its own macros resolved instead, which
				// C computes once per argument; the cache in `expanded` is that "once".
				std::unordered_map<std::string_view, std::string_view> raw;
				for (usize a = 0; a < fixed; ++a)
					raw.emplace(macro.parameters[a], args[a]);
				std::string variadicArgs;
				if (macro.variadic)
				{
					for (usize a = fixed; a < args.size(); ++a) { if (!variadicArgs.empty()) variadicArgs += ", "; variadicArgs += args[a]; }
					raw.emplace("__VA_ARGS__", variadicArgs);
				}
				std::unordered_map<std::string_view, std::string> expanded;
				auto expandedArgument = [&](std::string_view parameter) -> std::string_view
				{
					auto found = expanded.find(parameter);
					if (found == expanded.end()) { bool comments = false; found = expanded.emplace(parameter, expandMacros(raw.at(parameter), location, comments)).first; }
					return found->second;
				};
				auto pasteBefore = [&](usize at) { while (at > 0 && (replacement[at - 1] == ' ' || replacement[at - 1] == '\t')) --at; return at >= 2 && replacement[at - 2] == '#' && replacement[at - 1] == '#'; };
				auto pasteAfter = [&](usize at) { while (at < replacement.size() && (replacement[at] == ' ' || replacement[at] == '\t')) ++at; return at + 1 < replacement.size() && replacement[at] == '#' && replacement[at + 1] == '#'; };
				std::string substituted;
				for (usize j = 0; j < replacement.size();)
				{
					if (replacement[j] == '#' && j + 1 < replacement.size() && replacement[j + 1] == '#') { substituted += "##"; j += 2; continue; }
					if (replacement[j] == '#')
					{
						usize p = j + 1;
						while (p < replacement.size() && (replacement[p] == ' ' || replacement[p] == '\t')) ++p;
						if (p < replacement.size() && isIdentifierStart(replacement[p]))
						{
							usize s = p; while (p < replacement.size() && isIdentifierChar(replacement[p])) ++p;
							std::string_view word(replacement.data() + s, p - s);
							auto found = raw.find(word);
							if (found != raw.end()) { substituted += stringify(found->second); j = p; continue; }
						}
						substituted += replacement[j++];
						continue;
					}
					if (isIdentifierStart(replacement[j]))
					{
						usize s = j; while (j < replacement.size() && isIdentifierChar(replacement[j])) ++j;
						std::string_view word(replacement.data() + s, j - s);
						auto found = raw.find(word);
						if (found == raw.end()) { substituted.append(replacement, s, j - s); continue; }
						substituted += (pasteBefore(s) || pasteAfter(j)) ? found->second : expandedArgument(word);
						continue;
					}
					substituted += replacement[j++];
				}
				next += paste(std::move(substituted)); i = pos; changed = true;
			}
			current = std::move(next); if (current.size() > kMaxExpandedLineBytes) { _diagnostics.warning(DiagId::MacroExpansionByteLimit, location, "gave up expanding macros after {} bytes", kMaxExpandedLineBytes); return current; }
			if (pass == 0)
				inBlockComment = commentState;
			if (!changed)
				return current;
		}
		_diagnostics.warning(DiagId::MacroExpansionPassLimit, location, "gave up expanding macros after {} passes", kMaxMacroPasses); return current;
	}
	void Preprocessor::applyWarningPragma(std::string_view body, support::SourceLocation location,
		u32 outputLine, support::DiagnosticPolicy& policy)
	{
		using support::DiagnosticAction;

		// `warning(...)`: the parentheses are the whole of the syntax, so a body without them is
		// the one shape worth naming separately - it is almost always a missing pair rather than a
		// different idea.
		if (body.size() < 2 || body.front() != '(' || body.back() != ')')
		{
			policedWarning(DiagId::InvalidWarningPragma, location, outputLine, policy,
				"ignoring '#pragma warning {}': it takes a parenthesized body, as in "
				"'#pragma warning(disable: 2001)'", body);
			return;
		}
		std::string_view inner = trim(body.substr(1, body.size() - 2));
		if (inner == "push")
		{
			++_warningPragmaDepth;
			policy.push(outputLine);
			return;
		}
		if (inner == "pop")
		{
			// A pop with nothing pushed restores nothing, which is harmless but never what anyone
			// meant - it is a push deleted, or one that never made it out of a header.
			if (_warningPragmaDepth == 0)
			{
				policedWarning(DiagId::InvalidWarningPragma, location, outputLine, policy,
					"ignoring '#pragma warning(pop)': there is no matching '#pragma warning(push)'");
				return;
			}
			--_warningPragmaDepth;
			policy.pop(outputLine);
			return;
		}

		// Everything else is `<verb>: <number> <number> ...`. The verb is what MSVC calls it, and
		// means what MSVC means by it, with one addition of its own: `default` here restores what
		// the COMMAND LINE said, which is the only baseline this compiler has.
		usize colon = inner.find(':');
		std::string_view verb = trim(inner.substr(0, colon == std::string_view::npos ? inner.size() : colon));
		DiagnosticAction action = DiagnosticAction::Default;
		if (verb == "disable")      action = DiagnosticAction::Ignored;
		else if (verb == "default") action = DiagnosticAction::Default;
		else if (verb == "enable")  action = DiagnosticAction::Warning;
		else if (verb == "error")   action = DiagnosticAction::Error;
		else
		{
			policedWarning(DiagId::InvalidWarningPragma, location, outputLine, policy,
				"ignoring '#pragma warning({})': expected 'disable', 'default', 'enable', 'error', "
				"'push' or 'pop'", inner);
			return;
		}
		if (colon == std::string_view::npos)
		{
			policedWarning(DiagId::InvalidWarningPragma, location, outputLine, policy,
				"ignoring '#pragma warning({})': '{}' needs a ':' and at least one warning number",
				inner, verb);
			return;
		}

		// A list, separated by spaces or commas or both - `disable: 1002 2001` and
		// `disable: 1002, 2001` are the same thing, because both spellings are what people write.
		std::string_view list = inner.substr(colon + 1);
		for (usize i = 0; i < list.size();)
		{
			if (list[i] == ' ' || list[i] == '\t' || list[i] == ',')
			{
				++i;
				continue;
			}
			usize start = i;
			// An optional `W`, so both the code as it is PRINTED (W2001) and the bare number work.
			if (list[i] == 'W' || list[i] == 'w')
				++i;
			usize digits = i;
			while (i < list.size() && list[i] >= '0' && list[i] <= '9')
				++i;
			std::string_view word = list.substr(start, i - start);
			if (digits == i)
			{
				// Not a number at all. `all` is the one word allowed here.
				while (i < list.size() && list[i] != ' ' && list[i] != '\t' && list[i] != ',')
					++i;
				word = list.substr(start, i - start);
				if (word == "all")
				{
					policy.set(outputLine, support::DiagnosticPolicy::kAll, action);
					continue;
				}
				policedWarning(DiagId::InvalidWarningPragma, location, outputLine, policy,
					"ignoring '{}' in '#pragma warning({})': expected a warning number or 'all'", word, inner);
				continue;
			}

			u16 number = 0;
			std::string_view text = list.substr(digits, i - digits);
			std::from_chars(text.data(), text.data() + text.size(), number);
			if (std::optional<support::DiagnosticId> id = support::warningWithNumber(number))
			{
				policy.set(outputLine, support::diagnosticNumber(*id), action);
				continue;
			}
			// A number that names an error, or nothing at all. Saying which of the two it is would
			// mean a second table; saying that it is not a warning is what the program needs to
			// hear either way, because that is the whole of what this pragma can do.
			policedWarning(DiagId::UncontrollableDiagnostic, location, outputLine, policy,
				"'{}' does not name a warning, so '#pragma warning({}: {})' does nothing - "
				"only warnings can be turned off, and an error is not one", word, verb, word);
		}
	}

	bool Preprocessor::expandFile(const std::string& path, PreprocessedSource& out, std::vector<std::string>& includeStack)
	{
		std::string canonical = canonicalPath(path); if (std::find(_pragmaOnce.begin(), _pragmaOnce.end(), canonical) != _pragmaOnce.end()) return true;
		if (std::find(includeStack.begin(), includeStack.end(), canonical) != includeStack.end()) { _diagnostics.error(DiagId::IncludeCycle, {}, "include cycle: '{}' includes itself", path); return false; }
		if (includeStack.size() >= kMaxIncludeDepth) { _diagnostics.error(DiagId::IncludeTooDeep, {}, "#include nested more than {} deep, starting at '{}'", kMaxIncludeDepth, path); return false; }
		std::ifstream input(path, std::ios::binary); if (!input) { _diagnostics.error(DiagId::CannotOpenFile, {}, "cannot open '{}'", path); return false; }
		std::string contents{ std::istreambuf_iterator<char>(input), {} }; support::SourceId sourceId = _sourceManager.registerBuffer(path, std::move(contents)); const auto* buffer = _sourceManager.getBuffer(sourceId); if (!buffer) return false;
		includeStack.push_back(canonical); u32 previousIncludeLevel = _includeLevel; _includeLevel = static_cast<u32>(includeStack.size()) - 1;
		bool ok = true, inBlockComment = false; std::vector<Conditional> conditionals; u32 sourceLine = 0, logicalLine = 0; std::string_view text = buffer->buffer();
		auto active = [&] { return conditionals.empty() || conditionals.back().active; };
		auto blank = [&] { out.lineMap.append(static_cast<u32>(out.lineMap.entries().size() + 1), sourceId, logicalLine); out.text += '\n'; };
		for (usize position = 0; position < text.size();)
		{
			// One logical line, spliced from as many physical lines as end in a continuation
			// backslash. `logicalLine` names the first physical line the text came from, which is
			// where __LINE__ and the line map both point; `sourceLine` still advances over every
			// physical line so the next logical line is numbered correctly.
			usize newline = text.find('\n', position); bool last = newline == std::string_view::npos;
			std::string_view line = text.substr(position, (last ? text.size() : newline) - position);
			position = last ? text.size() : newline + 1; logicalLine = ++sourceLine;
			std::string spliced;
			if (!last && continuesWithBackslash(line))
			{
				spliced = stripContinuation(line);
				while (position < text.size())
				{
					newline = text.find('\n', position); last = newline == std::string_view::npos;
					std::string_view physical = text.substr(position, (last ? text.size() : newline) - position);
					position = last ? text.size() : newline + 1; ++sourceLine;
					if (!last && continuesWithBackslash(physical))
						spliced += stripContinuation(physical);
					else
					{
						spliced += physical;
						break;
					}
				}
				line = spliced;
			}
			support::SourceLocation here{ sourceId, logicalLine, 1, 0 }; std::string_view trimmed = trim(line);
			if (!inBlockComment && !trimmed.empty() && trimmed.front() == '#')
			{
				std::string_view directive = withoutDirectiveComment(trim(trimmed.substr(1)));
				auto condition = [&](bool value) { bool parent = active(); conditionals.push_back({ parent, parent && value, value, false, here }); };
				if (isDirective(directive, "if")) { i64 value = 0; bool valid = evaluateIfExpression(trim(directive.substr(2)), here, path, value); condition(valid && value != 0); blank(); continue; }
				if (isDirective(directive, "ifdef") || isDirective(directive, "ifndef")) { bool negated = isDirective(directive, "ifndef"); std::string_view name = trim(directive.substr(negated ? 6 : 5)); if (name.empty() || !isIdentifierStart(name.front()) || std::any_of(name.begin()+1, name.end(), [](char c){ return !isIdentifierChar(c); })) { _diagnostics.error(DiagId::ConditionalExpectsMacroName, here, "#{} expects a macro name", negated ? "ifndef" : "ifdef"); ok = false; condition(false); } else condition(_macros.contains(std::string(name)) != negated); blank(); continue; }
				if (isDirective(directive, "elif")) { if (conditionals.empty() || conditionals.back().sawElse) { _diagnostics.error(DiagId::ElifWithoutIf, here, "#elif without a matching #if"); ok = false; } else { Conditional& c = conditionals.back(); i64 value = 0; bool valid = evaluateIfExpression(trim(directive.substr(4)), here, path, value); c.active = c.parentActive && !c.branchTaken && valid && value != 0; c.branchTaken = c.branchTaken || (valid && value != 0); } blank(); continue; }
				if (isDirective(directive, "else")) { if (conditionals.empty() || conditionals.back().sawElse) { _diagnostics.error(DiagId::ElseWithoutIf, here, "#else without a matching #if"); ok = false; } else { Conditional& c = conditionals.back(); c.active = c.parentActive && !c.branchTaken; c.branchTaken = true; c.sawElse = true; } blank(); continue; }
				if (isDirective(directive, "endif")) { if (conditionals.empty()) { _diagnostics.error(DiagId::EndifWithoutIf, here, "#endif without a matching #if"); ok = false; } else conditionals.pop_back(); blank(); continue; }
				if (!active()) { blank(); continue; }
				if (isDirective(directive, "include")) { std::string_view target = trim(directive.substr(7)); bool angled = !target.empty() && target.front() == '<'; char close = angled ? '>' : '"'; if (target.size() < 2 || (target.front() != '<' && target.front() != '"') || target.back() != close) { _diagnostics.error(DiagId::IncludeExpectsTarget, here, "#include expects \"file.h\" or <file.h>"); ok = false; blank(); continue; } std::string resolved = resolveInclude(target.substr(1, target.size()-2), angled, path); if (resolved.empty()) { _diagnostics.error(DiagId::IncludeNotFound, here, "cannot find include file '{}'", target); ok = false; blank(); } else ok = expandFile(resolved, out, includeStack) && ok; continue; }
				if (isDirective(directive, "define")) { std::string_view rest = trim(directive.substr(6)); usize end = 0; while (end < rest.size() && isIdentifierChar(rest[end])) ++end; if (!end || !isIdentifierStart(rest[0])) { _diagnostics.error(DiagId::DefineExpectsName, here, "#define expects a name"); ok = false; blank(); continue; } Macro macro; std::string name(rest.substr(0, end)); if (end < rest.size() && rest[end] == '(') { macro.functionLike = true; usize p = end + 1; while (p < rest.size() && rest[p] != ')') { while (p < rest.size() && (rest[p] == ' ' || rest[p] == '\t' || rest[p] == ',')) ++p; if (rest.substr(p).starts_with("...")) { macro.variadic = true; p += 3; break; } usize s = p; while (p < rest.size() && isIdentifierChar(rest[p])) ++p; if (s == p || !isIdentifierStart(rest[s])) { _diagnostics.error(DiagId::InvalidMacroParameterList, here, "invalid macro parameter list"); ok = false; break; } macro.parameters.emplace_back(rest.substr(s, p-s)); } if (p >= rest.size() || rest[p] != ')') { _diagnostics.error(DiagId::UnterminatedMacroParameterList, here, "unterminated macro parameter list"); ok = false; } else end = p + 1; } macro.replacement = std::string(withoutDirectiveComment(trim(rest.substr(end)))); _macros[std::move(name)] = std::move(macro); blank(); continue; }
				if (isDirective(directive, "undef")) { std::string_view name = trim(directive.substr(5)); if (name.empty() || !isIdentifierStart(name.front()) || std::any_of(name.begin()+1, name.end(), [](char c){ return !isIdentifierChar(c); })) { _diagnostics.error(DiagId::UndefExpectsName, here, "#undef expects a name"); ok = false; } else _macros.erase(std::string(name)); blank(); continue; }
				if (isDirective(directive, "error") || isDirective(directive, "warning"))
				{
					bool fatal = isDirective(directive, "error");
					std::string_view message = trim(directive.substr(fatal ? 5 : 7));
					if (fatal)
					{
						_diagnostics.error(DiagId::UserError, here, "{}", message);
						ok = false;
					}
					else
					{
						policedWarning(DiagId::UserWarning, here,
							static_cast<u32>(out.lineMap.entries().size() + 1), out.diagnosticPolicy, "{}", message);
					}
					blank();
					continue;
				}
						if (isDirective(directive, "pragma"))
				{
					std::string_view body = trim(directive.substr(6));
					// The line this pragma governs from is the NEXT one it writes, which is the one
					// the blank() below is about to become.
					u32 outputLine = static_cast<u32>(out.lineMap.entries().size() + 1);
					if (body == "once")
						_pragmaOnce.push_back(canonical);
					else if (isDirective(body, "warning"))
						applyWarningPragma(trim(body.substr(7)), here, outputLine, out.diagnosticPolicy);
					else
						policedWarning(DiagId::UnknownPragma, here, outputLine, out.diagnosticPolicy,
							"ignoring unknown pragma '{}'", body);
					blank();
					continue;
				}
				if (directive.empty()) { blank(); continue; }
				std::string_view word = directive.substr(0, directive.find_first_of(" \t")); _diagnostics.error(DiagId::UnsupportedDirective, here, "'#{}' is not supported", word); ok = false; blank(); continue;
			}
			if (active()) { out.lineMap.append(static_cast<u32>(out.lineMap.entries().size()+1), sourceId, logicalLine); out.text += expandMacros(line, here, inBlockComment); out.text += '\n'; } else blank();
		}
		for (const Conditional& c : conditionals) { _diagnostics.error(DiagId::UnterminatedConditional, c.location, "unterminated conditional directive"); ok = false; }
		includeStack.pop_back(); _includeLevel = previousIncludeLevel; return ok;
	}
}
