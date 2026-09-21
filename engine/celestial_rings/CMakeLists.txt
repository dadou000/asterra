add_library(OrbitCelestialRings STATIC
    src/RingSystem.cpp
    src/RingRenderer.cpp
)
add_library(Orbit::CelestialRings ALIAS OrbitCelestialRings)
target_include_directories(OrbitCelestialRings PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(OrbitCelestialRings PUBLIC
    Orbit::Core
    Orbit::Math
    Orbit::RenderView
    Orbit::Rhi
    Orbit::Shader
    Orbit::Terrain
)
target_compile_features(OrbitCelestialRings PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitCelestialRings)
if(BUILD_TESTING)
    add_executable(OrbitCelestialRingsTests tests/RingSystemTests.cpp)
    target_link_libraries(OrbitCelestialRingsTests PRIVATE Orbit::CelestialRings)
    target_compile_features(OrbitCelestialRingsTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitCelestialRingsTests)
    add_test(NAME Orbit.CelestialRings COMMAND OrbitCelestialRingsTests)
endif()
