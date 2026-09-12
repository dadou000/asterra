add_library(OrbitTerrainStream STATIC
    src/TerrainSampleStreamer.cpp
    src/ToroidalResidency.cpp
)

add_library(Orbit::TerrainStream ALIAS OrbitTerrainStream)

target_include_directories(OrbitTerrainStream
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitTerrainStream
    PUBLIC
        Orbit::Core
        Orbit::Jobs
        Orbit::Terrain
        Orbit::TerrainView
        Orbit::World
)

target_compile_features(OrbitTerrainStream PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitTerrainStream)
