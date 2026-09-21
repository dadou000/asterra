add_library(OrbitCelestialRotation STATIC
    src/OrientationState.cpp
)

add_library(Orbit::CelestialRotation ALIAS OrbitCelestialRotation)

target_include_directories(OrbitCelestialRotation
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitCelestialRotation
    PUBLIC
        Orbit::CelestialOrbits
        Orbit::Core
        Orbit::Math
        Orbit::Time
)

target_compile_features(OrbitCelestialRotation PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitCelestialRotation)

if(BUILD_TESTING)
    add_executable(OrbitCelestialRotationTests
        tests/OrientationStateTests.cpp
    )

    target_link_libraries(OrbitCelestialRotationTests
        PRIVATE
            Orbit::CelestialRotation
    )

    target_compile_features(
        OrbitCelestialRotationTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitCelestialRotationTests)

    add_test(
        NAME Orbit.CelestialRotation
        COMMAND OrbitCelestialRotationTests
    )
endif()
