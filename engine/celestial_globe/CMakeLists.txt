add_library(OrbitCelestialGlobe STATIC
    src/MacroGlobe.cpp
)

add_library(
    Orbit::CelestialGlobe
    ALIAS OrbitCelestialGlobe
)

target_include_directories(
    OrbitCelestialGlobe
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
    OrbitCelestialGlobe
    PUBLIC
        Orbit::Core
        Orbit::Math
        Orbit::Rhi
        Orbit::Terrain
        Orbit::Universe
)

target_compile_features(
    OrbitCelestialGlobe PUBLIC cxx_std_23)
orbit_enable_warnings(
    OrbitCelestialGlobe)

if(BUILD_TESTING)
    add_executable(
        OrbitMacroGlobeTests
        tests/MacroGlobeTests.cpp
    )

    target_link_libraries(
        OrbitMacroGlobeTests
        PRIVATE
            Orbit::CelestialGlobe
    )

    target_compile_features(
        OrbitMacroGlobeTests PRIVATE cxx_std_23)
    orbit_enable_warnings(
        OrbitMacroGlobeTests)

    add_test(
        NAME Orbit.MacroGlobe
        COMMAND OrbitMacroGlobeTests
    )
endif()
