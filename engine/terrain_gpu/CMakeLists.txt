add_library(OrbitTerrainGpu STATIC
    src/GpuDepressionFill.cpp
    src/GpuErosion.cpp
    src/GpuFieldGenerator.cpp
    src/GpuFlowAccumulation.cpp
    src/GpuHydrologyRegion.cpp
    src/GpuPhysicalPageComposite.cpp
    src/PersistentGpuTerrainCache.cpp
    src/GpuRegionDelta.cpp
)

add_library(Orbit::TerrainGpu ALIAS OrbitTerrainGpu)

target_include_directories(OrbitTerrainGpu
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitTerrainGpu
    PUBLIC
        Orbit::Math
        Orbit::RHI
        Orbit::Shader
        Orbit::Terrain
        Orbit::TerrainErosion
        Orbit::TerrainGeology
        Orbit::TerrainStream
        Orbit::TerrainMaterialColumn
        Orbit::TerrainScatter
        Orbit::TerrainWater
        Orbit::TerrainHydrology
        Orbit::World
)

target_compile_features(OrbitTerrainGpu PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitTerrainGpu)

if(BUILD_TESTING)
endif()

if(BUILD_TESTING)
endif()

if(BUILD_TESTING)
endif()

if(BUILD_TESTING)
endif()

if(BUILD_TESTING)
endif()

if(BUILD_TESTING)
endif()

if(BUILD_TESTING)
    add_executable(OrbitTerrainGpuPersistentCacheTests
        tests/PersistentGpuTerrainCacheTests.cpp
    )

    target_link_libraries(OrbitTerrainGpuPersistentCacheTests
        PRIVATE
            Orbit::TerrainGpu
    )

    target_compile_features(OrbitTerrainGpuPersistentCacheTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTerrainGpuPersistentCacheTests)

    add_test(
        NAME Orbit.TerrainGpuPersistentCache
        COMMAND OrbitTerrainGpuPersistentCacheTests
    )
endif()

if(BUILD_TESTING)
    add_executable(OrbitTerrainGpuPhysicalPageCompositeShaderTests
        tests/M12PhysicalPageCompositeShaderTests.cpp
    )

    target_link_libraries(OrbitTerrainGpuPhysicalPageCompositeShaderTests
        PRIVATE
            Orbit::ShaderDxc
    )

    target_compile_features(
        OrbitTerrainGpuPhysicalPageCompositeShaderTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTerrainGpuPhysicalPageCompositeShaderTests)
    orbit_copy_dxc_runtime(OrbitTerrainGpuPhysicalPageCompositeShaderTests)

    add_test(
        NAME Orbit.TerrainGpuPhysicalPageCompositeShader
        COMMAND OrbitTerrainGpuPhysicalPageCompositeShaderTests
    )
endif()
