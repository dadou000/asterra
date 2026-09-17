add_library(OrbitTerrainGeology STATIC
    src/GeologicalMaterial.cpp
    src/GeologicalMaterialToml.cpp
    src/Stratigraphy.cpp
    src/StratigraphyToml.cpp
)

add_library(Orbit::TerrainGeology ALIAS OrbitTerrainGeology)

target_include_directories(OrbitTerrainGeology
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitTerrainGeology
    PUBLIC
        Orbit::Core
        Orbit::Terrain
    PRIVATE
        tomlplusplus::tomlplusplus
)

target_compile_features(OrbitTerrainGeology PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitTerrainGeology)

if(BUILD_TESTING)
    add_executable(OrbitTerrainGeologyTests
        tests/GeologicalMaterialTests.cpp
    )

    target_link_libraries(OrbitTerrainGeologyTests
        PRIVATE
            Orbit::TerrainGeology
    )

    target_compile_definitions(OrbitTerrainGeologyTests
        PRIVATE
            ORBIT_TERRAIN_GEOLOGY_REFERENCE_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets/reference"
    )

    target_compile_features(OrbitTerrainGeologyTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTerrainGeologyTests)

    add_test(
        NAME Orbit.TerrainGeologyMaterials
        COMMAND OrbitTerrainGeologyTests
    )

    add_executable(OrbitTerrainStratigraphyTests
        tests/StratigraphyTests.cpp
    )

    target_link_libraries(OrbitTerrainStratigraphyTests
        PRIVATE
            Orbit::TerrainGeology
    )

    target_compile_definitions(OrbitTerrainStratigraphyTests
        PRIVATE
            ORBIT_TERRAIN_GEOLOGY_REFERENCE_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets/reference"
    )

    target_compile_features(OrbitTerrainStratigraphyTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTerrainStratigraphyTests)

    add_test(
        NAME Orbit.TerrainStratigraphy
        COMMAND OrbitTerrainStratigraphyTests
    )
endif()
