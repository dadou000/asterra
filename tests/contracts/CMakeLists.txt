add_executable(OrbitTerrainContractTests
    ../TerrainContractTests.cpp
)

target_link_libraries(OrbitTerrainContractTests
    PRIVATE
        Orbit::Terrain
)

target_compile_features(OrbitTerrainContractTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitTerrainContractTests)

add_test(
    NAME Orbit.TerrainContracts
    COMMAND OrbitTerrainContractTests
)

add_executable(OrbitTerrainFaceSeamTests
    ../TerrainFaceSeamTests.cpp
)

target_link_libraries(OrbitTerrainFaceSeamTests
    PRIVATE
        Orbit::TerrainCache
)

target_compile_features(OrbitTerrainFaceSeamTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitTerrainFaceSeamTests)

add_test(
    NAME Orbit.TerrainFaceSeams
    COMMAND OrbitTerrainFaceSeamTests
)

add_executable(OrbitTerrainM01IntegrationTests
    TerrainM01IntegrationTests.cpp
)

target_link_libraries(OrbitTerrainM01IntegrationTests
    PRIVATE
        Orbit::TerrainCache
        Orbit::TerrainErosion
        Orbit::TerrainHydrology
)

target_compile_features(
    OrbitTerrainM01IntegrationTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitTerrainM01IntegrationTests)

add_test(
    NAME Orbit.TerrainM01Integration
    COMMAND OrbitTerrainM01IntegrationTests
)
