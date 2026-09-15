#pragma once

#include <ceresc/driver/options.h>

// Driver - orchestrates lexer -> parser -> sema -> ir -> codegen and owns the ceresc command
// line. The only library allowed to spawn a process: `ceres asm`/`ceres run` are invoked as real
// subprocesses for --run, exactly as a user would from a terminal - never linked against.
//
// Stops the pipeline before IrBuilder/CodeGen if sema ends with at least one error (not just
// warnings): there is no point generating code for a program that does not type-check. apps/
// ceresc/src/main.cpp calls into here once this is implemented. See the architecture plan, §11.
//
// Implemented starting Fase 6 of the phased plan (§13) and completed in Fase 8.

namespace ceresc::driver
{
	// Runs the full pipeline for `options`. Returns the process exit code: 0 on success, non-zero
	// if any phase reported an error or a requested subprocess (`ceres asm`/`ceres run`) failed -
	// that subprocess's own exit code is passed straight through. Diagnostics/output go to
	// stderr/stdout directly.
	int run(const Options& options);
}
