add_library(OrbitTerrainGpu STATIC
    src/GpuDepressionFill.cpp
    src/GpuElevationQuery.cpp
    src/GpuErosion.cpp
    src/GpuFieldGenerator.cpp
    src/GpuFlowAccumulation.cpp
    src/GpuHydrologyRegion.cpp
    src/GpuLakeBasins.cpp
    src/GpuMaterialColumnResources.cpp
    src/GpuRegionDelta.cpp
    src/GpuRiverGeometry.cpp
)

add_library(Orbit::TerrainGpu ALIAS OrbitTerrainGpu)

target_include_directories(OrbitTerrainGpu
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitTerrainGpu
    PUBLIC
        Orbit::Math
        Orbit::RHI
        Orbit::Shader
        Orbit::Terrain
        Orbit::TerrainStream
        Orbit::TerrainMaterialColumn
        Orbit::World
)

target_compile_features(OrbitTerrainGpu PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitTerrainGpu)
