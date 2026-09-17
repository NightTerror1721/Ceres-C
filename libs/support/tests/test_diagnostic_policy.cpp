#include <ceresc/support/diagnostic_id.h>
#include <ceresc/support/diagnostic_policy.h>
#include <ceresc/support/diagnostics.h>

#include "framework.h"

using namespace ceresc;
using namespace ceresc::support;

namespace
{
	u16 numberOf(DiagnosticId id) { return diagnosticNumber(id); }
}

TEST(diagnostic_id, a_code_says_which_phase_found_it_and_how_bad_it_is)
{
	CHECK_EQ(diagnosticCode(DiagnosticId::UnexpectedCharacter), "E0014");      // lexer
	CHECK_EQ(diagnosticCode(DiagnosticId::IncludeNotFound), "E1012");          // preprocessor
	CHECK_EQ(diagnosticCode(DiagnosticId::ExpectedExpression), "E2005");       // parser
	CHECK_EQ(diagnosticCode(DiagnosticId::UndeclaredIdentifier), "E3023");     // sema
	CHECK_EQ(diagnosticCode(DiagnosticId::ReservedCasmWord), "E4004");         // codegen
	CHECK_EQ(diagnosticCode(DiagnosticId::CompoundAssignToStruct), "E5001");   // ir

	CHECK_EQ(diagnosticCode(DiagnosticId::CappedTypeWidth), "W2001");
	CHECK_EQ(diagnosticCode(DiagnosticId::ConstWithoutInitializer), "W3001");

	// An id with no code prints none rather than a meaningless one.
	CHECK_EQ(diagnosticCode(DiagnosticId::None), std::string());
}

TEST(diagnostic_id, an_error_and_a_warning_may_share_a_number_without_sharing_an_id)
{
	// E3001 and W3001 are both real and mean different things - which is the whole reason the
	// severity lives in the enum value's top bit rather than being derived from the number.
	CHECK_EQ(numberOf(DiagnosticId::Redefinition), u16(3001));
	CHECK_EQ(numberOf(DiagnosticId::ConstWithoutInitializer), u16(3001));
	CHECK(!isWarningId(DiagnosticId::Redefinition));
	CHECK(isWarningId(DiagnosticId::ConstWithoutInitializer));
	CHECK(DiagnosticId::Redefinition != DiagnosticId::ConstWithoutInitializer);
}

TEST(diagnostic_id, only_a_warning_can_be_named_by_a_number)
{
	CHECK(warningWithNumber(3001).has_value());               // W3001
	CHECK(*warningWithNumber(3001) == DiagnosticId::ConstWithoutInitializer);
	CHECK(!warningWithNumber(3023).has_value());              // E3023 is an error
	CHECK(!warningWithNumber(9999).has_value());              // and this is nothing at all
}

TEST(diagnostic_policy, a_change_governs_the_lines_after_it_and_not_the_ones_before)
{
	DiagnosticPolicy policy;
	policy.set(10, numberOf(DiagnosticId::CappedTypeWidth), DiagnosticAction::Ignored);

	CHECK(policy.actionFor(DiagnosticId::CappedTypeWidth, 9) == DiagnosticAction::Default);
	CHECK(policy.actionFor(DiagnosticId::CappedTypeWidth, 10) == DiagnosticAction::Ignored);
	CHECK(policy.actionFor(DiagnosticId::CappedTypeWidth, 99) == DiagnosticAction::Ignored);

	// And says nothing about any other diagnostic.
	CHECK(policy.actionFor(DiagnosticId::ConstWithoutInitializer, 99) == DiagnosticAction::Default);
}

TEST(diagnostic_policy, the_last_change_before_a_line_is_the_one_in_force)
{
	DiagnosticPolicy policy;
	u16 capped = numberOf(DiagnosticId::CappedTypeWidth);
	policy.set(1, capped, DiagnosticAction::Ignored);
	policy.set(5, capped, DiagnosticAction::Error);
	policy.set(9, capped, DiagnosticAction::Default);

	CHECK(policy.actionFor(DiagnosticId::CappedTypeWidth, 3) == DiagnosticAction::Ignored);
	CHECK(policy.actionFor(DiagnosticId::CappedTypeWidth, 7) == DiagnosticAction::Error);
	CHECK(policy.actionFor(DiagnosticId::CappedTypeWidth, 11) == DiagnosticAction::Default);
}

TEST(diagnostic_policy, all_matches_everything_and_a_later_specific_change_still_wins)
{
	DiagnosticPolicy policy;
	policy.set(1, DiagnosticPolicy::kAll, DiagnosticAction::Ignored);
	policy.set(5, numberOf(DiagnosticId::CappedTypeWidth), DiagnosticAction::Error);

	CHECK(policy.actionFor(DiagnosticId::ConstWithoutInitializer, 3) == DiagnosticAction::Ignored);
	CHECK(policy.actionFor(DiagnosticId::ConstWithoutInitializer, 7) == DiagnosticAction::Ignored);
	CHECK(policy.actionFor(DiagnosticId::CappedTypeWidth, 3) == DiagnosticAction::Ignored);
	CHECK(policy.actionFor(DiagnosticId::CappedTypeWidth, 7) == DiagnosticAction::Error);
}

TEST(diagnostic_policy, pop_restores_exactly_what_push_saw)
{
	DiagnosticPolicy policy;
	u16 capped = numberOf(DiagnosticId::CappedTypeWidth);
	policy.set(1, capped, DiagnosticAction::Ignored);
	policy.push(3);
	policy.set(4, capped, DiagnosticAction::Error);
	policy.push(6);
	policy.set(7, DiagnosticPolicy::kAll, DiagnosticAction::Default);
	policy.pop(9);
	policy.pop(11);

	CHECK(policy.actionFor(DiagnosticId::CappedTypeWidth, 2) == DiagnosticAction::Ignored);
	CHECK(policy.actionFor(DiagnosticId::CappedTypeWidth, 5) == DiagnosticAction::Error);
	CHECK(policy.actionFor(DiagnosticId::CappedTypeWidth, 8) == DiagnosticAction::Default);
	CHECK(policy.actionFor(DiagnosticId::CappedTypeWidth, 10) == DiagnosticAction::Error);   // one pop
	CHECK(policy.actionFor(DiagnosticId::CappedTypeWidth, 12) == DiagnosticAction::Ignored); // both
}

TEST(diagnostic_policy, an_error_is_never_governed_by_one)
{
	// `all` means all WARNINGS. A program that does not compile does not compile.
	DiagnosticPolicy policy;
	policy.set(1, DiagnosticPolicy::kAll, DiagnosticAction::Ignored);
	CHECK(policy.actionFor(DiagnosticId::UndeclaredIdentifier, 5) == DiagnosticAction::Default);
}

TEST(diagnostic_policy, the_engine_applies_it_only_to_the_buffer_it_was_built_for)
{
	SourceId controlled = SourceId::make(7);
	SourceId other = SourceId::make(8);

	DiagnosticPolicy policy;
	policy.setControlledSourceId(controlled);
	policy.set(1, numberOf(DiagnosticId::CappedTypeWidth), DiagnosticAction::Ignored);

	DiagnosticEngine engine;
	engine.setPolicy(&policy);
	engine.warning(DiagnosticId::CappedTypeWidth, SourceLocation(controlled, 5, 1, 0), "dropped");
	engine.warning(DiagnosticId::CappedTypeWidth, SourceLocation(other, 5, 1, 0), "kept");

	CHECK_EQ(engine.diagnosticCount(), usize{ 1 });
	CHECK_EQ(engine.diagnostics()[0].message, "kept");
}

TEST(diagnostic_policy, an_error_action_makes_the_diagnostic_say_error)
{
	SourceId controlled = SourceId::make(7);
	DiagnosticPolicy policy;
	policy.setControlledSourceId(controlled);
	policy.set(1, numberOf(DiagnosticId::CappedTypeWidth), DiagnosticAction::Error);

	DiagnosticEngine engine;
	engine.setPolicy(&policy);
	engine.warning(DiagnosticId::CappedTypeWidth, SourceLocation(controlled, 5, 1, 0), "promoted");

	// Not merely counted as one, the way -Werror does it - the program asked for this one.
	CHECK(engine.hasErrors());
	CHECK(engine.diagnostics()[0].severity == DiagnosticSeverity::Error);
	CHECK_EQ(diagnosticCode(engine.diagnostics()[0].id), "W2001"); // and still says which warning
}

TEST(diagnostic_policy, an_explicit_enable_outranks_warnings_as_errors)
{
	SourceId controlled = SourceId::make(7);
	DiagnosticPolicy policy;
	policy.setControlledSourceId(controlled);
	policy.set(1, numberOf(DiagnosticId::CappedTypeWidth), DiagnosticAction::Warning);

	DiagnosticEngine engine;
	engine.setWarningsAsErrors(true);
	engine.setPolicy(&policy);
	engine.warning(DiagnosticId::CappedTypeWidth, SourceLocation(controlled, 5, 1, 0), "still a warning");
	engine.warning(DiagnosticId::ConstWithoutInitializer, SourceLocation(controlled, 5, 1, 0), "promoted");

	CHECK_EQ(engine.diagnosticCount(), usize{ 2 });
	CHECK_EQ(engine.errorCount(), usize{ 1 }); // only the one the pragma said nothing about
}
