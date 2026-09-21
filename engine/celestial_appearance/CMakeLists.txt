add_library(OrbitCelestialAppearance STATIC
    src/PlanetaryAppearance.cpp
)

add_library(
    Orbit::CelestialAppearance
    ALIAS OrbitCelestialAppearance
)

target_include_directories(
    OrbitCelestialAppearance
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
    OrbitCelestialAppearance
    PUBLIC
        Orbit::Core
        Orbit::Math
        Orbit::RHI
        Orbit::Terrain
)

target_compile_features(
    OrbitCelestialAppearance PUBLIC cxx_std_23)
orbit_enable_warnings(
    OrbitCelestialAppearance)

if(BUILD_TESTING)
    add_executable(
        OrbitPlanetaryAppearanceTests
        tests/PlanetaryAppearanceTests.cpp
    )

    target_link_libraries(
        OrbitPlanetaryAppearanceTests
        PRIVATE
            Orbit::CelestialAppearance
    )

    target_compile_features(
        OrbitPlanetaryAppearanceTests
        PRIVATE cxx_std_23)
    orbit_enable_warnings(
        OrbitPlanetaryAppearanceTests)

    add_test(
        NAME Orbit.PlanetaryAppearance
        COMMAND OrbitPlanetaryAppearanceTests
    )
endif()
