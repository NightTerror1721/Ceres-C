#include <ceresc/support/diagnostics.h>
#include <ceresc/support/source_location.h>

#include "framework.h"

#include <utility>

using namespace ceresc;
using namespace ceresc::support;

namespace
{
	SourceLocation locAt(u32 line, u32 column, u32 offset = 0)
	{
		return SourceLocation(SourceId::make(1), line, column, offset);
	}
}

TEST(diagnostics, reporting_an_error_counts_as_a_failure)
{
	DiagnosticEngine engine;
	engine.error(DiagnosticId::UnexpectedCharacter, locAt(1, 1), "something went wrong");

	CHECK(engine.hasErrors());
	CHECK(engine.hasDiagnostics());
	CHECK_EQ(engine.errorCount(), usize{ 1 });
	CHECK_EQ(engine.diagnosticCount(), usize{ 1 });
}

TEST(diagnostics, reporting_only_warnings_does_not_count_as_a_failure)
{
	DiagnosticEngine engine;
	engine.warning(DiagnosticId::ConstWithoutInitializer, locAt(1, 1), "unused variable '{}'", "x");

	CHECK(!engine.hasErrors());
	CHECK(engine.hasWarnings());
	CHECK(engine.hasDiagnostics());
	CHECK_EQ(engine.errorCount(), usize{ 0 });
}

TEST(diagnostics, message_formatting_interpolates_arguments)
{
	DiagnosticEngine engine;
	engine.error(DiagnosticId::UnexpectedCharacter, locAt(3, 7), "unknown type '{}'", "foo");

	CHECK_EQ(engine.diagnostics()[0].message, "unknown type 'foo'");
}

TEST(diagnostics, warnings_as_errors_promotes_warnings_to_failures)
{
	DiagnosticEngine engine;
	engine.setWarningsAsErrors(true);
	engine.warning(DiagnosticId::ConstWithoutInitializer, locAt(1, 1), "shadowed variable '{}'", "x");

	CHECK(engine.hasErrors());
	CHECK_EQ(engine.errorCount(), usize{ 1 });
	// the stored severity is still Warning - only whether it *counts* changes.
	CHECK(engine.diagnostics()[0].severity == DiagnosticSeverity::Warning);
}

TEST(diagnostics, warnings_as_errors_does_not_retroactively_affect_earlier_reports)
{
	DiagnosticEngine engine;
	engine.warning(DiagnosticId::ConstWithoutInitializer, locAt(1, 1), "first warning");
	engine.setWarningsAsErrors(true);
	engine.warning(DiagnosticId::ConstWithoutInitializer, locAt(2, 1), "second warning");

	// only the second warning was reported while the flag was on.
	CHECK_EQ(engine.errorCount(), usize{ 1 });
	CHECK_EQ(engine.diagnosticCount(), usize{ 2 });
}

TEST(diagnostics, sort_by_location_orders_diagnostics_by_position)
{
	DiagnosticEngine engine;
	engine.error(DiagnosticId::UnexpectedCharacter, locAt(10, 1), "reported first, but later in the file");
	engine.error(DiagnosticId::UnexpectedCharacter, locAt(3, 1), "reported second, but earlier in the file");
	engine.sortByLocation();

	auto diags = engine.diagnostics();
	CHECK_EQ(diags.size(), usize{ 2 });
	CHECK_EQ(diags[0].message, "reported second, but earlier in the file");
	CHECK_EQ(diags[1].message, "reported first, but later in the file");
}

TEST(diagnostics, sort_by_location_is_stable_for_ties_at_the_same_position)
{
	DiagnosticEngine engine;
	engine.error(DiagnosticId::UnexpectedCharacter, locAt(5, 1), "first reported at this exact position");
	engine.error(DiagnosticId::UnexpectedCharacter, locAt(5, 1), "second reported at this exact position");
	engine.sortByLocation();

	auto diags = engine.diagnostics();
	CHECK_EQ(diags[0].message, "first reported at this exact position");
	CHECK_EQ(diags[1].message, "second reported at this exact position");
}

TEST(diagnostics, clear_resets_everything)
{
	DiagnosticEngine engine;
	engine.error(DiagnosticId::UnexpectedCharacter, locAt(1, 1), "boom");
	engine.clear();

	CHECK(!engine.hasErrors());
	CHECK(!engine.hasDiagnostics());
	CHECK_EQ(engine.diagnosticCount(), usize{ 0 });
}

namespace
{
	// Mirrors the intended calling convention: Result<T> carries a single Diagnostic for a
	// localized failure, and the caller decides whether (and how) to feed it into the shared
	// DiagnosticEngine - the engine never learns about a failed Result on its own.
	Result<int> parsePositive(int value, SourceLocation loc)
	{
		if (value <= 0)
			return std::unexpected(Diagnostic(DiagnosticSeverity::Error, DiagnosticId::InvalidArraySize, loc, "expected a positive value"));
		return value;
	}
}

TEST(diagnostics, result_carries_a_diagnostic_on_failure_without_touching_the_engine)
{
	DiagnosticEngine engine;

	Result<int> ok = parsePositive(5, locAt(1, 1));
	CHECK(ok.has_value());
	CHECK_EQ(*ok, 5);

	Result<int> failed = parsePositive(-1, locAt(2, 1));
	CHECK(!failed.has_value());
	CHECK_EQ(engine.diagnosticCount(), usize{ 0 }); // Result<T> alone never reports anywhere

	// The caller decides to report it - matching how sema is meant to keep checking the rest of
	// the file after a type error even though it also reports it (§8 of the plan).
	engine.report(std::move(failed).error());
	CHECK(engine.hasErrors());
}
