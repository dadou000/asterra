add_library(OrbitStudioUi STATIC
    src/CelestialAuthoringUi.cpp
    src/DebugViewUi.cpp
    src/DisplayDiagnosticsUi.cpp
    src/DisplayEyeRpc.cpp
    src/ImplementationPlan.cpp
    src/ProfilerModel.cpp
    src/ProfilerPanelRpc.cpp
    src/ProfilerUi.cpp
    src/PlanningUi.cpp
    src/ProjectAuthoringUi.cpp
    src/ProjectSettingsUi.cpp
    src/ReportsUi.cpp
    src/ReportThumbnailCache.cpp
    src/ShadingUi.cpp
    src/SimulationControlsUi.cpp
    src/StudioExpansionShell.cpp
    src/StudioFlatMap.cpp
    src/StudioFlatMapRpc.cpp
    src/StudioRenderViewRpc.cpp
    src/StudioRenderViewSet.cpp
    src/StudioSurfacePicking.cpp
    src/StudioTerrainDiagnosticOverlayGeometry.cpp
    src/StudioTerrainOverlayGeometry.cpp
    src/SurfaceAuthoringUi.cpp
    src/SurfaceVolumeDebugRenderer.cpp
    src/SystemViewUi.cpp
    src/VolumeAuthoringUi.cpp
    src/StudioViewContinuity.cpp
    src/StudioViewportCamera.cpp
    src/StudioViewportManipulatorUi.cpp
    src/StudioViewportNavigation.cpp
    src/StudioViewportPanels.cpp
    src/StudioViewportPostProcess.cpp
    src/StudioViewportTerrainPass.cpp
    src/StudioViewportVolumePass.cpp
    src/StudioViewportLightingPass.cpp
    src/StudioViewportAtmospherePass.cpp
    src/StudioViewportBodyPass.cpp
    src/StudioViewportRenderer.cpp
    src/StudioShellInspector.cpp
    src/StudioShellNavigation.cpp
    src/StudioShellQuickCreateBrowser.cpp
    src/StudioShellQuickCreatePopup.cpp
    src/StudioShellCommandPalettePopup.cpp
    src/StudioShellPersistence.cpp
    src/StudioShellTerrainTools.cpp
    src/StudioShellViewportBand.cpp
    src/StudioViewportCompose.cpp
    src/StudioViewportDisplaySettings.cpp
    src/StudioViewportOverlayGeometry.cpp
    src/StudioViewportPresentation.cpp
    src/StudioViewportPrimitives.cpp
    src/StudioViewportQuickCreate.cpp
    src/StudioViewportRenderUtilities.cpp
    src/StudioViewportSceneResolution.cpp
    src/StudioViewportTerrainResources.cpp
    src/ViewportCaptureService.cpp
    src/StudioTextDiagnosticsHud.cpp
    src/StudioViewportTextDiagnostics.cpp
    src/WorldDocumentsUi.cpp
)

add_library(Orbit::StudioUi ALIAS OrbitStudioUi)

target_include_directories(OrbitStudioUi
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitStudioUi
    PUBLIC
        Orbit::Camera
        Orbit::CelestialAppearance
        Orbit::CelestialAtmosphere
        Orbit::CelestialClouds
        Orbit::CelestialOcean
        Orbit::CelestialRings
        Orbit::CelestialGlobe
        Orbit::CelestialGiants
        Orbit::CelestialMagnetosphereRender
        Orbit::CelestialCompactRender
        Orbit::CelestialSmallBodies
        Orbit::CelestialFarRender
        Orbit::CelestialRepresentation
        Orbit::CelestialScheduler
        Orbit::CelestialRadiometry
        Orbit::Content
        Orbit::Core
        Orbit::Jobs
        Orbit::EditorModel
        Orbit::EditorUi
        Orbit::Lighting
        Orbit::MeshRender
        Orbit::Platform
        Orbit::PostProcess
        Orbit::RenderGraph
        Orbit::RenderView
        Orbit::Rpc
        Orbit::Shading
        Orbit::StudioReports
        Orbit::StudioSession
        Orbit::TerrainBake
        Orbit::TerrainDebug
        Orbit::TerrainGpu
        Orbit::TerrainRender
        Orbit::TerrainView
        Orbit::VolumeFields
        Orbit::VolumeSolver
        Orbit::VolumeRender
        Orbit::VolumeRepresentation
)

target_compile_features(OrbitStudioUi PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitStudioUi)

if(ORBIT_ENABLE_VALIDATION_TOOLS)
    target_sources(OrbitStudioUi PRIVATE
        ${PROJECT_SOURCE_DIR}/tools/validation/SceneValidationCommands.cpp
        ${PROJECT_SOURCE_DIR}/tools/validation/SceneValidationScenarios.cpp)
    target_link_libraries(OrbitStudioUi PUBLIC OrbitTerrainValidationSupport)
    target_compile_definitions(OrbitStudioUi PUBLIC ORBIT_ENABLE_VALIDATION_TOOLS=1)
endif()

if(BUILD_TESTING)
    # Avoid compiling the same support twice in an optional validation build.
    if(ORBIT_ENABLE_VALIDATION_TOOLS)
        add_library(OrbitSceneValidationSupport INTERFACE)
        target_link_libraries(OrbitSceneValidationSupport INTERFACE Orbit::StudioUi)
    else()
        add_library(OrbitSceneValidationSupport STATIC
            ${PROJECT_SOURCE_DIR}/tools/validation/SceneValidationCommands.cpp
            ${PROJECT_SOURCE_DIR}/tools/validation/SceneValidationScenarios.cpp)
        target_include_directories(OrbitSceneValidationSupport PUBLIC
            ${PROJECT_SOURCE_DIR}/tools/validation/include)
        target_link_libraries(OrbitSceneValidationSupport PUBLIC Orbit::StudioUi)
        target_compile_features(OrbitSceneValidationSupport PUBLIC cxx_std_23)
        orbit_enable_warnings(OrbitSceneValidationSupport)
    endif()
    add_executable(OrbitStudioTerrainDiagnosticOverlayGeometryTests
        tests/StudioTerrainDiagnosticOverlayGeometryTests.cpp
    )

    target_link_libraries(OrbitStudioTerrainDiagnosticOverlayGeometryTests
        PRIVATE
            Orbit::StudioUi
    )

    target_compile_features(
        OrbitStudioTerrainDiagnosticOverlayGeometryTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitStudioTerrainDiagnosticOverlayGeometryTests)

    add_test(
        NAME Orbit.StudioTerrainDiagnosticOverlayGeometry
        COMMAND OrbitStudioTerrainDiagnosticOverlayGeometryTests
    )

    add_executable(OrbitStudioTerrainOverlayGeometryTests
        tests/StudioTerrainOverlayGeometryTests.cpp
    )

    target_link_libraries(OrbitStudioTerrainOverlayGeometryTests
        PRIVATE
            Orbit::StudioUi
    )

    target_compile_features(
        OrbitStudioTerrainOverlayGeometryTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitStudioTerrainOverlayGeometryTests)

    add_test(
        NAME Orbit.StudioTerrainOverlayGeometry
        COMMAND OrbitStudioTerrainOverlayGeometryTests
    )

    add_executable(OrbitStudioFlatMapTests
        tests/StudioFlatMapTests.cpp
    )

    target_link_libraries(OrbitStudioFlatMapTests
        PRIVATE
            Orbit::StudioUi
    )

    target_compile_features(OrbitStudioFlatMapTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitStudioFlatMapTests)

    add_test(
        NAME Orbit.StudioFlatMap
        COMMAND OrbitStudioFlatMapTests
    )

    add_executable(OrbitStudioSurfacePickingTests
        tests/StudioSurfacePickingTests.cpp
    )

    target_link_libraries(OrbitStudioSurfacePickingTests
        PRIVATE
            Orbit::StudioUi
    )

    target_compile_features(
        OrbitStudioSurfacePickingTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitStudioSurfacePickingTests)

    add_test(
        NAME Orbit.StudioSurfacePicking
        COMMAND OrbitStudioSurfacePickingTests
    )

    add_executable(OrbitStudioViewportCameraTests
        tests/StudioViewportCameraTests.cpp
    )

    target_link_libraries(OrbitStudioViewportCameraTests
        PRIVATE
            Orbit::StudioUi
    )

    target_compile_features(OrbitStudioViewportCameraTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitStudioViewportCameraTests)

    add_test(
        NAME Orbit.StudioViewportCamera
        COMMAND OrbitStudioViewportCameraTests
    )

    add_executable(OrbitStudioViewportNavigationTests
        tests/StudioViewportNavigationTests.cpp
    )

    target_link_libraries(OrbitStudioViewportNavigationTests
        PRIVATE
            Orbit::StudioUi
    )

    target_compile_features(
        OrbitStudioViewportNavigationTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitStudioViewportNavigationTests)

    add_test(
        NAME Orbit.StudioViewportNavigation
        COMMAND OrbitStudioViewportNavigationTests
    )

    add_executable(OrbitStudioViewportPresentationTests
        tests/StudioViewportPresentationTests.cpp
    )

    target_link_libraries(OrbitStudioViewportPresentationTests
        PRIVATE
            Orbit::StudioUi
    )

    target_compile_features(
        OrbitStudioViewportPresentationTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitStudioViewportPresentationTests)

    add_test(
        NAME Orbit.StudioViewportPresentation
        COMMAND OrbitStudioViewportPresentationTests
    )

    add_executable(OrbitVolumeSurfaceEffectRenderBridgeTests
        tests/VolumeSurfaceEffectRenderBridgeTests.cpp
    )

    target_link_libraries(OrbitVolumeSurfaceEffectRenderBridgeTests
        PRIVATE
            Orbit::StudioUi
    )

    target_compile_features(
        OrbitVolumeSurfaceEffectRenderBridgeTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitVolumeSurfaceEffectRenderBridgeTests)

    add_test(
        NAME Orbit.VolumeSurfaceEffectRenderBridge
        COMMAND OrbitVolumeSurfaceEffectRenderBridgeTests
    )

    # M38 particle/foam/droplet rendering is GPU-driven; this regression also
    # compiles the RHI indirect-draw contract used by the render bridge.
    add_executable(OrbitVolumeParticleRenderBridgeTests
        tests/VolumeParticleRenderBridgeTests.cpp
    )

    target_link_libraries(OrbitVolumeParticleRenderBridgeTests
        PRIVATE
            Orbit::StudioUi
    )

    target_compile_features(OrbitVolumeParticleRenderBridgeTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitVolumeParticleRenderBridgeTests)

    add_test(
        NAME Orbit.VolumeParticleRenderBridge
        COMMAND OrbitVolumeParticleRenderBridgeTests
    )

    # M39 is the end-to-end Studio workflow gate over the already-shipped
    # M30-M38 volume stack. It pins the real panel wiring together with cache,
    # lighting, representation and output contracts so later UI changes cannot
    # silently disconnect one part of the workflow.
    add_executable(OrbitVolumeAuthoringWorkflowTests
        tests/VolumeAuthoringWorkflowTests.cpp
    )

    target_link_libraries(OrbitVolumeAuthoringWorkflowTests
        PRIVATE
            Orbit::StudioUi
    )

    target_compile_features(
        OrbitVolumeAuthoringWorkflowTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitVolumeAuthoringWorkflowTests)

    add_test(
        NAME Orbit.VolumeAuthoringWorkflow
        COMMAND OrbitVolumeAuthoringWorkflowTests
    )

    add_executable(OrbitStudioLightingOverlayGeometryTests
        tests/StudioLightingOverlayGeometryTests.cpp
    )

    target_link_libraries(OrbitStudioLightingOverlayGeometryTests
        PRIVATE
            Orbit::StudioUi
    )

    target_compile_features(
        OrbitStudioLightingOverlayGeometryTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitStudioLightingOverlayGeometryTests)

    add_test(
        NAME Orbit.StudioLightingOverlayGeometry
        COMMAND OrbitStudioLightingOverlayGeometryTests
    )

    add_executable(OrbitLightingSelectionInspectionTests
        tests/LightingSelectionInspectionTests.cpp
    )

    target_link_libraries(OrbitLightingSelectionInspectionTests
        PRIVATE
            Orbit::StudioUi
    )

    target_compile_features(
        OrbitLightingSelectionInspectionTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitLightingSelectionInspectionTests)

    add_test(
        NAME Orbit.LightingSelectionInspection
        COMMAND OrbitLightingSelectionInspectionTests
    )

    add_executable(OrbitViewportCaptureTilingTests
        tests/ViewportCaptureTilingTests.cpp
    )

    target_link_libraries(OrbitViewportCaptureTilingTests
        PRIVATE
            Orbit::StudioUi
    )

    target_compile_features(OrbitViewportCaptureTilingTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitViewportCaptureTilingTests)

    add_test(
        NAME Orbit.ViewportCaptureTiling
        COMMAND OrbitViewportCaptureTilingTests
    )

    add_executable(OrbitStudioViewContinuityTests
        tests/StudioViewContinuityTests.cpp
    )

    target_link_libraries(OrbitStudioViewContinuityTests
        PRIVATE
            Orbit::StudioUi
    )

    target_compile_features(OrbitStudioViewContinuityTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitStudioViewContinuityTests)

    add_test(
        NAME Orbit.StudioViewContinuity
        COMMAND OrbitStudioViewContinuityTests
    )

    add_executable(OrbitProfilerModelTests
        tests/ProfilerModelTests.cpp
    )

    target_link_libraries(OrbitProfilerModelTests
        PRIVATE
            Orbit::StudioUi
    )

    target_compile_features(OrbitProfilerModelTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitProfilerModelTests)

    add_test(
        NAME Orbit.ProfilerModel
        COMMAND OrbitProfilerModelTests
    )

    add_executable(OrbitRuntimeProfilerSnapshotTests
        tests/RuntimeProfilerSnapshotTests.cpp
    )

    target_link_libraries(OrbitRuntimeProfilerSnapshotTests
        PRIVATE
            Orbit::StudioUi
    )

    target_compile_features(
        OrbitRuntimeProfilerSnapshotTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitRuntimeProfilerSnapshotTests)

    add_test(
        NAME Orbit.RuntimeProfilerSnapshot
        COMMAND OrbitRuntimeProfilerSnapshotTests
    )

    # M43 numerical gates are deterministic CPU/service invariants. GPU image
    # comparisons stay a separate hardware/tolerance gate by design.
    add_executable(OrbitSceneDeterministicValidationTests
        tests/SceneDeterministicValidationTests.cpp
    )

    target_link_libraries(OrbitSceneDeterministicValidationTests
        PRIVATE
            OrbitSceneValidationSupport
            Orbit::StudioUi
    )

    target_compile_features(
        OrbitSceneDeterministicValidationTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitSceneDeterministicValidationTests)

    add_test(
        NAME Orbit.SceneDeterministicValidation
        COMMAND OrbitSceneDeterministicValidationTests
    )

    # M44 pins the durable report schema plus RT/non-RT budget independence and
    # near/far volume Auto work reduction. Real GPU captures remain a hardware
    # validation activity and are never fabricated by this CPU regression.
    # M45 destroys and reconstructs project/session/content authority around a
    # real SQLite world checkpoint, material asset, project display settings
    # and .orbitvol cache. Runtime-only radiance state is required to start
    # empty so the reopened scene converges from persisted physical authority.
    add_executable(OrbitProjectPersistenceEquivalenceTests
        tests/ProjectPersistenceEquivalenceTests.cpp
    )

    target_link_libraries(OrbitProjectPersistenceEquivalenceTests
        PRIVATE
            Orbit::StudioUi
    )

    target_compile_features(
        OrbitProjectPersistenceEquivalenceTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitProjectPersistenceEquivalenceTests)

    add_test(
        NAME Orbit.ProjectPersistenceEquivalence
        COMMAND OrbitProjectPersistenceEquivalenceTests
    )

    # M46 is the release-level architectural smoke gate. Detailed subsystem
    # assertions remain in their original tests; this target pins the shared
    # cross-system contracts and is ordered after the major 0.0.7 gates.
    add_executable(OrbitStudioRenderingIntegrationTests
        tests/StudioRenderingIntegrationTests.cpp
    )

    target_link_libraries(OrbitStudioRenderingIntegrationTests
        PRIVATE
            OrbitSceneValidationSupport
            Orbit::StudioUi
    )

    target_compile_features(
        OrbitStudioRenderingIntegrationTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitStudioRenderingIntegrationTests)

    add_test(
        NAME Orbit.StudioRenderingIntegration
        COMMAND OrbitStudioRenderingIntegrationTests
    )

    add_executable(OrbitStudioShellModelTests
        tests/StudioShellModelTests.cpp
    )

    target_link_libraries(OrbitStudioShellModelTests
        PRIVATE
            Orbit::StudioUi
    )

    target_compile_features(
        OrbitStudioShellModelTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitStudioShellModelTests)

    add_test(
        NAME Orbit.StudioShellModel
        COMMAND OrbitStudioShellModelTests
    )

    add_executable(OrbitStudioInspectorExtensionTests
        tests/StudioInspectorExtensionTests.cpp
    )

    target_link_libraries(OrbitStudioInspectorExtensionTests
        PRIVATE
            Orbit::StudioUi
    )

    target_compile_features(
        OrbitStudioInspectorExtensionTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitStudioInspectorExtensionTests)

    add_test(
        NAME Orbit.StudioInspectorExtension
        COMMAND OrbitStudioInspectorExtensionTests
    )

    set_tests_properties(
        Orbit.StudioRenderingIntegration
        PROPERTIES
            DEPENDS "Orbit.VolumeAuthoringWorkflow;Orbit.StudioLightingOverlayGeometry;Orbit.LightingSelectionInspection;Orbit.RuntimeProfilerSnapshot;Orbit.SceneDeterministicValidation;Orbit.ProjectPersistenceEquivalence"
    )
endif()
