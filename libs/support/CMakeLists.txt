# ceresc-support - the base every other library depends on (§4 of the architecture plan).
#
# SourceLocation, SourceManager, DiagnosticEngine, Arena, plus the shared sized-integer aliases
# (types.h). Nothing here knows what a token or a C type is - it's infrastructure, not language.

ceresc_add_library(support)
ceresc_add_tests(support)
