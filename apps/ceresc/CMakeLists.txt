# ceresc - the product: the `ceresc` executable.
#
# Orchestrates the pipeline through libs/driver and exposes the command line described in §11 of
# the architecture plan. Kept as its own project for the same reason Ceres's apps/cli is: so that
# no library can depend on it - anything the CLI needs from elsewhere has to move up into a
# library first, and that shows while writing it.

configure_file(
	"${CMAKE_CURRENT_SOURCE_DIR}/src/version.h.in"
	"${CMAKE_CURRENT_BINARY_DIR}/generated/ceresc/version.h"
	@ONLY)

file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_executable(ceresc ${sources})

target_include_directories(ceresc PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

target_link_libraries(ceresc
	PRIVATE
		ceresc::driver
		ceresc::warnings)

set_target_properties(ceresc PROPERTIES FOLDER "apps")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES ${sources})
