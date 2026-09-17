add_library(OrbitTerrainRelief STATIC
    src/BaseReliefField.cpp
)

add_library(Orbit::TerrainRelief ALIAS OrbitTerrainRelief)

target_include_directories(OrbitTerrainRelief
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitTerrainRelief
    PUBLIC
        Orbit::Core
        Orbit::Math
        Orbit::World
        Orbit::Terrain
        Orbit::TerrainMacroGeology
)

target_compile_features(OrbitTerrainRelief PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitTerrainRelief)

if(BUILD_TESTING)
    add_executable(OrbitTerrainReliefTests
        tests/BaseReliefTests.cpp
    )

    target_link_libraries(OrbitTerrainReliefTests
        PRIVATE
            Orbit::TerrainRelief
    )

    target_compile_features(OrbitTerrainReliefTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTerrainReliefTests)

    add_test(
        NAME Orbit.TerrainRelief
        COMMAND OrbitTerrainReliefTests
    )
endif()
