add_library(OrbitCelestialCompactObjects STATIC
    src/CompactObject.cpp
)
add_library(
    Orbit::CelestialCompactObjects
    ALIAS OrbitCelestialCompactObjects)

target_include_directories(
    OrbitCelestialCompactObjects
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(
    OrbitCelestialCompactObjects
    PUBLIC
        Orbit::Core
        Orbit::Math
        Orbit::Terrain)

target_compile_features(
    OrbitCelestialCompactObjects PUBLIC cxx_std_23)
orbit_enable_warnings(
    OrbitCelestialCompactObjects)

if(BUILD_TESTING)
    add_executable(
        OrbitCelestialCompactObjectTests
        tests/CompactObjectTests.cpp)
    target_link_libraries(
        OrbitCelestialCompactObjectTests PRIVATE
        Orbit::CelestialCompactObjects)
    target_compile_features(
        OrbitCelestialCompactObjectTests PRIVATE cxx_std_23)
    orbit_enable_warnings(
        OrbitCelestialCompactObjectTests)
    add_test(
        NAME Orbit.CelestialCompactObjects
        COMMAND OrbitCelestialCompactObjectTests)
endif()
