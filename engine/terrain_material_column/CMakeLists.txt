add_library(OrbitTerrainMaterialColumn STATIC
    src/Local3DPromotion.cpp
    src/MaterialColumnPage.cpp
    src/SurfaceResolver.cpp
)

add_library(Orbit::TerrainMaterialColumn ALIAS OrbitTerrainMaterialColumn)

target_include_directories(OrbitTerrainMaterialColumn
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitTerrainMaterialColumn
    PUBLIC
        Orbit::Core
        Orbit::Terrain
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

if(BUILD_TESTING)
    add_executable(OrbitTerrainSurfaceResolverTests
        tests/SurfaceResolverTests.cpp
    )

    target_link_libraries(OrbitTerrainSurfaceResolverTests
        PRIVATE
            Orbit::TerrainMaterialColumn
            Orbit::TerrainRender
            Orbit::TerrainScatter
    )

    target_compile_features(OrbitTerrainSurfaceResolverTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTerrainSurfaceResolverTests)

    add_test(
        NAME Orbit.TerrainSurfaceResolver
        COMMAND OrbitTerrainSurfaceResolverTests
    )
endif()


if(BUILD_TESTING)
    add_executable(OrbitTerrainLocal3DPromotionTests
        tests/Local3DPromotionTests.cpp
    )

    target_link_libraries(OrbitTerrainLocal3DPromotionTests
        PRIVATE
            Orbit::TerrainMaterialColumn
    )

    target_compile_features(OrbitTerrainLocal3DPromotionTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTerrainLocal3DPromotionTests)

    add_test(
        NAME Orbit.TerrainLocal3DPromotion
        COMMAND OrbitTerrainLocal3DPromotionTests
    )
endif()
