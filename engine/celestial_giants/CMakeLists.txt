add_library(OrbitCelestialGiants STATIC
    src/GiantAppearance.cpp
)
add_library(Orbit::CelestialGiants ALIAS OrbitCelestialGiants)

target_include_directories(OrbitCelestialGiants PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(OrbitCelestialGiants PUBLIC
    Orbit::CelestialAppearance
    Orbit::Core
    Orbit::Math
    Orbit::Terrain
)

target_compile_features(OrbitCelestialGiants PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitCelestialGiants)

if(BUILD_TESTING)
    add_executable(
        OrbitCelestialGiantsTests
        tests/GiantAppearanceTests.cpp)
    target_link_libraries(
        OrbitCelestialGiantsTests PRIVATE
        Orbit::CelestialGiants)
    target_compile_features(
        OrbitCelestialGiantsTests PRIVATE cxx_std_23)
    orbit_enable_warnings(
        OrbitCelestialGiantsTests)
    add_test(
        NAME Orbit.CelestialGiants
        COMMAND OrbitCelestialGiantsTests)
endif()
