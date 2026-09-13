#pragma once

#include "source_location.h"
#include <string>
#include <vector>
#include <expected>
#include <format>
#include <span>
#include <algorithm>
#include <utility>

// DiagnosticEngine - collects errors/warnings from every pipeline stage into a single list, with
// severity, SourceLocation and message.
//
// Deliberately does not throw: each phase (lexer, parser, sema, ...) decides for itself whether
// it can keep going after recording a diagnostic, which is what lets the driver report every
// error in a broken file in one pass instead of one exception at a time. Modeled directly on
// ceres::casm::AssemblerErrorHandler in CeresASM. See the architecture plan, §4.
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
		Location location = {};
		std::string message = {};

		Diagnostic() = default;
		Diagnostic(const Diagnostic&) = default;
		Diagnostic(Diagnostic&&) = default;
		~Diagnostic() = default;

		Diagnostic& operator=(const Diagnostic&) = default;
		Diagnostic& operator=(Diagnostic&&) = default;

		Diagnostic(Severity severity, Location location, std::string&& message) noexcept :
			severity(severity), location(location), message(std::move(message))
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
			if (diagnostic.severity == DiagnosticSeverity::Error || (_warningsAsErrors && diagnostic.severity == DiagnosticSeverity::Warning))
				++_errorCount;
			_diagnostics.push_back(std::move(diagnostic));
		}

		template <typename... Args>
		void report(DiagnosticSeverity severity, SourceLocation location, std::format_string<Args...> format, Args&&... args)
		{
			std::string message = std::vformat(format.get(), std::make_format_args(args...));
			report(Diagnostic(severity, location, std::move(message)));
		}

		template <typename... Args>
		void error(SourceLocation location, std::format_string<Args...> format, Args&&... args)
		{
			report(DiagnosticSeverity::Error, location, format, std::forward<Args>(args)...);
		}

		template <typename... Args>
		void warning(SourceLocation location, std::format_string<Args...> format, Args&&... args)
		{
			report(DiagnosticSeverity::Warning, location, format, std::forward<Args>(args)...);
		}

		void clear() noexcept
		{
			_diagnostics.clear();
			_errorCount = 0;
		}
	};
}
