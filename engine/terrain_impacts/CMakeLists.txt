add_library(OrbitTerrainImpacts STATIC
    src/ImpactField.cpp
    src/ImpactFieldToml.cpp
)

add_library(Orbit::TerrainImpacts ALIAS OrbitTerrainImpacts)

target_include_directories(OrbitTerrainImpacts
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitTerrainImpacts
    PUBLIC
        Orbit::Core
        Orbit::Math
        Orbit::World
        Orbit::Terrain
    PRIVATE
        tomlplusplus::tomlplusplus
)

target_compile_features(OrbitTerrainImpacts PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitTerrainImpacts)

if(BUILD_TESTING)
    add_executable(OrbitTerrainImpactsTests
        tests/ImpactFieldTests.cpp
    )

    target_link_libraries(OrbitTerrainImpactsTests
        PRIVATE
            Orbit::TerrainImpacts
    )

    target_compile_features(OrbitTerrainImpactsTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTerrainImpactsTests)

    add_test(
        NAME Orbit.TerrainImpacts
        COMMAND OrbitTerrainImpactsTests
    )
endif()
