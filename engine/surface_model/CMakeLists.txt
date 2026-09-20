add_library(OrbitSurfaceModel STATIC
    src/SurfaceComposition.cpp
    src/SurfaceMaterialResolver.cpp
    src/TerrainBodyServices.cpp
)

add_library(Orbit::SurfaceModel ALIAS OrbitSurfaceModel)

target_include_directories(OrbitSurfaceModel
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitSurfaceModel
    PUBLIC
        Orbit::Core
        Orbit::Scene
        Orbit::Surface
        Orbit::Terrain
        Orbit::TerrainBiome
        Orbit::TerrainErosion
        Orbit::TerrainGeology
        Orbit::TerrainGpu
        Orbit::TerrainMaterialColumn
        Orbit::TerrainWater
        Orbit::Universe
        Orbit::WorldModel
)

target_compile_features(OrbitSurfaceModel PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitSurfaceModel)

if(BUILD_TESTING)
    add_executable(OrbitSurfaceCompositionTests
        tests/SurfaceCompositionTests.cpp
    )

    target_link_libraries(OrbitSurfaceCompositionTests
        PRIVATE
            Orbit::Commands
            Orbit::Documents
            Orbit::Schema
            Orbit::SurfaceModel
    )

    target_compile_features(OrbitSurfaceCompositionTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitSurfaceCompositionTests)

    add_test(
        NAME Orbit.SurfaceComposition
        COMMAND OrbitSurfaceCompositionTests
    )

    add_executable(OrbitSurfaceMaterialResolverTests
        tests/SurfaceMaterialResolverTests.cpp
    )

    target_link_libraries(OrbitSurfaceMaterialResolverTests
        PRIVATE
            Orbit::SurfaceModel
            Orbit::TerrainGeology
            Orbit::TerrainMaterialColumn
            Orbit::TerrainRender
    )

    target_compile_features(OrbitSurfaceMaterialResolverTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitSurfaceMaterialResolverTests)

    add_test(
        NAME Orbit.SurfaceMaterialResolver
        COMMAND OrbitSurfaceMaterialResolverTests
    )
endif()
