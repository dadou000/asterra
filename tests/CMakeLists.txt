add_executable(OrbitWorldTests
    WorldCoordinatesTests.cpp
)

# Manual performance/height survey; timing is deliberately not a CTest gate.
add_executable(OrbitTerrainBenchmark EXCLUDE_FROM_ALL
    ../tools/TerrainBenchmark.cpp
)
target_link_libraries(OrbitTerrainBenchmark PRIVATE Orbit::TerrainCache)
target_compile_features(OrbitTerrainBenchmark PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitTerrainBenchmark)

add_executable(OrbitTerrainMountainTests TerrainMountainTests.cpp)
target_link_libraries(OrbitTerrainMountainTests PRIVATE Orbit::Terrain)
target_compile_features(OrbitTerrainMountainTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitTerrainMountainTests)
add_test(NAME Orbit.TerrainMountains COMMAND OrbitTerrainMountainTests)

target_link_libraries(OrbitWorldTests
    PRIVATE
        Orbit::World
)

target_compile_features(OrbitWorldTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitWorldTests)

add_test(
    NAME Orbit.WorldCoordinates
    COMMAND OrbitWorldTests
)

add_executable(OrbitJobsTests
    JobSystemTests.cpp
)

target_link_libraries(OrbitJobsTests
    PRIVATE
        Orbit::Jobs
)

target_compile_features(OrbitJobsTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitJobsTests)

add_test(
    NAME Orbit.JobSystem
    COMMAND OrbitJobsTests
)

add_executable(OrbitPlanetTerrainTests
    PlanetTerrainTests.cpp
)

target_link_libraries(OrbitPlanetTerrainTests
    PRIVATE
        Orbit::Terrain
)

target_compile_features(OrbitPlanetTerrainTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitPlanetTerrainTests)

add_test(
    NAME Orbit.PlanetTerrain
    COMMAND OrbitPlanetTerrainTests
)

add_executable(OrbitTerrainViewTests
    TerrainViewTests.cpp
)

target_link_libraries(OrbitTerrainViewTests
    PRIVATE
        Orbit::TerrainView
)

target_compile_features(OrbitTerrainViewTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitTerrainViewTests)

add_test(
    NAME Orbit.TerrainView
    COMMAND OrbitTerrainViewTests
)

add_executable(OrbitTerrainCacheTests
    TerrainCacheTests.cpp
)

target_link_libraries(OrbitTerrainCacheTests
    PRIVATE
        Orbit::TerrainCache
)

target_compile_features(OrbitTerrainCacheTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitTerrainCacheTests)

add_test(
    NAME Orbit.TerrainCache
    COMMAND OrbitTerrainCacheTests
)

add_executable(OrbitShaderCompilerTests
    ShaderCompilerTests.cpp
)

target_link_libraries(OrbitShaderCompilerTests
    PRIVATE
        Orbit::ShaderDxc
)

target_compile_features(OrbitShaderCompilerTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitShaderCompilerTests)

# dxcompiler.dll is loaded at runtime, not just link time -- without
# it next to the executable, Windows blocks on a modal "module not
# found" dialog instead of failing fast, hanging the test.
add_custom_command(TARGET OrbitShaderCompilerTests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ORBIT_DXCOMPILER_DLL}"
        "$<TARGET_FILE_DIR:OrbitShaderCompilerTests>"
)

add_test(
    NAME Orbit.ShaderCompiler
    COMMAND OrbitShaderCompilerTests
)


add_executable(OrbitRhiComputeTests
    RhiComputeTests.cpp
)

target_link_libraries(OrbitRhiComputeTests
    PRIVATE
        Orbit::RhiVulkan
        Orbit::ShaderDxc
)

target_compile_features(OrbitRhiComputeTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitRhiComputeTests)

# dxcompiler.dll is loaded at runtime, not just link time -- see the
# identical comment on OrbitShaderCompilerTests above.
add_custom_command(TARGET OrbitRhiComputeTests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ORBIT_DXCOMPILER_DLL}"
        "$<TARGET_FILE_DIR:OrbitRhiComputeTests>"
)

add_test(
    NAME Orbit.RhiCompute
    COMMAND OrbitRhiComputeTests
)


add_executable(OrbitTerrainGpuFieldTests
    TerrainGpuFieldTests.cpp
)

target_link_libraries(OrbitTerrainGpuFieldTests
    PRIVATE
        Orbit::RhiVulkan
        Orbit::ShaderDxc
        Orbit::Terrain
        Orbit::TerrainGpu
)

target_compile_features(OrbitTerrainGpuFieldTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitTerrainGpuFieldTests)

add_custom_command(TARGET OrbitTerrainGpuFieldTests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ORBIT_DXCOMPILER_DLL}"
        "$<TARGET_FILE_DIR:OrbitTerrainGpuFieldTests>"
)

add_test(
    NAME Orbit.TerrainGpuField
    COMMAND OrbitTerrainGpuFieldTests
)


add_executable(OrbitTerrainGpuRegionDeltaTests
    TerrainGpuRegionDeltaTests.cpp
)

target_link_libraries(OrbitTerrainGpuRegionDeltaTests
    PRIVATE
        Orbit::RhiVulkan
        Orbit::ShaderDxc
        Orbit::Terrain
        Orbit::TerrainGpu
        Orbit::TerrainStream
)

target_compile_features(OrbitTerrainGpuRegionDeltaTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitTerrainGpuRegionDeltaTests)

add_custom_command(TARGET OrbitTerrainGpuRegionDeltaTests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ORBIT_DXCOMPILER_DLL}"
        "$<TARGET_FILE_DIR:OrbitTerrainGpuRegionDeltaTests>"
)

add_test(
    NAME Orbit.TerrainGpuRegionDelta
    COMMAND OrbitTerrainGpuRegionDeltaTests
)


add_executable(OrbitTerrainGpuElevationQueryTests
    TerrainGpuElevationQueryTests.cpp
)

target_link_libraries(OrbitTerrainGpuElevationQueryTests
    PRIVATE
        Orbit::RhiVulkan
        Orbit::ShaderDxc
        Orbit::Terrain
        Orbit::TerrainGpu
)

target_compile_features(OrbitTerrainGpuElevationQueryTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitTerrainGpuElevationQueryTests)

add_custom_command(TARGET OrbitTerrainGpuElevationQueryTests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ORBIT_DXCOMPILER_DLL}"
        "$<TARGET_FILE_DIR:OrbitTerrainGpuElevationQueryTests>"
)

add_test(
    NAME Orbit.TerrainGpuElevationQuery
    COMMAND OrbitTerrainGpuElevationQueryTests
)


add_executable(OrbitHydrologyGpuDepressionFillTests
    HydrologyGpuDepressionFillTests.cpp
)

target_link_libraries(OrbitHydrologyGpuDepressionFillTests
    PRIVATE
        Orbit::RhiVulkan
        Orbit::ShaderDxc
        Orbit::TerrainGpu
)

target_compile_features(OrbitHydrologyGpuDepressionFillTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitHydrologyGpuDepressionFillTests)

add_custom_command(TARGET OrbitHydrologyGpuDepressionFillTests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ORBIT_DXCOMPILER_DLL}"
        "$<TARGET_FILE_DIR:OrbitHydrologyGpuDepressionFillTests>"
)

add_test(
    NAME Orbit.HydrologyGpuDepressionFill
    COMMAND OrbitHydrologyGpuDepressionFillTests
)


add_executable(OrbitHydrologyGpuFlowAccumulationTests
    HydrologyGpuFlowAccumulationTests.cpp
)

target_link_libraries(OrbitHydrologyGpuFlowAccumulationTests
    PRIVATE
        Orbit::RhiVulkan
        Orbit::ShaderDxc
        Orbit::TerrainGpu
)

target_compile_features(OrbitHydrologyGpuFlowAccumulationTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitHydrologyGpuFlowAccumulationTests)

add_custom_command(TARGET OrbitHydrologyGpuFlowAccumulationTests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ORBIT_DXCOMPILER_DLL}"
        "$<TARGET_FILE_DIR:OrbitHydrologyGpuFlowAccumulationTests>"
)

add_test(
    NAME Orbit.HydrologyGpuFlowAccumulation
    COMMAND OrbitHydrologyGpuFlowAccumulationTests
)


add_executable(OrbitHydrologyGpuErosionTests
    HydrologyGpuErosionTests.cpp
)

target_link_libraries(OrbitHydrologyGpuErosionTests
    PRIVATE
        Orbit::RhiVulkan
        Orbit::ShaderDxc
        Orbit::TerrainGpu
)

target_compile_features(OrbitHydrologyGpuErosionTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitHydrologyGpuErosionTests)

add_custom_command(TARGET OrbitHydrologyGpuErosionTests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ORBIT_DXCOMPILER_DLL}"
        "$<TARGET_FILE_DIR:OrbitHydrologyGpuErosionTests>"
)

add_test(
    NAME Orbit.HydrologyGpuErosion
    COMMAND OrbitHydrologyGpuErosionTests
)


add_executable(OrbitHydrologyGpuRiverGeometryTests
    HydrologyGpuRiverGeometryTests.cpp
)

target_link_libraries(OrbitHydrologyGpuRiverGeometryTests
    PRIVATE
        Orbit::RhiVulkan
        Orbit::ShaderDxc
        Orbit::TerrainGpu
)

target_compile_features(OrbitHydrologyGpuRiverGeometryTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitHydrologyGpuRiverGeometryTests)

add_custom_command(TARGET OrbitHydrologyGpuRiverGeometryTests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ORBIT_DXCOMPILER_DLL}"
        "$<TARGET_FILE_DIR:OrbitHydrologyGpuRiverGeometryTests>"
)

add_test(
    NAME Orbit.HydrologyGpuRiverGeometry
    COMMAND OrbitHydrologyGpuRiverGeometryTests
)


add_executable(OrbitHydrologyGpuLakeBasinsTests
    HydrologyGpuLakeBasinsTests.cpp
)

target_link_libraries(OrbitHydrologyGpuLakeBasinsTests
    PRIVATE
        Orbit::RhiVulkan
        Orbit::ShaderDxc
        Orbit::TerrainGpu
)

target_compile_features(OrbitHydrologyGpuLakeBasinsTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitHydrologyGpuLakeBasinsTests)

add_custom_command(TARGET OrbitHydrologyGpuLakeBasinsTests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ORBIT_DXCOMPILER_DLL}"
        "$<TARGET_FILE_DIR:OrbitHydrologyGpuLakeBasinsTests>"
)

add_test(
    NAME Orbit.HydrologyGpuLakeBasins
    COMMAND OrbitHydrologyGpuLakeBasinsTests
)


add_executable(OrbitTerrainStreamTests
    TerrainStreamTests.cpp
)

target_link_libraries(OrbitTerrainStreamTests
    PRIVATE
        Orbit::TerrainStream
)

target_compile_features(OrbitTerrainStreamTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitTerrainStreamTests)

add_test(
    NAME Orbit.TerrainStream
    COMMAND OrbitTerrainStreamTests
)


add_executable(OrbitTerrainSampleStreamerTests
    TerrainSampleStreamerTests.cpp
)

target_link_libraries(OrbitTerrainSampleStreamerTests
    PRIVATE
        Orbit::TerrainStream
)

target_compile_features(OrbitTerrainSampleStreamerTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitTerrainSampleStreamerTests)

add_test(
    NAME Orbit.TerrainSampleStreamer
    COMMAND OrbitTerrainSampleStreamerTests
)

add_executable(OrbitTerrainMorphRefreshTests
    TerrainMorphRefreshTests.cpp
)

target_link_libraries(OrbitTerrainMorphRefreshTests
    PRIVATE Orbit::TerrainStream
)

target_compile_features(OrbitTerrainMorphRefreshTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitTerrainMorphRefreshTests)

add_test(
    NAME Orbit.TerrainMorphRefresh
    COMMAND OrbitTerrainMorphRefreshTests
)


add_executable(OrbitCameraTests
    CameraTests.cpp
)

target_link_libraries(OrbitCameraTests
    PRIVATE
        Orbit::Camera
)

target_compile_features(OrbitCameraTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitCameraTests)

add_test(
    NAME Orbit.Camera
    COMMAND OrbitCameraTests
)


add_executable(OrbitTerrainFieldsTests
    TerrainFieldsTests.cpp
)

target_link_libraries(OrbitTerrainFieldsTests
    PRIVATE
        Orbit::Terrain
)

target_compile_features(OrbitTerrainFieldsTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitTerrainFieldsTests)

add_test(
    NAME Orbit.TerrainFields
    COMMAND OrbitTerrainFieldsTests
)


add_executable(OrbitTerrainHydrologyTests
    TerrainHydrologyTests.cpp
)

target_link_libraries(OrbitTerrainHydrologyTests
    PRIVATE
        Orbit::TerrainHydrology
)

target_compile_features(OrbitTerrainHydrologyTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitTerrainHydrologyTests)

add_test(
    NAME Orbit.TerrainHydrology
    COMMAND OrbitTerrainHydrologyTests
)


add_executable(OrbitTerrainErosionTests
    TerrainErosionTests.cpp
)

target_link_libraries(OrbitTerrainErosionTests
    PRIVATE
        Orbit::TerrainErosion
)

target_compile_features(OrbitTerrainErosionTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitTerrainErosionTests)

add_test(
    NAME Orbit.TerrainErosion
    COMMAND OrbitTerrainErosionTests
)


add_executable(OrbitTerrainSedimentTests
    TerrainSedimentTests.cpp
)

target_link_libraries(OrbitTerrainSedimentTests
    PRIVATE
        Orbit::TerrainErosion
)

target_compile_features(OrbitTerrainSedimentTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitTerrainSedimentTests)

add_test(
    NAME Orbit.TerrainSediment
    COMMAND OrbitTerrainSedimentTests
)


add_executable(OrbitHydrologyRefinementTests
    HydrologyRefinementTests.cpp
)

target_link_libraries(OrbitHydrologyRefinementTests
    PRIVATE
        Orbit::TerrainErosion
)

target_compile_features(OrbitHydrologyRefinementTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitHydrologyRefinementTests)

add_test(
    NAME Orbit.HydrologyRefinement
    COMMAND OrbitHydrologyRefinementTests
)


add_executable(OrbitRegionalElevationDeltaTests
    RegionalElevationDeltaTests.cpp
)

target_link_libraries(OrbitRegionalElevationDeltaTests
    PRIVATE
        Orbit::TerrainErosion
)

target_compile_features(OrbitRegionalElevationDeltaTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitRegionalElevationDeltaTests)

add_test(
    NAME Orbit.RegionalElevationDelta
    COMMAND OrbitRegionalElevationDeltaTests
)


add_executable(OrbitDerivedTerrainRegionTests
    DerivedTerrainRegionTests.cpp
)

target_link_libraries(OrbitDerivedTerrainRegionTests
    PRIVATE
        Orbit::TerrainRegion
)

target_compile_features(OrbitDerivedTerrainRegionTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitDerivedTerrainRegionTests)

add_test(
    NAME Orbit.DerivedTerrainRegion
    COMMAND OrbitDerivedTerrainRegionTests
)


add_executable(OrbitDerivedTerrainRegionGpuTests
    DerivedTerrainRegionGpuTests.cpp
)

target_link_libraries(OrbitDerivedTerrainRegionGpuTests
    PRIVATE
        Orbit::RhiVulkan
        Orbit::ShaderDxc
        Orbit::Terrain
        Orbit::TerrainGpu
        Orbit::TerrainRegion
)

target_compile_features(OrbitDerivedTerrainRegionGpuTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitDerivedTerrainRegionGpuTests)

add_custom_command(TARGET OrbitDerivedTerrainRegionGpuTests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ORBIT_DXCOMPILER_DLL}"
        "$<TARGET_FILE_DIR:OrbitDerivedTerrainRegionGpuTests>"
)

add_test(
    NAME Orbit.DerivedTerrainRegionGpu
    COMMAND OrbitDerivedTerrainRegionGpuTests
)


add_executable(OrbitPlanetTileNeighborhoodTests
    PlanetTileNeighborhoodTests.cpp
)

target_link_libraries(OrbitPlanetTileNeighborhoodTests
    PRIVATE
        Orbit::World
)

target_compile_features(OrbitPlanetTileNeighborhoodTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitPlanetTileNeighborhoodTests)

add_test(
    NAME Orbit.PlanetTileNeighborhood
    COMMAND OrbitPlanetTileNeighborhoodTests
)


add_executable(OrbitDerivedTerrainStreamingTests
    DerivedTerrainStreamingTests.cpp
)

target_link_libraries(OrbitDerivedTerrainStreamingTests
    PRIVATE
        Orbit::TerrainRegion
)

target_compile_features(OrbitDerivedTerrainStreamingTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitDerivedTerrainStreamingTests)

add_test(
    NAME Orbit.DerivedTerrainStreaming
    COMMAND OrbitDerivedTerrainStreamingTests
)


add_executable(OrbitRiverWaterTests
    RiverWaterTests.cpp
)

target_link_libraries(OrbitRiverWaterTests
    PRIVATE
        Orbit::TerrainWater
)

target_compile_features(OrbitRiverWaterTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitRiverWaterTests)

add_test(
    NAME Orbit.RiverWater
    COMMAND OrbitRiverWaterTests
)


add_executable(OrbitLakeWaterTests
    LakeWaterTests.cpp
)

target_link_libraries(OrbitLakeWaterTests
    PRIVATE
        Orbit::TerrainWater
)

target_compile_features(OrbitLakeWaterTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitLakeWaterTests)

add_test(
    NAME Orbit.LakeWater
    COMMAND OrbitLakeWaterTests
)


add_executable(OrbitOceanMeshTests
    OceanMeshTests.cpp
)

target_link_libraries(OrbitOceanMeshTests
    PRIVATE
        Orbit::WaterRender
)

target_compile_features(OrbitOceanMeshTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitOceanMeshTests)

add_test(
    NAME Orbit.OceanMesh
    COMMAND OrbitOceanMeshTests
)

add_executable(OrbitStandingWaterTests StandingWaterTests.cpp)
target_link_libraries(OrbitStandingWaterTests PRIVATE Orbit::TerrainRegion Orbit::TerrainCache Orbit::TerrainStream)
target_compile_features(OrbitStandingWaterTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitStandingWaterTests)
add_test(NAME Orbit.StandingWater COMMAND OrbitStandingWaterTests)


# These are real Vulkan execution tests, not compile-only tests. GitHub's
# hosted Windows runners do not expose a Vulkan physical device, so CI builds
# every target but excludes this label from execution. Developer/workstation
# ctest runs still execute them normally on Vulkan-capable hardware.
set_tests_properties(
    Orbit.RhiCompute
    Orbit.TerrainGpuField
    Orbit.TerrainGpuRegionDelta
    Orbit.TerrainGpuElevationQuery
    Orbit.HydrologyGpuDepressionFill
    Orbit.HydrologyGpuFlowAccumulation
    Orbit.HydrologyGpuErosion
    Orbit.HydrologyGpuRiverGeometry
    Orbit.HydrologyGpuLakeBasins
    Orbit.DerivedTerrainRegionGpu
    PROPERTIES
        LABELS "gpu"
)


add_executable(OrbitTimeAndIdTests
    TimeAndIdTests.cpp
)

target_link_libraries(OrbitTimeAndIdTests
    PRIVATE
        Orbit::Core
        Orbit::Time
)

target_compile_features(OrbitTimeAndIdTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitTimeAndIdTests)

add_test(
    NAME Orbit.TimeAndIds
    COMMAND OrbitTimeAndIdTests
)


add_executable(OrbitFrameGraphTests
    FrameGraphTests.cpp
)

target_link_libraries(OrbitFrameGraphTests
    PRIVATE
        Orbit::Frames
)

target_compile_features(OrbitFrameGraphTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitFrameGraphTests)

add_test(
    NAME Orbit.FrameGraph
    COMMAND OrbitFrameGraphTests
)


add_executable(OrbitBodyRegistryTests
    BodyRegistryTests.cpp
)

target_link_libraries(OrbitBodyRegistryTests
    PRIVATE
        Orbit::Universe
)

target_compile_features(OrbitBodyRegistryTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitBodyRegistryTests)

add_test(
    NAME Orbit.BodyRegistry
    COMMAND OrbitBodyRegistryTests
)


add_executable(OrbitSurfaceRegistryTests
    SurfaceRegistryTests.cpp
)

target_link_libraries(OrbitSurfaceRegistryTests
    PRIVATE
        Orbit::Surface
)

target_compile_features(OrbitSurfaceRegistryTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitSurfaceRegistryTests)

add_test(
    NAME Orbit.SurfaceRegistry
    COMMAND OrbitSurfaceRegistryTests
)


add_executable(OrbitFieldRegistryTests
    FieldRegistryTests.cpp
)

target_link_libraries(OrbitFieldRegistryTests
    PRIVATE
        Orbit::Fields
)

target_compile_features(OrbitFieldRegistryTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitFieldRegistryTests)

add_test(
    NAME Orbit.FieldRegistry
    COMMAND OrbitFieldRegistryTests
)


add_executable(OrbitProceduralGraphTests
    ProceduralGraphTests.cpp
)

target_link_libraries(OrbitProceduralGraphTests
    PRIVATE
        Orbit::ProceduralGraph
)

target_compile_features(OrbitProceduralGraphTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitProceduralGraphTests)

add_test(
    NAME Orbit.ProceduralGraph
    COMMAND OrbitProceduralGraphTests
)


add_executable(OrbitDocumentsTests
    DocumentsTests.cpp
)

target_link_libraries(OrbitDocumentsTests
    PRIVATE
        Orbit::Documents
        SQLiteCpp
)

target_compile_features(OrbitDocumentsTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitDocumentsTests)

add_test(
    NAME Orbit.Documents
    COMMAND OrbitDocumentsTests
)


add_executable(OrbitAuthoringModelTests
    AuthoringModelTests.cpp
)

target_link_libraries(OrbitAuthoringModelTests
    PRIVATE
        Orbit::Commands
        Orbit::Documents
        Orbit::Scene
        Orbit::Schema
        Orbit::Selection
)

target_compile_features(OrbitAuthoringModelTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitAuthoringModelTests)

add_test(
    NAME Orbit.AuthoringModel
    COMMAND OrbitAuthoringModelTests
)


add_executable(OrbitRenderGraphTests
    RenderGraphTests.cpp
)

target_link_libraries(OrbitRenderGraphTests
    PRIVATE
        Orbit::RenderGraph
        Orbit::RenderView
)

target_compile_features(OrbitRenderGraphTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitRenderGraphTests)

add_test(
    NAME Orbit.RenderGraph
    COMMAND OrbitRenderGraphTests
)


add_executable(OrbitEditorModelTests
    EditorModelTests.cpp
)

target_link_libraries(OrbitEditorModelTests
    PRIVATE
        Orbit::Core
        Orbit::EditorModel
)

target_compile_features(OrbitEditorModelTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitEditorModelTests)

add_test(
    NAME Orbit.EditorModel
    COMMAND OrbitEditorModelTests
)


add_executable(OrbitExplorerInspectorTests
    ExplorerInspectorTests.cpp
)

target_link_libraries(OrbitExplorerInspectorTests
    PRIVATE
        Orbit::Commands
        Orbit::Documents
        Orbit::EditorModel
        Orbit::Scene
        Orbit::Schema
        Orbit::Selection
)

target_compile_features(OrbitExplorerInspectorTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitExplorerInspectorTests)

add_test(
    NAME Orbit.ExplorerInspector
    COMMAND OrbitExplorerInspectorTests
)


add_executable(OrbitCommandSurfaceTests
    CommandSurfaceTests.cpp
)

target_link_libraries(OrbitCommandSurfaceTests
    PRIVATE
        Orbit::Commands
        Orbit::Documents
        Orbit::EditorModel
        Orbit::Scene
        Orbit::Schema
        Orbit::Selection
)

target_compile_features(OrbitCommandSurfaceTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitCommandSurfaceTests)

add_test(
    NAME Orbit.CommandSurfaces
    COMMAND OrbitCommandSurfaceTests
)


add_executable(OrbitV003ScaffoldIntegrationTests
    V003ScaffoldIntegrationTests.cpp
)

target_link_libraries(OrbitV003ScaffoldIntegrationTests
    PRIVATE
        Orbit::Build
        Orbit::Content
        Orbit::ContentWic
        Orbit::EditorModel
        Orbit::EditorRpc
        Orbit::Frames
        Orbit::Paths
        Orbit::PlatformServices
        Orbit::Plugins
        Orbit::Surface
        Orbit::Terrain
        Orbit::Universe
)

target_compile_features(OrbitV003ScaffoldIntegrationTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitV003ScaffoldIntegrationTests)
add_dependencies(OrbitV003ScaffoldIntegrationTests OrbitPlayer)
orbit_copy_dxc_runtime(OrbitV003ScaffoldIntegrationTests)

add_test(
    NAME Orbit.V003ScaffoldIntegration
    COMMAND OrbitV003ScaffoldIntegrationTests $<TARGET_FILE:OrbitPlayer>
)


# V0.0.4 M30 deterministic cross-system validation suite. Keep the twenty
# mandatory cases in this single acceptance target so milestone status cannot
# be inferred from unrelated unit tests.
add_executable(OrbitV004ValidationTests
    V004TerrainValidationTests.cpp
)

target_link_libraries(OrbitV004ValidationTests
    PRIVATE
        Orbit::EditorModel
        Orbit::SurfaceAuthoring
        Orbit::SurfaceModel
        Orbit::TerrainBiome
        Orbit::TerrainCache
        Orbit::TerrainDebug
        Orbit::TerrainDependency
        Orbit::TerrainErosion
        Orbit::TerrainGeology
        Orbit::TerrainGpu
        Orbit::TerrainHydrology
        Orbit::TerrainMacroGeology
        Orbit::TerrainMaterialColumn
        Orbit::TerrainRegion
        Orbit::TerrainRender
        Orbit::TerrainScatter
        Orbit::TerrainStream
        Orbit::TerrainView
)

target_compile_definitions(OrbitV004ValidationTests
    PRIVATE
        ORBIT_TERRAIN_GEOLOGY_REFERENCE_ASSET_DIR="${CMAKE_SOURCE_DIR}/engine/terrain_geology/assets/reference"
)

target_compile_features(OrbitV004ValidationTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitV004ValidationTests)

add_test(
    NAME Orbit.V004Validation
    COMMAND OrbitV004ValidationTests
)

# M30 performance measurement is intentionally a hardware diagnostic rather
# than a CTest pass/fail gate. It uses real Vulkan timestamps and reports
# adapter-specific timing while exact terrain-owned memory bytes remain
# comparable across machines.
add_executable(OrbitV004TerrainPerformance
    V004TerrainPerformance.cpp
)

target_link_libraries(OrbitV004TerrainPerformance
    PRIVATE
        Orbit::RhiVulkan
        Orbit::ShaderDxc
        Orbit::TerrainGpu
)

target_compile_features(
    OrbitV004TerrainPerformance PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitV004TerrainPerformance)
orbit_copy_dxc_runtime(OrbitV004TerrainPerformance)

# V0.0.4 M31 integration acceptance is compiled continuously but is not
# registered as an executable CTest until the validated M30 hardware capture
# opens the final integration gate.
option(
    ORBIT_ENABLE_V004_M31_GATE
    "Register the V0.0.4 M31 final integration acceptance"
    OFF
)

add_executable(OrbitV004IntegrationGateTests
    V004IntegrationGateTests.cpp
)

target_link_libraries(OrbitV004IntegrationGateTests
    PRIVATE
        Orbit::Commands
        Orbit::Documents
        Orbit::Jobs
        Orbit::Scene
        Orbit::Schema
        Orbit::SurfaceAuthoring
        Orbit::SurfaceModel
        Orbit::Terrain
        Orbit::TerrainBiome
        Orbit::TerrainDebug
        Orbit::TerrainDependency
        Orbit::TerrainErosion
        Orbit::TerrainGeology
        Orbit::TerrainGpu
        Orbit::TerrainHydrology
        Orbit::TerrainMacroGeology
        Orbit::TerrainMaterialColumn
        Orbit::TerrainRender
        Orbit::TerrainScatter
        Orbit::WorldModel
        SQLiteCpp
)

target_compile_features(
    OrbitV004IntegrationGateTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitV004IntegrationGateTests)

if(ORBIT_ENABLE_V004_M31_GATE)
    add_test(
        NAME Orbit.V004IntegrationGate
        COMMAND OrbitV004IntegrationGateTests
    )
endif()

