add_library(OrbitStudioUi STATIC
    src/CelestialAuthoringUi.cpp
    src/DisplayDiagnosticsUi.cpp
    src/ProjectAuthoringUi.cpp
    src/ProjectSettingsUi.cpp
    src/StudioRenderViewSet.cpp
    src/StudioSurfacePicking.cpp
    src/StudioTerrainDiagnosticOverlayGeometry.cpp
    src/StudioTerrainOverlayGeometry.cpp
    src/StudioUiBundle.cpp
    src/SurfaceAuthoringUi.cpp
    src/SurfaceVolumeDebugRenderer.cpp
    src/SystemViewUi.cpp
    src/V007PerformanceCapture.cpp
    src/V007ValidationCommands.cpp
    src/V007ValidationScenarios.cpp
    src/VolumeAuthoringM36.cpp
    src/StudioViewportCamera.cpp
    src/StudioViewportNavigation.cpp
    src/StudioViewportPanels.cpp
    src/StudioViewportRenderer.cpp
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
        Orbit::EditorModel
        Orbit::EditorUi
        Orbit::Lighting
        Orbit::PostProcess
        Orbit::RenderGraph
        Orbit::RenderView
        Orbit::StudioSession
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

if(BUILD_TESTING)
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
    add_executable(OrbitV007DeterministicValidationTests
        tests/V007DeterministicValidationTests.cpp
    )

    target_link_libraries(OrbitV007DeterministicValidationTests
        PRIVATE
            Orbit::StudioUi
    )

    target_compile_features(
        OrbitV007DeterministicValidationTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitV007DeterministicValidationTests)

    add_test(
        NAME Orbit.V007DeterministicValidation
        COMMAND OrbitV007DeterministicValidationTests
    )

    # M44 pins the durable report schema plus RT/non-RT budget independence and
    # near/far volume Auto work reduction. Real GPU captures remain a hardware
    # validation activity and are never fabricated by this CPU regression.
    add_executable(OrbitV007PerformanceCaptureTests
        tests/V007PerformanceCaptureTests.cpp
    )

    target_link_libraries(OrbitV007PerformanceCaptureTests
        PRIVATE
            Orbit::StudioUi
    )

    target_compile_features(
        OrbitV007PerformanceCaptureTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitV007PerformanceCaptureTests)

    add_test(
        NAME Orbit.V007PerformanceCapture
        COMMAND OrbitV007PerformanceCaptureTests
    )

    # M45 destroys and reconstructs project/session/content authority around a
    # real SQLite world checkpoint, material asset, project display settings
    # and .orbitvol cache. Runtime-only radiance state is required to start
    # empty so the reopened scene converges from persisted physical authority.
    add_executable(OrbitV007ProjectEquivalenceTests
        tests/V007ProjectEquivalenceTests.cpp
    )

    target_link_libraries(OrbitV007ProjectEquivalenceTests
        PRIVATE
            Orbit::StudioUi
    )

    target_compile_features(
        OrbitV007ProjectEquivalenceTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitV007ProjectEquivalenceTests)

    add_test(
        NAME Orbit.V007ProjectEquivalence
        COMMAND OrbitV007ProjectEquivalenceTests
    )

    # M46 is the release-level architectural smoke gate. Detailed subsystem
    # assertions remain in their original tests; this target pins the shared
    # cross-system contracts and is ordered after the major 0.0.7 gates.
    add_executable(OrbitV007IntegrationGateTests
        tests/V007IntegrationGateTests.cpp
    )

    target_link_libraries(OrbitV007IntegrationGateTests
        PRIVATE
            Orbit::StudioUi
    )

    target_compile_features(
        OrbitV007IntegrationGateTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitV007IntegrationGateTests)

    add_test(
        NAME Orbit.V007IntegrationGate
        COMMAND OrbitV007IntegrationGateTests
    )

    set_tests_properties(
        Orbit.V007IntegrationGate
        PROPERTIES
            DEPENDS "Orbit.VolumeAuthoringWorkflow;Orbit.StudioLightingOverlayGeometry;Orbit.LightingSelectionInspection;Orbit.RuntimeProfilerSnapshot;Orbit.V007DeterministicValidation;Orbit.V007PerformanceCapture;Orbit.V007ProjectEquivalence"
    )
endif()
