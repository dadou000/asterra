add_library(OrbitTerrainDependency STATIC
    src/TerrainDependencyGraph.cpp
)

add_library(Orbit::TerrainDependency ALIAS OrbitTerrainDependency)

target_include_directories(OrbitTerrainDependency
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitTerrainDependency
    PUBLIC
        Orbit::Core
        Orbit::ProceduralGraph
        Orbit::Terrain
        Orbit::TerrainGpu
        Orbit::World
)

target_compile_features(OrbitTerrainDependency PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitTerrainDependency)

if(BUILD_TESTING)
    add_executable(OrbitTerrainDependencyTests
        tests/TerrainDependencyGraphTests.cpp
    )

    target_link_libraries(OrbitTerrainDependencyTests
        PRIVATE
            Orbit::Jobs
            Orbit::TerrainDependency
    )

    target_compile_features(OrbitTerrainDependencyTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTerrainDependencyTests)

    add_test(
        NAME Orbit.TerrainDependency
        COMMAND OrbitTerrainDependencyTests
    )
endif()
