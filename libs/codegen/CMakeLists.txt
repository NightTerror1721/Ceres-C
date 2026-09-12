# ceresc-codegen - IR -> CASM text (§10 of the architecture plan).
#
# The only library that knows the Ceres ABI, the real ISA and .casm syntax - everything upstream
# of it is target-agnostic.

ceresc_add_library(codegen DEPENDS ir sema support)
ceresc_add_tests(codegen)
