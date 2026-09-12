add_library(OrbitTerrainRegion STATIC
    src/DerivedTerrainRegion.cpp
)

add_library(Orbit::TerrainRegion ALIAS OrbitTerrainRegion)

target_include_directories(OrbitTerrainRegion
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitTerrainRegion
    PUBLIC
        Orbit::Core
        Orbit::Terrain
        Orbit::TerrainHydrology
        Orbit::TerrainErosion
        Orbit::World
)

target_compile_features(OrbitTerrainRegion PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitTerrainRegion)
