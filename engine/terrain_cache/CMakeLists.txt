add_library(OrbitTerrainCache STATIC
    src/TerrainPageBuilder.cpp
)

add_library(Orbit::TerrainCache ALIAS OrbitTerrainCache)

target_include_directories(OrbitTerrainCache
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitTerrainCache
    PUBLIC
        Orbit::Core
        Orbit::Terrain
        Orbit::World
)

target_compile_features(OrbitTerrainCache PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitTerrainCache)
