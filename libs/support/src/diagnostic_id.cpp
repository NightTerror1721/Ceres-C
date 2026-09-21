#include <ceresc/support/diagnostic_id.h>

#include <format>

// The two functions that need a table rather than arithmetic - see diagnostic_id.h for what a code
// is and how it is numbered.

namespace ceresc::support
{
	std::string diagnosticCode(DiagnosticId id)
	{
		if (id == DiagnosticId::None)
			return {};
		return std::format("{}{:04}", isWarningId(id) ? 'W' : 'E', diagnosticNumber(id));
	}

	std::optional<DiagnosticId> warningWithNumber(u16 number) noexcept
	{
		// Every controllable diagnostic, listed once. A switch rather than a loop over a table so
		// that adding a warning to diagnostic_id.h and forgetting this is a compile-time -Wswitch
		// complaint on the enum, not a silently uncontrollable warning.
		switch (static_cast<DiagnosticId>(number | kDiagnosticWarningBit))
		{
			case DiagnosticId::MacroExpansionByteLimit:
			case DiagnosticId::MacroExpansionPassLimit:
			case DiagnosticId::UserWarning:
			case DiagnosticId::UnknownPragma:
			case DiagnosticId::InvalidWarningPragma:
			case DiagnosticId::UncontrollableDiagnostic:
			case DiagnosticId::CappedTypeWidth:
			case DiagnosticId::AttributeIgnored:
			case DiagnosticId::NoReturnFunctionReturns:
			case DiagnosticId::ConstWithoutInitializer:
				return static_cast<DiagnosticId>(number | kDiagnosticWarningBit);
			default:
				return std::nullopt;
		}
	}
}
