cmake_minimum_required(VERSION 3.28)

project(Orbit
    VERSION 0.0.1
    DESCRIPTION "Orbit - custom engine for Asterra"
    LANGUAGES CXX
)

if(NOT WIN32)
    message(FATAL_ERROR "Orbit M0 currently targets Windows. Platform backends will be added explicitly.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ORBIT_WARNINGS_AS_ERRORS "Treat Orbit warnings as errors" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(OrbitWarnings)
include(CTest)

add_subdirectory(engine)
add_subdirectory(apps/sandbox)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
