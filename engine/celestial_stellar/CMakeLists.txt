add_library(OrbitCelestialStellar STATIC
    src/StellarAppearance.cpp
)
add_library(Orbit::CelestialStellar ALIAS OrbitCelestialStellar)

target_include_directories(OrbitCelestialStellar PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(OrbitCelestialStellar PUBLIC
    Orbit::Core
    Orbit::Math
    Orbit::Terrain
)

target_compile_features(OrbitCelestialStellar PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitCelestialStellar)

if(BUILD_TESTING)
    add_executable(
        OrbitCelestialStellarTests
        tests/StellarAppearanceTests.cpp)
    target_link_libraries(
        OrbitCelestialStellarTests PRIVATE
        Orbit::CelestialStellar)
    target_compile_features(
        OrbitCelestialStellarTests PRIVATE cxx_std_23)
    orbit_enable_warnings(
        OrbitCelestialStellarTests)
    add_test(
        NAME Orbit.CelestialStellar
        COMMAND OrbitCelestialStellarTests)
endif()
