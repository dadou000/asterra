add_library(OrbitTerrainView STATIC
    src/ClipmapLayout.cpp
    src/ClipmapPlanner.cpp
    src/ClipmapTracker.cpp
)

add_library(Orbit::TerrainView ALIAS OrbitTerrainView)

target_include_directories(OrbitTerrainView
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitTerrainView
    PUBLIC
        Orbit::Core
        Orbit::Math
        Orbit::World
)

target_compile_features(OrbitTerrainView PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitTerrainView)

if(BUILD_TESTING)
    add_executable(OrbitClipmapPlannerTests
        tests/ClipmapPlannerTests.cpp
    )

    target_link_libraries(OrbitClipmapPlannerTests
        PRIVATE
            Orbit::TerrainView
    )

    target_compile_features(OrbitClipmapPlannerTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitClipmapPlannerTests)

    add_test(
        NAME Orbit.ClipmapPlanner
        COMMAND OrbitClipmapPlannerTests
    )
endif()
