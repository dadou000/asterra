add_library(OrbitTerrainRegion STATIC
    src/DerivedRegionTerrainSource.cpp
    src/DerivedTerrainRegion.cpp
    src/DerivedTerrainRegionCache.cpp
    src/DerivedTerrainRegionGpu.cpp
    src/DerivedTerrainRegionStreamer.cpp
    src/SurfaceBoundaryExchange.cpp
)

add_library(Orbit::TerrainRegion ALIAS OrbitTerrainRegion)

target_include_directories(OrbitTerrainRegion
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitTerrainRegion
    PUBLIC
        Orbit::Core
        Orbit::Jobs
        Orbit::Math
        Orbit::RHI
        Orbit::Terrain
        Orbit::TerrainErosion
        Orbit::TerrainGpu
        Orbit::TerrainHydrology
        Orbit::TerrainMaterialColumn
        Orbit::TerrainWater
        Orbit::World
)

target_compile_features(OrbitTerrainRegion PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitTerrainRegion)


if(BUILD_TESTING)
    add_executable(OrbitTerrainBoundaryExchangeTests
        tests/SurfaceBoundaryExchangeTests.cpp
    )

    target_link_libraries(OrbitTerrainBoundaryExchangeTests
        PRIVATE
            Orbit::TerrainRegion
    )

    target_compile_features(OrbitTerrainBoundaryExchangeTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTerrainBoundaryExchangeTests)

    add_test(
        NAME Orbit.TerrainBoundaryExchange
        COMMAND OrbitTerrainBoundaryExchangeTests
    )
endif()
