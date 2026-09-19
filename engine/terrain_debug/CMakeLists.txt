add_library(OrbitTerrainDebug STATIC
    src/TerrainDebugField.cpp
)

add_library(Orbit::TerrainDebug ALIAS OrbitTerrainDebug)

target_include_directories(OrbitTerrainDebug
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitTerrainDebug
    PUBLIC
        Orbit::Core
        Orbit::RHI
        Orbit::Terrain
        Orbit::World
)

target_compile_features(OrbitTerrainDebug PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitTerrainDebug)

if(BUILD_TESTING)
    add_executable(OrbitTerrainDebugTests
        tests/TerrainDebugFieldTests.cpp
    )

    target_link_libraries(OrbitTerrainDebugTests
        PRIVATE
            Orbit::TerrainDebug
    )

    target_compile_features(OrbitTerrainDebugTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTerrainDebugTests)

    add_test(
        NAME Orbit.TerrainDebug
        COMMAND OrbitTerrainDebugTests
    )
endif()
