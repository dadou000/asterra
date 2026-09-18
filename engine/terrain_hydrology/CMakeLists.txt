add_library(OrbitTerrainHydrology STATIC
    src/DrainagePage.cpp
    src/HydrologyGrid.cpp
    src/RiverGraph.cpp
)

add_library(Orbit::TerrainHydrology ALIAS OrbitTerrainHydrology)

target_include_directories(OrbitTerrainHydrology
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitTerrainHydrology
    PUBLIC
        Orbit::Core
        Orbit::Math
        Orbit::Terrain
        Orbit::TerrainMaterialColumn
        Orbit::World
)

target_compile_features(OrbitTerrainHydrology PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitTerrainHydrology)

if(BUILD_TESTING)
    add_executable(OrbitTerrainDrainagePageTests
        tests/DrainagePageTests.cpp
    )

    target_link_libraries(OrbitTerrainDrainagePageTests
        PRIVATE
            Orbit::TerrainHydrology
    )

    target_compile_features(OrbitTerrainDrainagePageTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTerrainDrainagePageTests)

    add_test(
        NAME Orbit.TerrainDrainagePage
        COMMAND OrbitTerrainDrainagePageTests
    )
endif()
