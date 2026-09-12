# ceresc-parser - Token[] -> AST (§7 of the architecture plan).
#
# Recursive descent for declarations/statements, precedence climbing for expressions.

ceresc_add_library(parser DEPENDS lexer ast)
ceresc_add_tests(parser)
