add_library(OrbitTerrain STATIC
    src/AnalyticTerrainSource.cpp
)

add_library(Orbit::Terrain ALIAS OrbitTerrain)

target_include_directories(OrbitTerrain
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitTerrain
    PUBLIC
        Orbit::Core
        Orbit::Math
        Orbit::World
)

target_compile_features(OrbitTerrain PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitTerrain)
