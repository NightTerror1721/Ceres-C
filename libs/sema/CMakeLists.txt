# ceresc-sema - the type checker and symbol table (§8 of the architecture plan).
#
# Walks the parser's AST and produces the same tree, annotated: every Expr with its resolved
# Type*, every VarDecl with its frame offset or global address.

ceresc_add_library(sema DEPENDS ast support)
# lexer+parser are test-only (see ceresc_add_tests()'s own note): sema's tests build their fixture
# ASTs by parsing source text, but the sema library itself never depends on either.
ceresc_add_tests(sema DEPENDS lexer parser)
