# ceresc-lexer - turns a source buffer into Token[] (§5 of the architecture plan).
#
# Doesn't know the grammar: it knows that `if` is a keyword, not that `if` needs parentheses.

ceresc_add_library(lexer DEPENDS support)
ceresc_add_tests(lexer)
