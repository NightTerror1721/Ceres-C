# ceresc-driver - orchestrates the pipeline and owns the command line (§11 of the architecture
# plan).
#
# The only library allowed to spawn a process: `ceres asm`/`ceres run` are invoked as external
# subprocesses for --run, exactly as a user would from a terminal - never linked against.

ceresc_add_library(driver DEPENDS preprocessor parser sema ir codegen)
ceresc_add_tests(driver)
