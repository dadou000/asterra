add_library(OrbitTerrainMacroGeology STATIC
    src/MacroGeologyField.cpp
)

add_library(Orbit::TerrainMacroGeology ALIAS OrbitTerrainMacroGeology)

target_include_directories(OrbitTerrainMacroGeology
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitTerrainMacroGeology
    PUBLIC
        Orbit::Core
        Orbit::Math
        Orbit::World
        Orbit::Terrain
        Orbit::SurfaceAuthoring
)

target_compile_features(OrbitTerrainMacroGeology PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitTerrainMacroGeology)

if(BUILD_TESTING)
    add_executable(OrbitTerrainMacroGeologyTests
        tests/MacroGeologyTests.cpp
    )

    target_link_libraries(OrbitTerrainMacroGeologyTests
        PRIVATE
            Orbit::TerrainMacroGeology
    )

    target_compile_features(OrbitTerrainMacroGeologyTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTerrainMacroGeologyTests)

    add_test(
        NAME Orbit.TerrainMacroGeology
        COMMAND OrbitTerrainMacroGeologyTests
    )
endif()
