add_library(OrbitTerrainErosion STATIC
    src/HydrologyRefinement.cpp
    src/RegionalElevationDelta.cpp
    src/RiverCarving.cpp
    src/RiverCarvedTerrainSource.cpp
    src/SedimentTransport.cpp
    src/StreamPowerErosion.cpp
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
        Orbit::TerrainGeology
        Orbit::TerrainHydrology
        Orbit::TerrainMacroGeology
        Orbit::TerrainMaterialColumn
        Orbit::SurfaceAuthoring
        Orbit::World
)

target_compile_features(OrbitTerrainErosion PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitTerrainErosion)

if(BUILD_TESTING)
    add_executable(OrbitTerrainStreamPowerTests
        tests/StreamPowerErosionTests.cpp
    )

    target_link_libraries(OrbitTerrainStreamPowerTests
        PRIVATE
            Orbit::TerrainErosion
    )

    target_compile_features(OrbitTerrainStreamPowerTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTerrainStreamPowerTests)

    add_test(
        NAME Orbit.TerrainStreamPower
        COMMAND OrbitTerrainStreamPowerTests
    )
endif()
