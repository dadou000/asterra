execute_process(
    COMMAND git rev-parse --short=8 HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    RESULT_VARIABLE ORBIT_GIT_RESULT
    OUTPUT_VARIABLE ORBIT_GIT_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

if(NOT ORBIT_GIT_RESULT EQUAL 0 OR ORBIT_GIT_SHA STREQUAL "")
    set(ORBIT_GIT_SHA "nogit")
endif()

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/orbit/core/BuildInfo.hpp.in
    ${CMAKE_BINARY_DIR}/generated/orbit/core/BuildInfo.hpp
    @ONLY
)

add_library(OrbitCore STATIC
    src/Log.cpp
    src/Profiler.cpp
    src/ThreadName.cpp
)

add_library(Orbit::Core ALIAS OrbitCore)

target_include_directories(OrbitCore
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_BINARY_DIR}/generated
)

# The profiler symbolises stall stack samples with dbghelp.
target_link_libraries(OrbitCore PUBLIC dbghelp)

target_compile_features(OrbitCore PUBLIC cxx_std_23)

target_compile_definitions(OrbitCore
    PUBLIC
        ORBIT_BUILD_CONFIGURATION="$<CONFIG>"
)
orbit_enable_warnings(OrbitCore)

if(BUILD_TESTING)
    add_executable(OrbitProfilerTests
        tests/ProfilerTests.cpp
    )

    target_link_libraries(OrbitProfilerTests
        PRIVATE
            Orbit::Core
    )

    target_compile_features(OrbitProfilerTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitProfilerTests)

    add_test(
        NAME Orbit.Profiler
        COMMAND OrbitProfilerTests
    )
endif()
