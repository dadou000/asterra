cmake_minimum_required(VERSION 3.28)

project(Orbit
    VERSION 0.0.9
    DESCRIPTION "Orbit - custom engine for Asterra"
    LANGUAGES CXX
)

if(NOT WIN32)
    message(FATAL_ERROR "Orbit M0 currently targets Windows. Platform backends will be added explicitly.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ORBIT_ENABLE_VALIDATION_TOOLS "Include scene acceptance commands and isolated terrain validation in Studio" OFF)

option(ORBIT_WARNINGS_AS_ERRORS "Treat Orbit warnings as errors" OFF)
option(
    ORBIT_FAST_HOT_ITERATION
    "Favor low-latency incremental compile/link behavior for local Orbit Studio save-to-reflect iteration"
    OFF
)

# Local development builds can trade final-link compactness for much faster
# incremental native iteration. This is intentionally opt-in so CI and shipping
# builds keep their ordinary link policy.
if(MSVC AND ORBIT_FAST_HOT_ITERATION)
    add_compile_options(/MP)
    message(STATUS "Orbit fast hot iteration is enabled")
endif()

# The micro-profiler symbolises stall stack samples (docs/ORBIT_PROFILER.md), so
# optimised builds carry debug info. /Z7 embeds it in each object (no shared PDB
# server contention under /MP) and the linker emits one PDB per executable; it
# does not change generated code.
option(
    ORBIT_PROFILER_SYMBOLS
    "Emit debug symbols in optimised builds so profiler stall stacks have function names"
    ON
)
if(MSVC AND ORBIT_PROFILER_SYMBOLS)
    add_compile_options("$<$<CONFIG:Release,MinSizeRel>:/Z7>")
    add_link_options("$<$<CONFIG:Release,MinSizeRel>:/DEBUG>")
    if(NOT ORBIT_FAST_HOT_ITERATION)
        # /DEBUG turns COMDAT folding off by default; keep the release size.
        add_link_options("$<$<CONFIG:Release,MinSizeRel>:/OPT:REF;/OPT:ICF>")
    endif()
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(OrbitWarnings)
include(OrbitDependencies)
include(CTest)

add_subdirectory(engine)
add_subdirectory(apps/player)
add_subdirectory(apps/editor)
add_subdirectory(apps/build)

if(BUILD_TESTING)
    add_subdirectory(tests)
    add_subdirectory(tests/contracts)
endif()
