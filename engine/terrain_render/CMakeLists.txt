add_library(OrbitTerrainRender STATIC
    src/PhysicalSurface.cpp
    src/SurfaceEffectShader.cpp
    src/SurfaceEffects.cpp
    src/SurfaceMaterial.cpp
    src/TerrainPreviewRenderer.cpp
    src/UniformPlanetRenderer.cpp
)

add_library(Orbit::TerrainRender ALIAS OrbitTerrainRender)

target_include_directories(OrbitTerrainRender
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitTerrainRender
    PUBLIC
        Orbit::Math
        Orbit::RHI
        Orbit::Shader
        Orbit::SurfaceModel
        Orbit::Terrain
        Orbit::TerrainGpu
        Orbit::TerrainMaterialColumn
        Orbit::TerrainRegion
        Orbit::TerrainStream
        Orbit::TerrainView
        Orbit::World
)

target_compile_features(OrbitTerrainRender PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitTerrainRender)

if(BUILD_TESTING)
    add_executable(OrbitTerrainSurfaceShaderTests
        tests/TerrainSurfaceShaderTests.cpp
    )

    target_link_libraries(OrbitTerrainSurfaceShaderTests
        PRIVATE
            Orbit::ShaderDxc
            Orbit::TerrainRender
    )

    target_compile_features(OrbitTerrainSurfaceShaderTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTerrainSurfaceShaderTests)
    orbit_copy_dxc_runtime(OrbitTerrainSurfaceShaderTests)

    add_test(
        NAME Orbit.TerrainSurfaceShader
        COMMAND OrbitTerrainSurfaceShaderTests
    )

    add_executable(OrbitTerrainSurfaceEffectsTests
        tests/SurfaceEffectsTests.cpp
    )

    target_link_libraries(OrbitTerrainSurfaceEffectsTests
        PRIVATE
            Orbit::TerrainRender
    )

    target_compile_features(OrbitTerrainSurfaceEffectsTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTerrainSurfaceEffectsTests)

    add_test(
        NAME Orbit.TerrainSurfaceEffects
        COMMAND OrbitTerrainSurfaceEffectsTests
    )
endif()
