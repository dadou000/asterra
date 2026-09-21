add_library(OrbitCelestialOcean STATIC
    src/OceanOptics.cpp
)
add_library(Orbit::CelestialOcean ALIAS OrbitCelestialOcean)

target_include_directories(OrbitCelestialOcean PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(OrbitCelestialOcean PUBLIC
    Orbit::CelestialAppearance
    Orbit::Core
    Orbit::Math
    Orbit::Terrain)

target_compile_features(OrbitCelestialOcean PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitCelestialOcean)

if(BUILD_TESTING)
    add_executable(
        OrbitCelestialOceanTests
        tests/OceanOpticsTests.cpp)
    target_link_libraries(
        OrbitCelestialOceanTests PRIVATE
        Orbit::CelestialOcean)
    target_compile_features(
        OrbitCelestialOceanTests PRIVATE cxx_std_23)
    orbit_enable_warnings(
        OrbitCelestialOceanTests)
    add_test(
        NAME Orbit.CelestialOcean
        COMMAND OrbitCelestialOceanTests)
endif()
