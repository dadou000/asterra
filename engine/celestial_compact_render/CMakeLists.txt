add_library(OrbitCelestialCompactRender STATIC
    src/CompactObjectRenderer.cpp
)
add_library(
    Orbit::CelestialCompactRender
    ALIAS OrbitCelestialCompactRender)

target_include_directories(
    OrbitCelestialCompactRender
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(
    OrbitCelestialCompactRender
    PUBLIC
        Orbit::CelestialCompactObjects
        Orbit::RenderView
        Orbit::RHI
        Orbit::Shader)

target_compile_features(
    OrbitCelestialCompactRender PUBLIC cxx_std_23)
orbit_enable_warnings(
    OrbitCelestialCompactRender)
