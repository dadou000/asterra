add_library(OrbitTerrainRender STATIC
    src/PhysicalSurface.cpp
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
