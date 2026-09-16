# ceresc-preprocessor - the text-to-text pass that runs before the lexer (§5/§13's Fase 9).
#
# #include, #define (object-like), #undef and #pragma once, plus the line map that lets a
# diagnostic about an expanded line name the file it was really written in.
#
# Depends on support alone: it knows nothing about tokens, and the lexer knows nothing about it.

ceresc_add_library(preprocessor DEPENDS support)
ceresc_add_tests(preprocessor)
