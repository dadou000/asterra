add_library(OrbitTerrainWater STATIC
    src/CoastalProcess.cpp
    src/LakeWater.cpp
    src/RiverWater.cpp
)

add_library(Orbit::TerrainWater ALIAS OrbitTerrainWater)

target_include_directories(OrbitTerrainWater
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitTerrainWater
    PUBLIC
        Orbit::Core
        Orbit::Math
        Orbit::TerrainErosion
        Orbit::TerrainGeology
        Orbit::TerrainHydrology
        Orbit::TerrainMaterialColumn
        Orbit::World
)

target_compile_features(OrbitTerrainWater PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitTerrainWater)

if(BUILD_TESTING)
    add_executable(OrbitTerrainCoastalProcessTests
        tests/CoastalProcessTests.cpp
    )

    target_link_libraries(OrbitTerrainCoastalProcessTests
        PRIVATE
            Orbit::TerrainWater
    )

    target_compile_features(OrbitTerrainCoastalProcessTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTerrainCoastalProcessTests)

    add_test(
        NAME Orbit.TerrainCoastalProcess
        COMMAND OrbitTerrainCoastalProcessTests
    )
endif()
