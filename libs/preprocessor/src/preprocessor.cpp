#include <ceresc/preprocessor/preprocessor.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>

namespace ceresc::preprocessor
{
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
			void fail(std::string_view message) { if (_ok) _diagnostics.error(_location, "{}", message); _ok = false; }
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
	bool Preprocessor::evaluateIfExpression(std::string_view expression, support::SourceLocation location, i64& value)
	{
		// `defined(NAME)` is recognized before ordinary substitution so NAME remains an identifier
		// even when it is itself an object-like macro.
		std::string protectedExpression;
		for (usize i = 0; i < expression.size();)
		{
			if (expression.substr(i).starts_with("defined") &&
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
				if (!macro.functionLike) { next += macro.replacement; changed = true; continue; }
				usize call = i; while (call < current.size() && (current[call] == ' ' || current[call] == '\t')) ++call;
				if (call == current.size() || current[call] != '(') { next += name; continue; }
				usize pos = call + 1, argStart = pos, depth = 0; std::vector<std::string> args; bool closed = false;
				for (; pos < current.size(); ++pos) { char ch = current[pos]; if (ch == '(') ++depth; else if (ch == ')' && depth-- == 0) { args.emplace_back(trim(current.substr(argStart, pos - argStart))); closed = true; ++pos; break; } else if (ch == ',' && depth == 0) { args.emplace_back(trim(current.substr(argStart, pos - argStart))); argStart = pos + 1; } }
				if (closed && args.size() == 1 && args.front().empty())
					args.clear();
				if (!closed || (!args.empty() && macro.parameters.empty() && !macro.variadic)) { _diagnostics.error(location, "malformed invocation of macro '{}'", name); next += name; continue; }
				usize fixed = macro.parameters.size(); if ((!macro.variadic && args.size() != fixed) || (macro.variadic && args.size() < fixed)) { _diagnostics.error(location, "macro '{}' expects {} argument(s), got {}", name, fixed + (macro.variadic ? 1 : 0), args.size()); next += name; continue; }
				std::string replacement = macro.replacement;
				auto substitute = [&](std::string_view parameter, std::string_view value) { std::string r; for (usize j = 0; j < replacement.size();) { if (isIdentifierStart(replacement[j])) { usize s = j++; while (j < replacement.size() && isIdentifierChar(replacement[j])) ++j; std::string_view word(replacement.data() + s, j - s); r += word == parameter ? value : word; } else r += replacement[j++]; } replacement = std::move(r); };
				for (usize a = 0; a < fixed; ++a) substitute(macro.parameters[a], args[a]);
				if (macro.variadic) { std::string joined; for (usize a = fixed; a < args.size(); ++a) { if (!joined.empty()) joined += ", "; joined += args[a]; } substitute("__VA_ARGS__", joined); }
				next += replacement; i = pos; changed = true;
			}
			current = std::move(next); if (current.size() > kMaxExpandedLineBytes) { _diagnostics.warning(location, "gave up expanding macros after {} bytes", kMaxExpandedLineBytes); return current; }
			if (pass == 0)
				inBlockComment = commentState;
			if (!changed)
				return current;
		}
		_diagnostics.warning(location, "gave up expanding macros after {} passes", kMaxMacroPasses); return current;
	}
	bool Preprocessor::expandFile(const std::string& path, PreprocessedSource& out, std::vector<std::string>& includeStack)
	{
		std::string canonical = canonicalPath(path); if (std::find(_pragmaOnce.begin(), _pragmaOnce.end(), canonical) != _pragmaOnce.end()) return true;
		if (std::find(includeStack.begin(), includeStack.end(), canonical) != includeStack.end()) { _diagnostics.error({}, "include cycle: '{}' includes itself", path); return false; }
		if (includeStack.size() >= kMaxIncludeDepth) { _diagnostics.error({}, "#include nested more than {} deep, starting at '{}'", kMaxIncludeDepth, path); return false; }
		std::ifstream input(path, std::ios::binary); if (!input) { _diagnostics.error({}, "cannot open '{}'", path); return false; }
		std::string contents{ std::istreambuf_iterator<char>(input), {} }; support::SourceId sourceId = _sourceManager.registerBuffer(path, std::move(contents)); const auto* buffer = _sourceManager.getBuffer(sourceId); if (!buffer) return false;
		includeStack.push_back(canonical); u32 previousIncludeLevel = _includeLevel; _includeLevel = static_cast<u32>(includeStack.size()) - 1;
		bool ok = true, inBlockComment = false; std::vector<Conditional> conditionals; u32 sourceLine = 0; std::string_view text = buffer->buffer();
		auto active = [&] { return conditionals.empty() || conditionals.back().active; };
		auto blank = [&] { out.lineMap.append(static_cast<u32>(out.lineMap.entries().size() + 1), sourceId, sourceLine); out.text += '\n'; };
		for (usize position = 0; position < text.size();)
		{
			usize newline = text.find('\n', position); bool last = newline == std::string_view::npos; std::string_view line = text.substr(position, (last ? text.size() : newline) - position); position = last ? text.size() : newline + 1; ++sourceLine;
			support::SourceLocation here{ sourceId, sourceLine, 1, 0 }; std::string_view trimmed = trim(line);
			if (!inBlockComment && !trimmed.empty() && trimmed.front() == '#')
			{
				std::string_view directive = withoutDirectiveComment(trim(trimmed.substr(1)));
				auto condition = [&](bool value) { bool parent = active(); conditionals.push_back({ parent, parent && value, value, false, here }); };
				if (isDirective(directive, "if")) { i64 value = 0; bool valid = evaluateIfExpression(trim(directive.substr(2)), here, value); condition(valid && value != 0); blank(); continue; }
				if (isDirective(directive, "ifdef") || isDirective(directive, "ifndef")) { bool negated = isDirective(directive, "ifndef"); std::string_view name = trim(directive.substr(negated ? 6 : 5)); if (name.empty() || !isIdentifierStart(name.front()) || std::any_of(name.begin()+1, name.end(), [](char c){ return !isIdentifierChar(c); })) { _diagnostics.error(here, "#{} expects a macro name", negated ? "ifndef" : "ifdef"); ok = false; condition(false); } else condition(_macros.contains(std::string(name)) != negated); blank(); continue; }
				if (isDirective(directive, "elif")) { if (conditionals.empty() || conditionals.back().sawElse) { _diagnostics.error(here, "#elif without a matching #if"); ok = false; } else { Conditional& c = conditionals.back(); i64 value = 0; bool valid = evaluateIfExpression(trim(directive.substr(4)), here, value); c.active = c.parentActive && !c.branchTaken && valid && value != 0; c.branchTaken = c.branchTaken || (valid && value != 0); } blank(); continue; }
				if (isDirective(directive, "else")) { if (conditionals.empty() || conditionals.back().sawElse) { _diagnostics.error(here, "#else without a matching #if"); ok = false; } else { Conditional& c = conditionals.back(); c.active = c.parentActive && !c.branchTaken; c.branchTaken = true; c.sawElse = true; } blank(); continue; }
				if (isDirective(directive, "endif")) { if (conditionals.empty()) { _diagnostics.error(here, "#endif without a matching #if"); ok = false; } else conditionals.pop_back(); blank(); continue; }
				if (!active()) { blank(); continue; }
				if (isDirective(directive, "include")) { std::string_view target = trim(directive.substr(7)); bool angled = !target.empty() && target.front() == '<'; char close = angled ? '>' : '"'; if (target.size() < 2 || (target.front() != '<' && target.front() != '"') || target.back() != close) { _diagnostics.error(here, "#include expects \"file.h\" or <file.h>"); ok = false; blank(); continue; } std::string resolved = resolveInclude(target.substr(1, target.size()-2), angled, path); if (resolved.empty()) { _diagnostics.error(here, "cannot find include file '{}'", target); ok = false; blank(); } else ok = expandFile(resolved, out, includeStack) && ok; continue; }
				if (isDirective(directive, "define")) { std::string_view rest = trim(directive.substr(6)); usize end = 0; while (end < rest.size() && isIdentifierChar(rest[end])) ++end; if (!end || !isIdentifierStart(rest[0])) { _diagnostics.error(here, "#define expects a name"); ok = false; blank(); continue; } Macro macro; std::string name(rest.substr(0, end)); if (end < rest.size() && rest[end] == '(') { macro.functionLike = true; usize p = end + 1; while (p < rest.size() && rest[p] != ')') { while (p < rest.size() && (rest[p] == ' ' || rest[p] == '\t' || rest[p] == ',')) ++p; if (rest.substr(p).starts_with("...")) { macro.variadic = true; p += 3; break; } usize s = p; while (p < rest.size() && isIdentifierChar(rest[p])) ++p; if (s == p || !isIdentifierStart(rest[s])) { _diagnostics.error(here, "invalid macro parameter list"); ok = false; break; } macro.parameters.emplace_back(rest.substr(s, p-s)); } if (p >= rest.size() || rest[p] != ')') { _diagnostics.error(here, "unterminated macro parameter list"); ok = false; } else end = p + 1; } macro.replacement = std::string(withoutDirectiveComment(trim(rest.substr(end)))); _macros[std::move(name)] = std::move(macro); blank(); continue; }
				if (isDirective(directive, "undef")) { std::string_view name = trim(directive.substr(5)); if (name.empty() || !isIdentifierStart(name.front()) || std::any_of(name.begin()+1, name.end(), [](char c){ return !isIdentifierChar(c); })) { _diagnostics.error(here, "#undef expects a name"); ok = false; } else _macros.erase(std::string(name)); blank(); continue; }
				if (isDirective(directive, "error") || isDirective(directive, "warning")) { std::string_view message = trim(directive.substr(isDirective(directive, "error") ? 5 : 7)); if (isDirective(directive, "error")) { _diagnostics.error(here, "{}", message); ok = false; } else _diagnostics.warning(here, "{}", message); blank(); continue; }
				if (isDirective(directive, "pragma")) { if (trim(directive.substr(6)) == "once") _pragmaOnce.push_back(canonical); else _diagnostics.warning(here, "ignoring unknown pragma '{}'", trim(directive.substr(6))); blank(); continue; }
				if (directive.empty()) { blank(); continue; }
				std::string_view word = directive.substr(0, directive.find_first_of(" \t")); _diagnostics.error(here, "'#{}' is not supported", word); ok = false; blank(); continue;
			}
			if (active()) { out.lineMap.append(static_cast<u32>(out.lineMap.entries().size()+1), sourceId, sourceLine); out.text += expandMacros(line, here, inBlockComment); out.text += '\n'; } else blank();
		}
		for (const Conditional& c : conditionals) { _diagnostics.error(c.location, "unterminated conditional directive"); ok = false; }
		includeStack.pop_back(); _includeLevel = previousIncludeLevel; return ok;
	}
}
