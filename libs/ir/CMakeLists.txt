# ceresc-ir - the three-address-code intermediate representation (§9 of the architecture plan).
#
# Deliberately not SSA. Separates "what to compute" from "which physical register it lives in" -
# codegen is the only library that ever thinks about a real register.

ceresc_add_library(ir DEPENDS ast sema)
ceresc_add_tests(ir)
