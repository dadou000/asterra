add_library(OrbitTerrainDebug STATIC
    src/TerrainDebugField.cpp
    src/TerrainDebugPageData.cpp
    src/TerrainDebugLivePages.cpp
    src/TerrainDebugRaster.cpp
    src/TerrainDebugSeam.cpp
    src/TerrainDebugTexture.cpp
)

add_library(Orbit::TerrainDebug ALIAS OrbitTerrainDebug)

target_include_directories(OrbitTerrainDebug
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitTerrainDebug
    PUBLIC
        Orbit::Core
        Orbit::RHI
        Orbit::Terrain
        Orbit::TerrainBiome
        Orbit::TerrainErosion
        Orbit::TerrainGeology
        Orbit::TerrainHydrology
        Orbit::TerrainMacroGeology
        Orbit::TerrainMaterialColumn
        Orbit::TerrainScatter
        Orbit::TerrainRegion
        Orbit::World
)

target_compile_features(OrbitTerrainDebug PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitTerrainDebug)

if(BUILD_TESTING)
    add_executable(OrbitTerrainDebugTests
        tests/TerrainDebugFieldTests.cpp
    )
    target_link_libraries(OrbitTerrainDebugTests PRIVATE Orbit::TerrainDebug)
    target_compile_features(OrbitTerrainDebugTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTerrainDebugTests)
    add_test(NAME Orbit.TerrainDebug COMMAND OrbitTerrainDebugTests)

    add_executable(OrbitTerrainDebugRasterTests
        tests/TerrainDebugRasterTests.cpp
    )
    target_link_libraries(OrbitTerrainDebugRasterTests PRIVATE Orbit::TerrainDebug)
    target_compile_features(OrbitTerrainDebugRasterTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTerrainDebugRasterTests)
    add_test(NAME Orbit.TerrainDebugRaster COMMAND OrbitTerrainDebugRasterTests)

    add_executable(OrbitTerrainDebugPageDataTests
        tests/TerrainDebugPageDataTests.cpp
    )
    target_link_libraries(OrbitTerrainDebugPageDataTests PRIVATE Orbit::TerrainDebug)
    target_compile_features(OrbitTerrainDebugPageDataTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTerrainDebugPageDataTests)
    add_test(NAME Orbit.TerrainDebugPageData COMMAND OrbitTerrainDebugPageDataTests)

    add_executable(OrbitTerrainDebugLivePageTests
        tests/TerrainDebugLivePagesTests.cpp
    )
    target_link_libraries(OrbitTerrainDebugLivePageTests PRIVATE Orbit::TerrainDebug)
    target_compile_features(OrbitTerrainDebugLivePageTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTerrainDebugLivePageTests)
    add_test(NAME Orbit.TerrainDebugLivePages COMMAND OrbitTerrainDebugLivePageTests)

    add_executable(OrbitTerrainDebugSeamTests
        tests/TerrainDebugSeamTests.cpp
    )
    target_link_libraries(OrbitTerrainDebugSeamTests PRIVATE Orbit::TerrainDebug)
    target_compile_features(OrbitTerrainDebugSeamTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTerrainDebugSeamTests)
    add_test(NAME Orbit.TerrainDebugSeam COMMAND OrbitTerrainDebugSeamTests)
endif()
