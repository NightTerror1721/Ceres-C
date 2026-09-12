# The tests' shared scaffolding: framework.h and the main() that runs what's registered.
#
# Copied from Ceres/tests/framework (same TEST()/CHECK_EQ() pattern, §12 of the architecture
# plan) rather than reinvented - Ceres-C has no package manager either, and these ~90 lines cost
# less to maintain than wiring in Catch2 or doctest.
#
# main.cpp is exposed as an INTERFACE source, not inside a static library: it's sixteen lines
# recompiled per test executable, and in exchange there is no need to reason about whether the
# linker pulls the object defining main() out of a .lib or not.

add_library(ceresc_test_framework INTERFACE)
add_library(ceresc::test_framework ALIAS ceresc_test_framework)

target_include_directories(ceresc_test_framework INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ceresc_test_framework INTERFACE ceresc::settings)

# A glob with CONFIGURE_DEPENDS, not an if(EXISTS): the if is only evaluated at configure time, so
# a suite would need a manual reconfigure to show up otherwise. With the glob, the next build
# notices on its own.
file(GLOB runner CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp")

if(NOT runner)
	message(STATUS "ceresc: tests/framework/main.cpp is missing - no suite will be built")
	return()
endif()

add_library(ceresc_test_main INTERFACE)
add_library(ceresc::test_main ALIAS ceresc_test_main)

target_sources(ceresc_test_main INTERFACE "$<BUILD_INTERFACE:${runner}>")
target_link_libraries(ceresc_test_main INTERFACE ceresc::test_framework)
