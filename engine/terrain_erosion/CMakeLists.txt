add_library(OrbitTerrainErosion STATIC
    src/RiverCarving.cpp
    src/RiverCarvedTerrainSource.cpp
    src/SedimentTransport.cpp
)

add_library(Orbit::TerrainErosion ALIAS OrbitTerrainErosion)

target_include_directories(OrbitTerrainErosion
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitTerrainErosion
    PUBLIC
        Orbit::Core
        Orbit::Math
        Orbit::Terrain
        Orbit::TerrainHydrology
        Orbit::World
)

target_compile_features(OrbitTerrainErosion PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitTerrainErosion)
