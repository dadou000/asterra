add_library(OrbitWaterRender STATIC
    src/OceanMesh.cpp
    src/OceanRenderer.cpp
    src/RiverWaterRenderer.cpp
)

add_library(Orbit::WaterRender ALIAS OrbitWaterRender)

target_include_directories(OrbitWaterRender
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitWaterRender
    PUBLIC
        Orbit::Core
        Orbit::Math
        Orbit::RHI
        Orbit::Shader
        Orbit::TerrainRegion
        Orbit::TerrainWater
        Orbit::World
)

target_compile_features(OrbitWaterRender PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitWaterRender)
