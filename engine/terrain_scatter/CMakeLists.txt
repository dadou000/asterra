add_library(OrbitTerrainScatter STATIC
    src/DeterministicScatter.cpp
    src/PhysicalSurface.cpp
)

add_library(Orbit::TerrainScatter ALIAS OrbitTerrainScatter)

target_include_directories(OrbitTerrainScatter
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitTerrainScatter
    PUBLIC
        Orbit::Core
        Orbit::TerrainBiome
        Orbit::TerrainGeology
        Orbit::TerrainMaterialColumn
        Orbit::World
)

target_compile_features(OrbitTerrainScatter PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitTerrainScatter)

if(BUILD_TESTING)
    add_executable(OrbitTerrainScatterTests
        tests/DeterministicScatterTests.cpp
    )

    target_link_libraries(OrbitTerrainScatterTests
        PRIVATE
            Orbit::TerrainScatter
    )

    target_compile_features(OrbitTerrainScatterTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTerrainScatterTests)

    add_test(
        NAME Orbit.TerrainScatter
        COMMAND OrbitTerrainScatterTests
    )
endif()
