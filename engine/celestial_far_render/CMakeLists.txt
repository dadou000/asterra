add_library(OrbitCelestialFarRender STATIC
    src/FarBodyRenderer.cpp
)

add_library(
    Orbit::CelestialFarRender
    ALIAS OrbitCelestialFarRender
)

target_include_directories(
    OrbitCelestialFarRender
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
    OrbitCelestialFarRender
    PUBLIC
        Orbit::CelestialAppearance
        Orbit::CelestialLighting
        Orbit::CelestialRepresentation
        Orbit::Core
        Orbit::Math
        Orbit::RenderView
        Orbit::Rhi
        Orbit::Shader
        Orbit::Terrain
        Orbit::Universe
)

target_compile_features(
    OrbitCelestialFarRender PUBLIC cxx_std_23)
orbit_enable_warnings(
    OrbitCelestialFarRender)

if(BUILD_TESTING)
    add_executable(
        OrbitCelestialFarRenderTests
        tests/FarBodyRendererTests.cpp
    )

    target_link_libraries(
        OrbitCelestialFarRenderTests
        PRIVATE
            Orbit::CelestialFarRender
    )

    target_compile_features(
        OrbitCelestialFarRenderTests PRIVATE cxx_std_23)
    orbit_enable_warnings(
        OrbitCelestialFarRenderTests)

    add_test(
        NAME Orbit.CelestialFarRender
        COMMAND OrbitCelestialFarRenderTests
    )
endif()
