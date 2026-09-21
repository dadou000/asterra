add_library(OrbitCelestialMagnetosphere STATIC
    src/Magnetosphere.cpp
)
add_library(Orbit::CelestialMagnetosphere ALIAS OrbitCelestialMagnetosphere)

target_include_directories(OrbitCelestialMagnetosphere PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(OrbitCelestialMagnetosphere PUBLIC
    Orbit::Core
    Orbit::Math
    Orbit::Terrain
)

target_compile_features(OrbitCelestialMagnetosphere PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitCelestialMagnetosphere)

if(BUILD_TESTING)
    add_executable(
        OrbitCelestialMagnetosphereTests
        tests/MagnetosphereTests.cpp)
    target_link_libraries(
        OrbitCelestialMagnetosphereTests PRIVATE
        Orbit::CelestialMagnetosphere)
    target_compile_features(
        OrbitCelestialMagnetosphereTests PRIVATE cxx_std_23)
    orbit_enable_warnings(
        OrbitCelestialMagnetosphereTests)
    add_test(
        NAME Orbit.CelestialMagnetosphere
        COMMAND OrbitCelestialMagnetosphereTests)
endif()
