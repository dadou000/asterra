add_library(OrbitTerrainBiome STATIC
    src/BiomeService.cpp
)

add_library(Orbit::TerrainBiome ALIAS OrbitTerrainBiome)

target_include_directories(OrbitTerrainBiome
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitTerrainBiome
    PUBLIC
        Orbit::Core
        Orbit::TerrainGeology
        Orbit::Universe
)

target_compile_features(OrbitTerrainBiome PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitTerrainBiome)

if(BUILD_TESTING)
    add_executable(OrbitTerrainBiomeTests
        tests/BiomeServiceTests.cpp
    )

    target_link_libraries(OrbitTerrainBiomeTests
        PRIVATE
            Orbit::TerrainBiome
            Orbit::TerrainGeology
            Orbit::TerrainMaterialColumn
    )

    target_compile_features(OrbitTerrainBiomeTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTerrainBiomeTests)

    add_test(
        NAME Orbit.TerrainBiome
        COMMAND OrbitTerrainBiomeTests
    )
endif()
