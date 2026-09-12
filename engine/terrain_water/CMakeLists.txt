add_library(OrbitTerrainWater STATIC
    src/LakeWater.cpp
    src/RiverWater.cpp
)

add_library(Orbit::TerrainWater ALIAS OrbitTerrainWater)

target_include_directories(OrbitTerrainWater
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitTerrainWater
    PUBLIC
        Orbit::Core
        Orbit::Math
        Orbit::TerrainErosion
        Orbit::TerrainHydrology
        Orbit::World
)

target_compile_features(OrbitTerrainWater PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitTerrainWater)
