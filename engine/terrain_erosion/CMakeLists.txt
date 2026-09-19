add_library(OrbitTerrainErosion STATIC
    src/AeolianErosion.cpp
    src/GlacialErosion.cpp
    src/HydraulicErosion.cpp
    src/HydrologyRefinement.cpp
    src/MultiScaleTerrain.cpp
    src/RegionalElevationDelta.cpp
    src/RiverCarving.cpp
    src/RiverCarvedTerrainSource.cpp
    src/RiverNetwork.cpp
    src/SedimentExchange.cpp
    src/SedimentTransport.cpp
    src/StreamPowerErosion.cpp
    src/ThermalErosion.cpp
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

if(BUILD_TESTING)
    add_executable(OrbitTerrainHydraulicErosionTests
        tests/HydraulicErosionTests.cpp
    )

    target_link_libraries(OrbitTerrainHydraulicErosionTests
        PRIVATE
            Orbit::TerrainErosion
    )

    target_compile_features(OrbitTerrainHydraulicErosionTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTerrainHydraulicErosionTests)

    add_test(
        NAME Orbit.TerrainHydraulicErosion
        COMMAND OrbitTerrainHydraulicErosionTests
    )
endif()

if(BUILD_TESTING)
    add_executable(OrbitTerrainThermalErosionTests
        tests/ThermalErosionTests.cpp
    )

    target_link_libraries(OrbitTerrainThermalErosionTests
        PRIVATE
            Orbit::TerrainErosion
    )

    target_compile_features(OrbitTerrainThermalErosionTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTerrainThermalErosionTests)

    add_test(
        NAME Orbit.TerrainThermalErosion
        COMMAND OrbitTerrainThermalErosionTests
    )
endif()

if(BUILD_TESTING)
    add_executable(OrbitTerrainAeolianErosionTests
        tests/AeolianErosionTests.cpp
    )

    target_link_libraries(OrbitTerrainAeolianErosionTests
        PRIVATE
            Orbit::TerrainErosion
    )

    target_compile_features(OrbitTerrainAeolianErosionTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTerrainAeolianErosionTests)

    add_test(
        NAME Orbit.TerrainAeolianErosion
        COMMAND OrbitTerrainAeolianErosionTests
    )
endif()

if(BUILD_TESTING)
    add_executable(OrbitTerrainSedimentExchangeTests
        tests/SedimentExchangeTests.cpp
    )

    target_link_libraries(OrbitTerrainSedimentExchangeTests
        PRIVATE
            Orbit::TerrainErosion
    )

    target_compile_features(OrbitTerrainSedimentExchangeTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTerrainSedimentExchangeTests)

    add_test(
        NAME Orbit.TerrainSedimentExchange
        COMMAND OrbitTerrainSedimentExchangeTests
    )
endif()

if(BUILD_TESTING)
    add_executable(OrbitTerrainGlacialErosionTests
        tests/GlacialErosionTests.cpp
    )

    target_link_libraries(OrbitTerrainGlacialErosionTests
        PRIVATE
            Orbit::TerrainErosion
    )

    target_compile_features(OrbitTerrainGlacialErosionTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTerrainGlacialErosionTests)

    add_test(
        NAME Orbit.TerrainGlacialErosion
        COMMAND OrbitTerrainGlacialErosionTests
    )
endif()

if(BUILD_TESTING)
    add_executable(OrbitTerrainRiverNetworkTests
        tests/RiverNetworkTests.cpp
    )

    target_link_libraries(OrbitTerrainRiverNetworkTests
        PRIVATE
            Orbit::TerrainErosion
    )

    target_compile_features(OrbitTerrainRiverNetworkTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTerrainRiverNetworkTests)

    add_test(
        NAME Orbit.TerrainRiverNetwork
        COMMAND OrbitTerrainRiverNetworkTests
    )
endif()


if(BUILD_TESTING)
    add_executable(OrbitTerrainMultiScaleTests
        tests/MultiScaleTerrainTests.cpp
    )

    target_link_libraries(OrbitTerrainMultiScaleTests
        PRIVATE
            Orbit::TerrainErosion
    )

    target_compile_features(OrbitTerrainMultiScaleTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTerrainMultiScaleTests)

    add_test(
        NAME Orbit.TerrainMultiScale
        COMMAND OrbitTerrainMultiScaleTests
    )
endif()
