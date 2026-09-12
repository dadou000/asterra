add_library(OrbitTerrainView STATIC
    src/ClipmapLayout.cpp
)

add_library(Orbit::TerrainView ALIAS OrbitTerrainView)

target_include_directories(OrbitTerrainView
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitTerrainView
    PUBLIC
        Orbit::Core
        Orbit::Math
        Orbit::World
)

target_compile_features(OrbitTerrainView PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitTerrainView)
