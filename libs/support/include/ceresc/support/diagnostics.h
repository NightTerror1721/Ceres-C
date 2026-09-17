#pragma once

#include "diagnostic_id.h"
#include "diagnostic_policy.h"
#include "source_location.h"
#include <string>
#include <vector>
#include <expected>
#include <format>
#include <span>
#include <algorithm>
#include <utility>

// DiagnosticEngine - collects errors/warnings from every pipeline stage into a single list, with
// severity, DiagnosticId, SourceLocation and message.
//
// Deliberately does not throw: each phase (lexer, parser, sema, ...) decides for itself whether
// it can keep going after recording a diagnostic, which is what lets the driver report every
// error in a broken file in one pass instead of one exception at a time. Modeled directly on
// ceres::casm::AssemblerErrorHandler in CeresASM. See the architecture plan, §4.
//
// Every diagnostic carries a DiagnosticId (diagnostic_id.h), which is what a message's printed
// `E3023`/`W2001` code comes from and what a program's `#pragma warning(...)` names. The id is a
// required argument rather than a defaulted one on purpose: a new call site has to say which kind
// of thing it is reporting, and "which kind" is a question the author of the message is the only
// one able to answer.
//
// Also home to Result<T> (an alias of std::expected<T, Diagnostic>) for operations that can fail
// in isolation, e.g. resolving a type.

namespace ceresc::support
{
	enum class DiagnosticSeverity : u8
	{
		Error,
		Warning
	};

	struct Diagnostic
	{
		using Severity = DiagnosticSeverity;
		using Location = SourceLocation;

		Severity severity = Severity::Error;
		DiagnosticId id = DiagnosticId::None;
		Location location = {};
		std::string message = {};

		Diagnostic() = default;
		Diagnostic(const Diagnostic&) = default;
		Diagnostic(Diagnostic&&) = default;
		~Diagnostic() = default;

		Diagnostic& operator=(const Diagnostic&) = default;
		Diagnostic& operator=(Diagnostic&&) = default;

		Diagnostic(Severity severity, DiagnosticId id, Location location, std::string&& message) noexcept :
			severity(severity), id(id), location(location), message(std::move(message))
		{}
	};

	template <typename T>
	using Result = std::expected<T, Diagnostic>;

	class DiagnosticEngine
	{
	private:
		std::vector<Diagnostic> _diagnostics;
		usize _errorCount = 0;
		bool _warningsAsErrors = false;	// -Werror
		const DiagnosticPolicy* _policy = nullptr; // nullable - see setPolicy()

	public:
		DiagnosticEngine() = default;
		DiagnosticEngine(const DiagnosticEngine&) = delete;
		DiagnosticEngine(DiagnosticEngine&&) = default;
		~DiagnosticEngine() = default;

		DiagnosticEngine& operator=(const DiagnosticEngine&) = delete;
		DiagnosticEngine& operator=(DiagnosticEngine&&) = default;

	public:
		void setWarningsAsErrors(bool enabled) noexcept { _warningsAsErrors = enabled; }
		bool warningsAsErrors() const noexcept { return _warningsAsErrors; }

		// What the program's own `#pragma warning(...)` asked for (diagnostic_policy.h). Applies to
		// warnings whose location is in the buffer the policy was built for, and to nothing else -
		// so a phase driven from a plain string, or a diagnostic about a different translation
		// unit, is unaffected by one. Null until the driver has a policy to hand over.
		void setPolicy(const DiagnosticPolicy* policy) noexcept { _policy = policy; }

		std::span<const Diagnostic> diagnostics() const noexcept { return _diagnostics; }

		usize errorCount() const noexcept { return _errorCount; }
		usize warningCount() const noexcept { return _diagnostics.size() - _errorCount; }
		usize diagnosticCount() const noexcept { return _diagnostics.size(); }

		bool hasErrors() const noexcept { return _errorCount > 0; }
		bool hasWarnings() const noexcept { return _diagnostics.size() > _errorCount; }
		bool hasDiagnostics() const noexcept { return !_diagnostics.empty(); }

		void sortByLocation()
		{
			std::stable_sort(_diagnostics.begin(), _diagnostics.end(), [](const Diagnostic& a, const Diagnostic& b) {
				return a.location < b.location;
			});
		}

		void report(Diagnostic&& diagnostic)
		{
			bool promoted = _warningsAsErrors;
			if (diagnostic.severity == DiagnosticSeverity::Warning && _policy &&
				diagnostic.location.sourceId == _policy->controlledSourceId())
			{
				switch (_policy->actionFor(diagnostic.id, diagnostic.location.line))
				{
					case DiagnosticAction::Ignored:
						return; // never recorded, so nothing counts it and nothing prints it
					case DiagnosticAction::Warning:
						promoted = false; // an explicit `enable` outranks -Werror
						break;
					case DiagnosticAction::Error:
						// Not merely counted as one, the way -Werror does it: the program asked for
						// THIS diagnostic to be an error, so it says "error" when it prints. The
						// code still reads W####, which is what says which warning was promoted.
						diagnostic.severity = DiagnosticSeverity::Error;
						break;
					case DiagnosticAction::Default:
						break;
				}
			}

			if (diagnostic.severity == DiagnosticSeverity::Error ||
				(promoted && diagnostic.severity == DiagnosticSeverity::Warning))
				++_errorCount;
			_diagnostics.push_back(std::move(diagnostic));
		}

		template <typename... Args>
		void report(DiagnosticSeverity severity, DiagnosticId id, SourceLocation location,
			std::format_string<Args...> format, Args&&... args)
		{
			std::string message = std::vformat(format.get(), std::make_format_args(args...));
			report(Diagnostic(severity, id, location, std::move(message)));
		}

		template <typename... Args>
		void error(DiagnosticId id, SourceLocation location, std::format_string<Args...> format, Args&&... args)
		{
			report(DiagnosticSeverity::Error, id, location, format, std::forward<Args>(args)...);
		}

		template <typename... Args>
		void warning(DiagnosticId id, SourceLocation location, std::format_string<Args...> format, Args&&... args)
		{
			report(DiagnosticSeverity::Warning, id, location, format, std::forward<Args>(args)...);
		}

		void clear() noexcept
		{
			_diagnostics.clear();
			_errorCount = 0;
		}
	};
}
