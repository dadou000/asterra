add_library(OrbitCelestialMagnetosphereRender STATIC
    ../celestial_magnetosphere/src/AuroraRenderer.cpp
)
add_library(
    Orbit::CelestialMagnetosphereRender
    ALIAS OrbitCelestialMagnetosphereRender)

target_include_directories(
    OrbitCelestialMagnetosphereRender
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(
    OrbitCelestialMagnetosphereRender
    PUBLIC
        Orbit::CelestialMagnetosphere
        Orbit::RenderView
        Orbit::RHI
        Orbit::Shader)

target_compile_features(
    OrbitCelestialMagnetosphereRender PUBLIC cxx_std_23)
orbit_enable_warnings(
    OrbitCelestialMagnetosphereRender)
