# ceresc-ir - the three-address-code intermediate representation (§9 of the architecture plan).
#
# Deliberately not SSA. Separates "what to compute" from "which physical register it lives in" -
# codegen is the only library that ever thinks about a real register.

ceresc_add_library(ir DEPENDS ast sema)
# lexer+parser are test-only (see ceresc_add_tests()'s own note): ir's tests build their fixture
# ASTs by parsing and type-checking source text, but the ir library itself never depends on either.
ceresc_add_tests(ir DEPENDS lexer parser)
