# ceresc-codegen - IR -> CASM text (§10 of the architecture plan).
#
# The only library that knows the Ceres ABI, the real ISA and .casm syntax - everything upstream
# of it is target-agnostic.

ceresc_add_library(codegen DEPENDS ir sema support)
# lexer+parser are test-only (see ceresc_add_tests()'s own note): codegen's tests build their
# fixture programs by parsing and type-checking source text, but the codegen library itself never
# depends on either.
ceresc_add_tests(codegen DEPENDS lexer parser)
