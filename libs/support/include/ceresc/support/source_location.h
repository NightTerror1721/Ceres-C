#pragma once

#include "identifier.h"

// SourceLocation - a trivial, non-owning {fileId, line, column, offset} value.
//
// Every Token (libs/lexer) and every AST node (libs/ast) carries one of these so diagnostics.h
// can point at exactly where they came from, and so codegen can cite the originating C line in
// its comments. See the architecture plan, §4.
//
// Implemented in Fase 0 of the phased plan (§13): a comparable (<=>) aggregate, so a
// DiagnosticEngine can sort its collected diagnostics by position.

namespace ceresc::support
{
	using SourceIdGenerator = GenericIdentifierGenerator<u32>;
	using SourceId = SourceIdGenerator::IdentifierType;
	using LineNumber = u32;
	using ColumnNumber = u32;
	using Offset = u32;

	struct SourceLocation
	{
		SourceId	 sourceId = {};
		LineNumber	 line	  = 0;
		ColumnNumber column	  = 0;
		Offset		 offset	  = 0;

		constexpr SourceLocation() noexcept = default;
		constexpr SourceLocation(const SourceLocation&) noexcept = default;
		constexpr SourceLocation(SourceLocation&&) noexcept = default;
		constexpr ~SourceLocation() noexcept = default;

		constexpr SourceLocation& operator=(const SourceLocation&) noexcept = default;
		constexpr SourceLocation& operator=(SourceLocation&&) noexcept = default;

		constexpr bool operator==(const SourceLocation&) const noexcept = default;
		constexpr auto operator<=>(const SourceLocation&) const noexcept = default;

		constexpr SourceLocation(SourceId sourceId, LineNumber line, ColumnNumber column, Offset offset) noexcept :
			sourceId(sourceId),
			line(line),
			column(column),
			offset(offset)
		{}

		constexpr bool isValid() const noexcept { return sourceId && line > 0 && column > 0; }
		constexpr bool isInvalid() const noexcept { return !sourceId || line == 0 || column == 0; }

		constexpr explicit operator bool() const noexcept { return sourceId && line > 0 && column > 0; }
		constexpr bool operator!() const noexcept { return !sourceId || line == 0 || column == 0; }
	};
}

template <>
struct std::hash<ceresc::support::SourceLocation>
{
	constexpr std::size_t operator()(const ceresc::support::SourceLocation& loc) const noexcept
	{
		std::size_t h1 = std::hash<ceresc::support::SourceId>{}(loc.sourceId);
		std::size_t h2 = std::hash<ceresc::support::LineNumber>{}(loc.line);
		std::size_t h3 = std::hash<ceresc::support::ColumnNumber>{}(loc.column);
		std::size_t h4 = std::hash<ceresc::support::Offset>{}(loc.offset);
		return h1 ^ (h2 << 1) ^ (h3 << 2) ^ (h4 << 3);
	}
};
