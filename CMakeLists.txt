# Ceres-C - the build root.
#
# A minimalist C subset compiler that emits CASM text for CeresASM (github.com/Krampus1721/CeresASM).
# Independent project, sibling to Ceres: it never links against Ceres's own libraries - only
# invokes the `ceres` executable (asm/run) as an external process from libs/driver, exactly like a
# user would from a terminal. See the architecture plan (§0-§1) for the full rationale.
#
# Eight libraries, one per pipeline stage plus the shared base, each buildable and testable on its
# own (§1-§2 of the plan):
#
#     support -> lexer  -\
#             \-> ast ----> parser -> sema -> ir -> codegen -> driver -> apps/ceresc
#
# Every library starts empty and still configures and builds today, the same way Ceres's own CMake
# migration did it: a placeholder translation unit compiles until real sources land in a library's
# src/ (see cmake/CeresCLibrary.cmake). The phased plan (§13) fills them in one at a time.
#
# Open in Visual Studio via File > Open > Folder (VS reads CMakePresets.json on its own), or from
# the command line:
#     cmake --preset msvc && cmake --build --preset msvc-debug

cmake_minimum_required(VERSION 3.28)

project(CeresC
    VERSION      0.1.0
    DESCRIPTION  "A minimalist C subset compiler targeting CeresASM"
    HOMEPAGE_URL "https://github.com/Krampus1721/Ceres-C"
    LANGUAGES    CXX)

# C++23 across the whole tree, with no compiler extensions: what compiles here compiles
# identically with MSVC, GCC and Clang.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

# Only makes sense when Ceres-C is the top-level project; if it's ever included from another tree,
# whoever's in charge there decides where the binaries go and whether tests get built.
if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
    set_property(GLOBAL PROPERTY USE_FOLDERS ON)
    set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "cmake")
endif()

option(CERESC_BUILD_TESTS        "Build the test suites"                                  ${PROJECT_IS_TOP_LEVEL})
option(CERESC_WARNINGS_AS_ERRORS "Treat warnings as errors"                                OFF)
# Whole-program optimization for Release builds: /GL+/LTCG on MSVC, -flto on GCC/Clang, across
# all eight static libraries. Off by default because it makes the Release build itself noticeably
# slower - turn it on for the build you actually ship (the msvc-ipo/gcc-ipo/clang-ipo presets do
# this for you without touching the plain ones).
option(CERESC_ENABLE_IPO         "Enable interprocedural optimization for Release builds"  OFF)

if(CERESC_ENABLE_IPO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT CERESC_IPO_SUPPORTED OUTPUT CERESC_IPO_ERROR LANGUAGES CXX)
    if(NOT CERESC_IPO_SUPPORTED)
        message(FATAL_ERROR "CERESC_ENABLE_IPO was requested, but the compiler does not support it: ${CERESC_IPO_ERROR}")
    endif()
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()

include(CeresCSettings)
include(CeresCLibrary)

if(CERESC_BUILD_TESTS)
    enable_testing()
    # Before the libraries: ceresc_add_tests() checks for the runner's target, and an
    # if(TARGET ...) check is evaluated on the spot, not at generate time.
    add_subdirectory(tests/framework)
endif()

# Declaration order follows the dependency graph of §1: nothing is declared before what it
# depends on.
add_subdirectory(libs/support)
add_subdirectory(libs/lexer)
add_subdirectory(libs/ast)
add_subdirectory(libs/parser)
add_subdirectory(libs/sema)
add_subdirectory(libs/ir)
add_subdirectory(libs/codegen)
add_subdirectory(libs/driver)
add_subdirectory(apps/ceresc)

if(CERESC_BUILD_TESTS)
    add_subdirectory(tests/e2e)
    add_subdirectory(tests/examples)
endif()

if(PROJECT_IS_TOP_LEVEL)
    ceresc_report_status()
endif()
