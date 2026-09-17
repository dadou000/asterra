add_library(OrbitSurfaceAuthoring STATIC
    src/TerrainConstraints.cpp
    src/TerrainConstraintsToml.cpp
)

add_library(Orbit::SurfaceAuthoring ALIAS OrbitSurfaceAuthoring)

target_include_directories(OrbitSurfaceAuthoring
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitSurfaceAuthoring
    PUBLIC
        Orbit::Core
        Orbit::Math
        Orbit::World
        Orbit::Terrain
        Orbit::TerrainGeology
    PRIVATE
        tomlplusplus::tomlplusplus
)

target_compile_features(OrbitSurfaceAuthoring PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitSurfaceAuthoring)

if(BUILD_TESTING)
    add_executable(OrbitSurfaceAuthoringTests
        tests/TerrainConstraintTests.cpp
    )

    target_link_libraries(OrbitSurfaceAuthoringTests
        PRIVATE
            Orbit::SurfaceAuthoring
    )

    target_compile_features(OrbitSurfaceAuthoringTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitSurfaceAuthoringTests)

    add_test(
        NAME Orbit.TerrainAuthoredConstraints
        COMMAND OrbitSurfaceAuthoringTests
    )
endif()
