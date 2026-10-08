add_library(OrbitTerrainBake STATIC
    src/PlanetBakeFile.cpp
    src/RiverBaker.cpp
    src/TectonicBaker.cpp
    src/TerrainBakeService.cpp
)

add_library(Orbit::TerrainBake ALIAS OrbitTerrainBake)

target_include_directories(OrbitTerrainBake
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitTerrainBake
    PUBLIC
        Orbit::Core
        Orbit::Math
        Orbit::Terrain
        Orbit::World
)

target_compile_features(OrbitTerrainBake PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitTerrainBake)

if(BUILD_TESTING)
    add_executable(OrbitTerrainBakeTests
        tests/TerrainBakeTests.cpp
    )

    target_link_libraries(OrbitTerrainBakeTests
        PRIVATE
            Orbit::TerrainBake
    )

    target_compile_features(OrbitTerrainBakeTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTerrainBakeTests)

    add_test(
        NAME Orbit.TerrainBake
        COMMAND OrbitTerrainBakeTests
    )
endif()
