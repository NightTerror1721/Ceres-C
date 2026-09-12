# The end-to-end suite (§12 of the architecture plan): compile a real .c file, invoke `ceres asm`
# and `ceres run` as real subprocesses (no mocks), and compare their terminal output/exit code
# against a golden .expected file - the same rule §12 requires for everything under examples/.
#
# Empty until Fase 6 lands the first program that runs through the whole pipeline for real; see
# §13.

file(GLOB sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

if(NOT sources)
	return()
endif()

if(NOT TARGET ceresc_test_main)
	message(STATUS "ceresc: e2e tests exist, but tests/framework/main.cpp is missing - suite skipped")
	return()
endif()

add_executable(ceresc_e2e_tests ${sources})

target_include_directories(ceresc_e2e_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ceresc_e2e_tests
	PRIVATE
		ceresc::driver
		ceresc::test_main
		ceresc::warnings)

set_target_properties(ceresc_e2e_tests PROPERTIES FOLDER "tests")

add_test(NAME e2e COMMAND ceresc_e2e_tests)
