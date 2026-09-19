add_library(OrbitTerrainBoundary STATIC
    src/PhysicalPageBoundary.cpp
)

add_library(Orbit::TerrainBoundary ALIAS OrbitTerrainBoundary)

target_include_directories(OrbitTerrainBoundary
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitTerrainBoundary
    PUBLIC
        Orbit::Core
        Orbit::Terrain
        Orbit::TerrainErosion
        Orbit::TerrainHydrology
        Orbit::TerrainMaterialColumn
        Orbit::World
)

target_compile_features(OrbitTerrainBoundary PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitTerrainBoundary)

if(BUILD_TESTING)
    add_executable(OrbitTerrainBoundaryExchangeTests
        tests/PhysicalPageBoundaryTests.cpp
    )

    target_link_libraries(OrbitTerrainBoundaryExchangeTests
        PRIVATE
            Orbit::TerrainBoundary
    )

    target_compile_features(OrbitTerrainBoundaryExchangeTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTerrainBoundaryExchangeTests)

    add_test(
        NAME Orbit.TerrainBoundaryExchange
        COMMAND OrbitTerrainBoundaryExchangeTests
    )
endif()
