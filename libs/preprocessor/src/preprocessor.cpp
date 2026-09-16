#include <ceresc/preprocessor/preprocessor.h>

#include <algorithm>
#include <charconv>
#include <filesystem>
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

	support::SourceLocation LineMap::toOriginal(support::SourceLocation location) const noexcept
	{
		if (_entries.empty() || location.line == 0 || location.line > _entries.size()) return location;
		const LineMapEntry& entry = _entries[location.line - 1]; return { entry.sourceId, entry.sourceLine, location.column, location.offset };
	}
	PreprocessedSource Preprocessor::run(const std::string& path)
	{
		_macros.clear();
		for (const auto& [name, replacement] : _predefines)
		{
			Macro macro;
			macro.replacement = replacement;
			_macros.emplace(name, std::move(macro));
		}
		_pragmaOnce.clear(); PreprocessedSource result; std::vector<std::string> stack; result.ok = expandFile(path, result, stack); return result;
	}
	std::string Preprocessor::resolveInclude(std::string_view target, bool angled, const std::string& includingFile) const
	{
		std::error_code error;
		if (!angled) { fs::path relative = fs::path(includingFile).parent_path() / std::string(target); if (fs::is_regular_file(relative, error)) return relative.string(); }
		for (const std::string& directory : _includeDirectories) { fs::path candidate = fs::path(directory) / std::string(target); if (fs::is_regular_file(candidate, error)) return candidate.string(); }
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
		includeStack.push_back(canonical); bool ok = true, inBlockComment = false; std::vector<Conditional> conditionals; u32 sourceLine = 0; std::string_view text = buffer->buffer();
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
		includeStack.pop_back(); return ok;
	}
}
