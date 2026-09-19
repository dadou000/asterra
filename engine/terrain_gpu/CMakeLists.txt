add_library(OrbitTerrainGpu STATIC
    src/GpuAeolianErosionPage.cpp
    src/GpuBiomeScatterPage.cpp
    src/GpuCoastalProcessPage.cpp
    src/GpuDepressionFill.cpp
    src/GpuDrainagePage.cpp
    src/GpuElevationQuery.cpp
    src/GpuErosion.cpp
    src/GpuFieldGenerator.cpp
    src/GpuFlowAccumulation.cpp
    src/GpuHydraulicErosionPage.cpp
    src/GpuThermalErosionPage.cpp
    src/GpuHydrologyRegion.cpp
    src/GpuLakeBasins.cpp
    src/GpuMaterialColumnResources.cpp
    src/GpuRegionDelta.cpp
    src/GpuRiverGeometry.cpp
    src/GpuSedimentExchangeResources.cpp
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
    add_executable(OrbitTerrainGpuDrainageShaderTests
        tests/M09DrainageShaderTests.cpp
    )

    target_link_libraries(OrbitTerrainGpuDrainageShaderTests
        PRIVATE
            Orbit::ShaderDxc
    )

    target_compile_features(OrbitTerrainGpuDrainageShaderTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTerrainGpuDrainageShaderTests)
    orbit_copy_dxc_runtime(OrbitTerrainGpuDrainageShaderTests)

    add_test(
        NAME Orbit.TerrainGpuDrainageShaders
        COMMAND OrbitTerrainGpuDrainageShaderTests
    )
endif()

if(BUILD_TESTING)
    add_executable(OrbitTerrainGpuHydraulicShaderTests
        tests/M11HydraulicShaderTests.cpp
    )

    target_link_libraries(OrbitTerrainGpuHydraulicShaderTests
        PRIVATE
            Orbit::ShaderDxc
    )

    target_compile_features(OrbitTerrainGpuHydraulicShaderTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTerrainGpuHydraulicShaderTests)
    orbit_copy_dxc_runtime(OrbitTerrainGpuHydraulicShaderTests)

    add_test(
        NAME Orbit.TerrainGpuHydraulicShaders
        COMMAND OrbitTerrainGpuHydraulicShaderTests
    )
endif()

if(BUILD_TESTING)
    add_executable(OrbitTerrainGpuThermalShaderTests
        tests/M12ThermalShaderTests.cpp
    )

    target_link_libraries(OrbitTerrainGpuThermalShaderTests
        PRIVATE
            Orbit::ShaderDxc
    )

    target_compile_features(OrbitTerrainGpuThermalShaderTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTerrainGpuThermalShaderTests)
    orbit_copy_dxc_runtime(OrbitTerrainGpuThermalShaderTests)

    add_test(
        NAME Orbit.TerrainGpuThermalShaders
        COMMAND OrbitTerrainGpuThermalShaderTests
    )
endif()

if(BUILD_TESTING)
    add_executable(OrbitTerrainGpuAeolianShaderTests
        tests/M13AeolianShaderTests.cpp
    )

    target_link_libraries(OrbitTerrainGpuAeolianShaderTests
        PRIVATE
            Orbit::ShaderDxc
    )

    target_compile_features(OrbitTerrainGpuAeolianShaderTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTerrainGpuAeolianShaderTests)
    orbit_copy_dxc_runtime(OrbitTerrainGpuAeolianShaderTests)

    add_test(
        NAME Orbit.TerrainGpuAeolianShaders
        COMMAND OrbitTerrainGpuAeolianShaderTests
    )
endif()

if(BUILD_TESTING)
    add_executable(OrbitTerrainGpuCoastalShaderTests
        tests/M17CoastalShaderTests.cpp
    )

    target_link_libraries(OrbitTerrainGpuCoastalShaderTests
        PRIVATE
            Orbit::ShaderDxc
    )

    target_compile_features(OrbitTerrainGpuCoastalShaderTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTerrainGpuCoastalShaderTests)
    orbit_copy_dxc_runtime(OrbitTerrainGpuCoastalShaderTests)

    add_test(
        NAME Orbit.TerrainGpuCoastalShaders
        COMMAND OrbitTerrainGpuCoastalShaderTests
    )
endif()

if(BUILD_TESTING)
    add_executable(OrbitTerrainGpuScatterShaderTests
        tests/M22ScatterShaderTests.cpp
    )

    target_link_libraries(OrbitTerrainGpuScatterShaderTests
        PRIVATE
            Orbit::ShaderDxc
    )

    target_compile_features(OrbitTerrainGpuScatterShaderTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitTerrainGpuScatterShaderTests)
    orbit_copy_dxc_runtime(OrbitTerrainGpuScatterShaderTests)

    add_test(
        NAME Orbit.TerrainGpuScatterShaders
        COMMAND OrbitTerrainGpuScatterShaderTests
    )
endif()
