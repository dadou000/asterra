add_library(OrbitJobs STATIC
    src/JobSystem.cpp
)

add_library(Orbit::Jobs ALIAS OrbitJobs)

target_include_directories(OrbitJobs
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitJobs
    PUBLIC
        Orbit::Core
)

target_compile_features(OrbitJobs PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitJobs)

if(BUILD_TESTING)
    add_executable(OrbitJobSystemTelemetryTests
        tests/JobSystemTelemetryTests.cpp
    )

    target_link_libraries(OrbitJobSystemTelemetryTests
        PRIVATE
            Orbit::Jobs
    )

    target_compile_features(
        OrbitJobSystemTelemetryTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitJobSystemTelemetryTests)

    add_test(
        NAME Orbit.JobSystemTelemetry
        COMMAND OrbitJobSystemTelemetryTests
    )
endif()
