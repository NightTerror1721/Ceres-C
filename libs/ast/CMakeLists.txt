# ceresc-ast - the AST node hierarchy and the Type system (§6 of the architecture plan).
#
# Deliberately passive: a data library with no dependency on the lexer or the parser that
# produces its nodes.

ceresc_add_library(ast DEPENDS support)
ceresc_add_tests(ast)
