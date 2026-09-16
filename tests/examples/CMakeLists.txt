# The examples suite (§12's CI rule, and a Fase 8 deliverable): every program under examples/ is
# compiled, assembled and run on every build, and its output compared against a sibling .expected.
#
# Built exactly like tests/e2e: it links libs/driver (it compiles through the real pipeline rather
# than shelling out to the ceresc binary, so a failure lands in a debugger) and drives `ceres asm`
# and `ceres run` as real subprocesses through tests/framework/ceres_tool.h.

file(GLOB sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

if(NOT sources)
	return()
endif()

if(NOT TARGET ceresc_test_main)
	message(STATUS "ceresc: examples tests exist, but tests/framework/main.cpp is missing - suite skipped")
	return()
endif()

add_executable(ceresc_examples_tests ${sources})

target_include_directories(ceresc_examples_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# The suite reads the real examples/ directory out of the source tree at run time rather than
# copying it next to the binary: a golden file edited by hand is then tested by the very next run,
# with no build step in between to forget about.
target_compile_definitions(ceresc_examples_tests
	PRIVATE "CERESC_EXAMPLES_DIR=\"${CMAKE_SOURCE_DIR}/examples\"")

target_link_libraries(ceresc_examples_tests
	PRIVATE
		ceresc::driver
		ceresc::test_main
		ceresc::warnings)

set_target_properties(ceresc_examples_tests PROPERTIES FOLDER "tests")

add_test(NAME examples COMMAND ceresc_examples_tests)
