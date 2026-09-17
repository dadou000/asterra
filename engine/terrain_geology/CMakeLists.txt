add_library(OrbitTerrainGeology STATIC
    src/GeologicalMaterial.cpp
    src/GeologicalMaterialToml.cpp
)

add_library(Orbit::TerrainGeology ALIAS OrbitTerrainGeology)

target_include_directories(OrbitTerrainGeology
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitTerrainGeology
    PUBLIC
        Orbit::Core
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
endif()
