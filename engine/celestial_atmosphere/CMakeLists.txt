add_library(OrbitCelestialAtmosphere STATIC
    src/Atmosphere.cpp
)

add_library(
    Orbit::CelestialAtmosphere
    ALIAS OrbitCelestialAtmosphere
)

target_include_directories(
    OrbitCelestialAtmosphere
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
    OrbitCelestialAtmosphere
    PUBLIC
        Orbit::Core
        Orbit::Math
        Orbit::RHI
)

target_compile_features(
    OrbitCelestialAtmosphere PUBLIC cxx_std_23)
orbit_enable_warnings(
    OrbitCelestialAtmosphere)

if(BUILD_TESTING)
    add_executable(
        OrbitCelestialAtmosphereTests
        tests/AtmosphereTests.cpp
    )

    target_link_libraries(
        OrbitCelestialAtmosphereTests
        PRIVATE
            Orbit::CelestialAtmosphere
    )

    target_compile_features(
        OrbitCelestialAtmosphereTests PRIVATE cxx_std_23)
    orbit_enable_warnings(
        OrbitCelestialAtmosphereTests)

    add_test(
        NAME Orbit.CelestialAtmosphere
        COMMAND OrbitCelestialAtmosphereTests
    )
endif()
