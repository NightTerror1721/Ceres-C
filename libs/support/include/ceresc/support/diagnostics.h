#pragma once

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
}
