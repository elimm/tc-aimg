cmake_minimum_required(VERSION 3.13) # target_link_options() requires 3.13+
# Keep this VERSION in sync with src/aimg_version.h (AIMG_VERSION_MAJOR/MINOR/
# PATCH) and pluginst.inf's `version=` key -- not consumed by the build itself,
# just kept aligned by hand for consistency.
project(aimg VERSION 0.1.3 LANGUAGES CXX C RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(aimg SHARED
    src/metadata_parser.cpp
    src/aimg_decoder.cpp
    src/aimg.cpp
    src/aimg.def
    src/aimg.rc
)

target_include_directories(aimg PRIVATE src)

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
    target_compile_options(aimg PRIVATE /W4 /WX /O2 /GL /Gy /GR- /EHsc /DUNICODE /D_UNICODE)
    target_link_options(aimg PRIVATE /LTCG /OPT:REF /OPT:ICF)
else()
    # Static runtime linkage, matching build.bat's raw g++ invocation: this
    # plugin is dropped as a bare DLL next to Total Commander, not installed
    # alongside its own runtime, so it must not depend on libgcc/libstdc++/
    # libwinpthread being present on the target machine.
    # -flto/-fno-rtti/-ffunction-sections+-fdata-sections+--gc-sections are the
    # GCC/Clang equivalents of the MSVC flags above.
    target_compile_options(aimg PRIVATE -Wall -O3 -flto -fno-rtti -ffunction-sections -fdata-sections -DUNICODE -D_UNICODE)
    target_link_options(aimg PRIVATE -static -flto -s -Wl,--gc-sections)
endif()
