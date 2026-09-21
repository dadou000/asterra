add_library(OrbitCelestialSmallBodies STATIC
    src/SmallBodyAppearance.cpp
)
add_library(Orbit::CelestialSmallBodies ALIAS OrbitCelestialSmallBodies)

target_include_directories(OrbitCelestialSmallBodies PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(OrbitCelestialSmallBodies PUBLIC
    Orbit::CelestialAppearance
    Orbit::Core
    Orbit::Math
    Orbit::Terrain
)

target_compile_features(OrbitCelestialSmallBodies PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitCelestialSmallBodies)

if(BUILD_TESTING)
    add_executable(
        OrbitCelestialSmallBodiesTests
        tests/SmallBodyAppearanceTests.cpp)
    target_link_libraries(
        OrbitCelestialSmallBodiesTests PRIVATE
        Orbit::CelestialSmallBodies)
    target_compile_features(
        OrbitCelestialSmallBodiesTests PRIVATE cxx_std_23)
    orbit_enable_warnings(
        OrbitCelestialSmallBodiesTests)
    add_test(
        NAME Orbit.CelestialSmallBodies
        COMMAND OrbitCelestialSmallBodiesTests)
endif()
