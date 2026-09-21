add_library(OrbitCelestialLighting STATIC
    src/CelestialLighting.cpp
)

add_library(
    Orbit::CelestialLighting
    ALIAS OrbitCelestialLighting
)

target_include_directories(
    OrbitCelestialLighting
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
    OrbitCelestialLighting
    PUBLIC
        Orbit::CelestialRadiometry
        Orbit::Core
        Orbit::Math
)

target_compile_features(
    OrbitCelestialLighting PUBLIC cxx_std_23)
orbit_enable_warnings(
    OrbitCelestialLighting)

if(BUILD_TESTING)
    add_executable(
        OrbitCelestialLightingTests
        tests/CelestialLightingTests.cpp
    )

    target_link_libraries(
        OrbitCelestialLightingTests
        PRIVATE
            Orbit::CelestialLighting
    )

    target_compile_features(
        OrbitCelestialLightingTests PRIVATE cxx_std_23)
    orbit_enable_warnings(
        OrbitCelestialLightingTests)

    add_test(
        NAME Orbit.CelestialLighting
        COMMAND OrbitCelestialLightingTests
    )
endif()
