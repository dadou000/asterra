add_library(OrbitStudioSession STATIC
    src/BezierHandleEditor.cpp
    src/StudioAuthoringBinding.cpp
    src/StudioSession.cpp
    src/StudioRuntimeBinding.cpp
    src/StudioTerrainAuthoringInvalidation.cpp
    src/StudioTerrainRebuildScheduler.cpp
    src/StudioTerrainPhysicalPageService.cpp
    src/StudioTerrainRoundTripVerifier.cpp
    src/StudioTerrainRuntimeBridge.cpp
    src/StudioTerrainServiceStatus.cpp
    src/StudioWorkspace.cpp
    src/StudioWorkspaceRpcHost.cpp
    src/ProjectBrowserModel.cpp
    src/ProjectSettingsModel.cpp
    src/UniverseBoundPathCache.cpp
    src/UniverseBoundRoutePlanner.cpp
    src/ViewportTargetRegistry.cpp
    src/ViewportTargetRpc.cpp
    src/WorldBoundPathNetwork.cpp
)

add_library(Orbit::StudioSession ALIAS OrbitStudioSession)

target_include_directories(OrbitStudioSession
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitStudioSession
    PUBLIC
        Orbit::Documents
        Orbit::EditorRpc
        Orbit::EditorSession
        Orbit::Jobs
        Orbit::PathGeometry
        Orbit::PathRouting
        Orbit::Paths
        Orbit::Rpc
        Orbit::SurfaceModel
        Orbit::Terrain
        Orbit::TerrainDebug
        Orbit::TerrainDependency
        Orbit::TerrainGpu
        Orbit::TerrainHydrology
        Orbit::TerrainMacroGeology
        Orbit::TerrainMaterialColumn
        Orbit::TerrainScatter
        Orbit::TerrainStream
        Orbit::TerrainView
        Orbit::World
)

target_compile_features(OrbitStudioSession PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitStudioSession)

if(BUILD_TESTING)
    add_executable(OrbitStudioSessionTests
        tests/StudioSessionTests.cpp
    )

    target_link_libraries(OrbitStudioSessionTests
        PRIVATE
            Orbit::StudioSession
            Orbit::WorldModel
    )

    target_compile_features(OrbitStudioSessionTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitStudioSessionTests)

    add_test(
        NAME Orbit.StudioSession
        COMMAND OrbitStudioSessionTests
    )

    add_executable(OrbitStudioRuntimeBindingTests
        tests/StudioRuntimeBindingTests.cpp
    )

    target_link_libraries(OrbitStudioRuntimeBindingTests
        PRIVATE
            Orbit::StudioSession
            Orbit::WorldModel
    )

    target_compile_features(OrbitStudioRuntimeBindingTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitStudioRuntimeBindingTests)

    add_test(
        NAME Orbit.StudioRuntimeBinding
        COMMAND OrbitStudioRuntimeBindingTests
    )



    add_executable(OrbitStudioTerrainAuthoringInvalidationTests
        tests/StudioTerrainAuthoringInvalidationTests.cpp
    )

    target_link_libraries(OrbitStudioTerrainAuthoringInvalidationTests
        PRIVATE
            Orbit::StudioSession
    )

    target_compile_features(
        OrbitStudioTerrainAuthoringInvalidationTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitStudioTerrainAuthoringInvalidationTests)

    add_test(
        NAME Orbit.StudioTerrainAuthoringInvalidation
        COMMAND OrbitStudioTerrainAuthoringInvalidationTests
    )

    add_executable(OrbitStudioTerrainRebuildSchedulerTests
        tests/StudioTerrainRebuildSchedulerTests.cpp
    )

    target_link_libraries(OrbitStudioTerrainRebuildSchedulerTests
        PRIVATE
            Orbit::StudioSession
    )

    target_compile_features(
        OrbitStudioTerrainRebuildSchedulerTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitStudioTerrainRebuildSchedulerTests)

    add_test(
        NAME Orbit.StudioTerrainRebuildScheduler
        COMMAND OrbitStudioTerrainRebuildSchedulerTests
    )

    add_executable(OrbitStudioTerrainPhysicalPageServiceTests
        tests/StudioTerrainPhysicalPageServiceTests.cpp
    )

    target_link_libraries(OrbitStudioTerrainPhysicalPageServiceTests
        PRIVATE
            Orbit::StudioSession
            Orbit::WorldModel
    )

    target_compile_features(
        OrbitStudioTerrainPhysicalPageServiceTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitStudioTerrainPhysicalPageServiceTests)

    add_test(
        NAME Orbit.StudioTerrainPhysicalPageService
        COMMAND OrbitStudioTerrainPhysicalPageServiceTests
    )

    add_executable(OrbitStudioTerrainRoundTripVerifierTests
        tests/StudioTerrainRoundTripVerifierTests.cpp
    )

    target_link_libraries(OrbitStudioTerrainRoundTripVerifierTests
        PRIVATE
            Orbit::EditorModel
            Orbit::StudioSession
            Orbit::WorldModel
    )

    target_compile_features(
        OrbitStudioTerrainRoundTripVerifierTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitStudioTerrainRoundTripVerifierTests)

    add_test(
        NAME Orbit.StudioTerrainRoundTripVerifier
        COMMAND OrbitStudioTerrainRoundTripVerifierTests
    )

    add_executable(OrbitStudioTerrainServiceStatusTests
        tests/StudioTerrainServiceStatusTests.cpp
    )

    target_link_libraries(OrbitStudioTerrainServiceStatusTests
        PRIVATE
            Orbit::EditorModel
            Orbit::StudioSession
            Orbit::WorldModel
    )

    target_compile_features(
        OrbitStudioTerrainServiceStatusTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitStudioTerrainServiceStatusTests)

    add_test(
        NAME Orbit.StudioTerrainServiceStatus
        COMMAND OrbitStudioTerrainServiceStatusTests
    )

    add_executable(OrbitStudioTerrainRuntimeBridgeTests
        tests/StudioTerrainRuntimeBridgeTests.cpp
    )

    target_link_libraries(OrbitStudioTerrainRuntimeBridgeTests
        PRIVATE
            Orbit::EditorModel
            Orbit::StudioSession
            Orbit::WorldModel
    )

    target_compile_features(
        OrbitStudioTerrainRuntimeBridgeTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitStudioTerrainRuntimeBridgeTests)

    add_test(
        NAME Orbit.StudioTerrainRuntimeBridge
        COMMAND OrbitStudioTerrainRuntimeBridgeTests
    )

    add_executable(OrbitStudioAuthoringBindingTests
        tests/StudioAuthoringBindingTests.cpp
    )

    target_link_libraries(OrbitStudioAuthoringBindingTests
        PRIVATE
            Orbit::StudioSession
            Orbit::WorldModel
    )

    target_compile_features(OrbitStudioAuthoringBindingTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitStudioAuthoringBindingTests)

    add_test(
        NAME Orbit.StudioAuthoringBinding
        COMMAND OrbitStudioAuthoringBindingTests
    )

    add_executable(OrbitBezierHandleEditorTests
        tests/BezierHandleEditorTests.cpp
    )

    target_link_libraries(OrbitBezierHandleEditorTests
        PRIVATE
            Orbit::StudioSession
    )

    target_compile_features(OrbitBezierHandleEditorTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitBezierHandleEditorTests)

    add_test(
        NAME Orbit.BezierHandleEditor
        COMMAND OrbitBezierHandleEditorTests
    )

    add_executable(OrbitStudioWorkspaceTests
        tests/StudioWorkspaceTests.cpp
    )

    target_link_libraries(OrbitStudioWorkspaceTests
        PRIVATE
            Orbit::StudioSession
            Orbit::WorldModel
    )

    target_compile_features(OrbitStudioWorkspaceTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitStudioWorkspaceTests)

    add_test(
        NAME Orbit.StudioWorkspace
        COMMAND OrbitStudioWorkspaceTests
    )

    add_executable(OrbitProjectBrowserModelTests
        tests/ProjectBrowserModelTests.cpp
    )

    target_link_libraries(OrbitProjectBrowserModelTests
        PRIVATE
            Orbit::StudioSession
    )

    target_compile_features(OrbitProjectBrowserModelTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitProjectBrowserModelTests)

    add_test(
        NAME Orbit.ProjectBrowserModel
        COMMAND OrbitProjectBrowserModelTests
    )

    add_executable(OrbitProjectSettingsModelTests
        tests/ProjectSettingsModelTests.cpp
    )

    target_link_libraries(OrbitProjectSettingsModelTests
        PRIVATE
            Orbit::StudioSession
    )

    target_compile_features(OrbitProjectSettingsModelTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitProjectSettingsModelTests)

    add_test(
        NAME Orbit.ProjectSettingsModel
        COMMAND OrbitProjectSettingsModelTests
    )

    add_executable(OrbitStudioWorkspaceRpcHostTests
        tests/StudioWorkspaceRpcHostTests.cpp
    )

    target_link_libraries(OrbitStudioWorkspaceRpcHostTests
        PRIVATE
            Orbit::StudioSession
            Orbit::WorldModel
    )

    target_compile_features(OrbitStudioWorkspaceRpcHostTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitStudioWorkspaceRpcHostTests)

    add_test(
        NAME Orbit.StudioWorkspaceRpcHost
        COMMAND OrbitStudioWorkspaceRpcHostTests
    )

    add_executable(OrbitUniverseBoundRoutePlannerTests
        tests/UniverseBoundRoutePlannerTests.cpp
    )

    target_link_libraries(OrbitUniverseBoundRoutePlannerTests
        PRIVATE
            Orbit::StudioSession
            Orbit::WorldModel
    )

    target_compile_features(OrbitUniverseBoundRoutePlannerTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitUniverseBoundRoutePlannerTests)

    add_test(
        NAME Orbit.UniverseBoundRoutePlanner
        COMMAND OrbitUniverseBoundRoutePlannerTests
    )

    add_executable(OrbitUniverseBoundPathCacheTests
        tests/UniverseBoundPathCacheTests.cpp
    )

    target_link_libraries(OrbitUniverseBoundPathCacheTests
        PRIVATE
            Orbit::StudioSession
            Orbit::WorldModel
    )

    target_compile_features(OrbitUniverseBoundPathCacheTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitUniverseBoundPathCacheTests)

    add_test(
        NAME Orbit.UniverseBoundPathCache
        COMMAND OrbitUniverseBoundPathCacheTests
    )

    add_executable(OrbitViewportTargetRegistryTests
        tests/ViewportTargetRegistryTests.cpp
    )

    target_link_libraries(OrbitViewportTargetRegistryTests
        PRIVATE
            Orbit::StudioSession
            Orbit::WorldModel
    )

    target_compile_features(OrbitViewportTargetRegistryTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitViewportTargetRegistryTests)

    add_test(
        NAME Orbit.ViewportTargetRegistry
        COMMAND OrbitViewportTargetRegistryTests
    )

    add_executable(OrbitViewportTargetRpcTests
        tests/ViewportTargetRpcTests.cpp
    )

    target_link_libraries(OrbitViewportTargetRpcTests
        PRIVATE
            Orbit::StudioSession
            Orbit::WorldModel
    )

    target_compile_features(OrbitViewportTargetRpcTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitViewportTargetRpcTests)

    add_test(
        NAME Orbit.ViewportTargetRpc
        COMMAND OrbitViewportTargetRpcTests
    )
endif()
