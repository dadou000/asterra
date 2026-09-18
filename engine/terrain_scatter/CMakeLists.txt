add_library(OrbitTerrainScatter STATIC
    src/PhysicalSurface.cpp
)

add_library(Orbit::TerrainScatter ALIAS OrbitTerrainScatter)

target_include_directories(OrbitTerrainScatter
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitTerrainScatter
    PUBLIC
        Orbit::Core
        Orbit::TerrainGeology
        Orbit::TerrainMaterialColumn
)

target_compile_features(OrbitTerrainScatter PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitTerrainScatter)
