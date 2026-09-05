cmake_minimum_required(VERSION 3.15) # target_link_options() requires 3.13+, MSVC_RUNTIME_LIBRARY requires 3.15+
# CMP0091 makes MSVC_RUNTIME_LIBRARY (below) control /MT vs /MD; it must be
# set before project() to take effect.
cmake_policy(SET CMP0091 NEW)

# Must sit BEFORE project(): on a single-config generator CMake's own MSVC
# module writes Debug into CMAKE_BUILD_TYPE during project(), so the same
# check placed after it never fires. That default doesn't merely build the
# wrong thing, it doesn't build at all -- Debug adds /RTC1, which cl rejects
# against the /O2 this project sets below. An explicit
# -DCMAKE_BUILD_TYPE=Debug hits the same conflict: this build system exists
# to produce the shipping artifact, and has exactly one configuration.
#
# A CACHE entry, not a plain variable: project() writes its own default into
# the cache, and a plain variable would shadow it correctly while leaving
# CMakeCache.txt still reporting Debug.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build configuration" FORCE)
endif()

# Keep this VERSION in sync with src/aimg_version.h (AIMG_VERSION_MAJOR/MINOR/
# PATCH) and pluginst.inf's `version=` key -- not consumed by the build itself,
# just kept aligned by hand for consistency.
project(aimg VERSION 0.2.0 LANGUAGES CXX C RC)

# Multi-config generators ignore CMAKE_BUILD_TYPE and hardcode Debug when
# `cmake --build .` is given no --config. Offering only Release turns that
# into an explicit "no Debug|x64 configuration" rather than the cryptic
# /RTC1-vs-/O2 flag error above. A bare `cmake --build .` still needs
# --config Release on a Visual Studio generator.
if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES Release CACHE STRING "Supported build configurations" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(aimg SHARED
    src/metadata_parser.cpp
    src/aimg_decoder.cpp
    src/comfyui_decoder.cpp
    src/aimg.cpp
    src/aimg.def
    src/aimg.rc
)

target_include_directories(aimg PRIVATE src)

# The DLL goes directly in the build directory, not a per-config subfolder:
# a multi-config generator would otherwise write build\Release\aimg.wdx64,
# where a stale copy left in build\ by an earlier raw-cl run would be picked
# up instead. The per-config variable is needed as well as the plain one --
# multi-config generators append the config name unless the _<CONFIG> form
# overrides it. Set on the TARGET, not via the CMAKE_RUNTIME_OUTPUT_DIRECTORY
# variables: those only seed the property at add_library() time and would
# silently do nothing from here, below the target.
set_target_properties(aimg PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}")

# Define output extension depending on target architecture
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set_target_properties(aimg PROPERTIES SUFFIX ".wdx64" PREFIX "")
else()
    set_target_properties(aimg PROPERTIES SUFFIX ".wdx" PREFIX "")
endif()

if(MSVC)
    # /GL+/LTCG (whole-program optimization), /Gy (function-level linking) +
    # /OPT:REF,ICF (drop/fold unused code), /GR- (no RTTI: unused in this
    # codebase, no dynamic_cast/typeid) -- safe, behavior-preserving size/perf
    # flags with no new runtime dependency.
    target_compile_options(aimg PRIVATE /W4 /WX /O2 /GL /Gy /GR- /EHsc /DUNICODE /D_UNICODE /DNDEBUG)
    target_link_options(aimg PRIVATE /LTCG /OPT:REF /OPT:ICF)
    # Matches build_msvc.bat's /MT: the plugin ships as a bare DLL next to
    # totalcmd.exe with no runtime of its own, so a /MD build would fail to
    # load on a machine without the VC++ Redistributable installed.
    set_property(TARGET aimg PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
else()
    # Static runtime linkage, matching build.bat's raw g++ invocation: this
    # plugin is dropped as a bare DLL next to Total Commander, not installed
    # alongside its own runtime, so it must not depend on libgcc/libstdc++/
    # libwinpthread being present on the target machine.
    # -flto/-fno-rtti/-ffunction-sections+-fdata-sections+--gc-sections are the
    # GCC/Clang equivalents of the MSVC flags above.
    target_compile_options(aimg PRIVATE -Wall -O3 -flto -fno-rtti -ffunction-sections -fdata-sections -DUNICODE -D_UNICODE -DNDEBUG)
    target_link_options(aimg PRIVATE -static -flto -s -Wl,--gc-sections)
endif()
