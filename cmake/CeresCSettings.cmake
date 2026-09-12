# Two INTERFACE targets shared by every library and app in the tree - same split Ceres itself
# uses, and for the same reason:
#
#   ceresc_settings   linked PUBLIC.  Changes the language seen by whoever includes us:
#                      conformance mode, the CERESC_DEBUG macro, C++23, Release optimizations.
#
#   ceresc_warnings   linked PRIVATE. Warnings are the business of whoever compiles a file, not
#                      whoever includes it - if they propagated, a warning of ours would show up
#                      in someone else's code with no way to silence it from the right place.

include_guard(GLOBAL)
include(CheckLinkerFlag)

# ---------------------------------------------------------------------------- ceresc_settings

add_library(ceresc_settings INTERFACE)
add_library(ceresc::settings ALIAS ceresc_settings)

target_compile_features(ceresc_settings INTERFACE cxx_std_23)

# CERESC_DEBUG turns on assertions in headers, so it has to reach whoever includes us - hence the
# PUBLIC settings target, not the PRIVATE warnings one.
target_compile_definitions(ceresc_settings INTERFACE $<$<CONFIG:Debug>:CERESC_DEBUG>)

if(MSVC)
    target_compile_options(ceresc_settings INTERFACE
        /permissive-        # standards-conformant mode, not MSVC's historical relaxed parsing
        /utf-8              # source files may contain non-ASCII characters in comments and literals
        /Zc:preprocessor    # the conformant preprocessor
        /Zc:__cplusplus     # without this, __cplusplus always reports 199711L
        /EHsc
        /Oi)                # intrinsic substitution in every configuration, not just Release
else()
    target_compile_options(ceresc_settings INTERFACE -finput-charset=UTF-8)
endif()

# ---------------------------------------------------------------------------- ceresc_warnings

add_library(ceresc_warnings INTERFACE)
add_library(ceresc::warnings ALIAS ceresc_warnings)

if(MSVC)
    target_compile_options(ceresc_warnings INTERFACE /W4)
else()
    # An unused parameter is normal in an AstVisitor override that ignores most of its arguments
    # (§6 of the architecture plan) - warning about that only teaches people to ignore warnings.
    target_compile_options(ceresc_warnings INTERFACE -Wall -Wextra -Wno-unused-parameter)
endif()

if(CERESC_WARNINGS_AS_ERRORS)
    if(MSVC)
        target_compile_options(ceresc_warnings INTERFACE /WX)
    else()
        target_compile_options(ceresc_warnings INTERFACE -Werror)
    endif()
endif()

# Parallel compilation under the Visual Studio generator; Ninja already distributes work on its
# own, so this only matters for the (uncommon) case of configuring with -G "Visual Studio 17 2022".
if(MSVC AND CMAKE_GENERATOR MATCHES "Visual Studio")
    target_compile_options(ceresc_warnings INTERFACE /MP)
endif()

# ---------------------------------------------------------------------------- Release optimizations
#
# CMake's own defaults already do the bulk of the work (/O2 /Ob2 /DNDEBUG on MSVC, -O3 -DNDEBUG on
# GCC/Clang - see CMAKE_CXX_FLAGS_RELEASE). What's added here is what those defaults leave out:
#
#   /Gy + /OPT:REF,ICF        function-level linking plus the linker discarding unreferenced
#   -ffunction/data-sections   COMDATs/sections and folding identical ones. This matters
#   + --gc-sections             specifically here because the compiler is split into eight static
#                               libraries (support/lexer/ast/parser/sema/ir/codegen/driver) -
#                               without it, dead code from a library low in the dependency chain
#                               rides along into ceresc.exe even when only one function from it is
#                               ever called.
#   CERESC_ENABLE_IPO          whole-program optimization across those same eight libraries
#                               (/GL+/LTCG, or -flto) - opt-in (see the root CMakeLists.txt)
#                               because it makes the Release build itself slower.
if(MSVC)
    target_compile_options(ceresc_settings INTERFACE $<$<CONFIG:Release>:/Gy>)
    target_link_options(ceresc_settings INTERFACE $<$<CONFIG:Release>:/OPT:REF /OPT:ICF>)
else()
    target_compile_options(ceresc_settings INTERFACE $<$<CONFIG:Release>:-ffunction-sections -fdata-sections>)

    check_linker_flag(CXX "-Wl,--gc-sections" CERESC_HAS_GC_SECTIONS)
    if(CERESC_HAS_GC_SECTIONS)
        target_link_options(ceresc_settings INTERFACE $<$<CONFIG:Release>:-Wl,--gc-sections>)
    endif()
endif()
