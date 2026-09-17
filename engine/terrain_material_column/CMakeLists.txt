add_library(OrbitTerrainMaterialColumn STATIC
    src/MaterialColumnPage.cpp
)

add_library(Orbit::TerrainMaterialColumn ALIAS OrbitTerrainMaterialColumn)

target_include_directories(OrbitTerrainMaterialColumn
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitTerrainMaterialColumn
    PUBLIC
        Orbit::Core
        Orbit::TerrainGeology
        Orbit::TerrainImpacts
)

target_compile_features(OrbitTerrainMaterialColumn PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitTerrainMaterialColumn)

if(BUILD_TESTING)
    add_executable(OrbitTerrainMaterialColumnTests
        tests/MaterialColumnPageTests.cpp
    )

    target_link_libraries(OrbitTerrainMaterialColumnTests
        PRIVATE
            Orbit::TerrainMaterialColumn
            Orbit::RHI
    )

    target_compile_features(OrbitTerrainMaterialColumnTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTerrainMaterialColumnTests)

    add_test(
        NAME Orbit.TerrainMaterialColumn
        COMMAND OrbitTerrainMaterialColumnTests
    )
endif()
