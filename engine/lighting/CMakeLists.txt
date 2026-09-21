add_library(OrbitLighting STATIC
    src/SurfaceData.cpp
    src/LocalLightRegistry.cpp
    src/SurfaceDebugRenderer.cpp
    src/LightingView.cpp
    src/DirectLighting.cpp
    src/Visibility.cpp
    src/AnalyticBodyVisibility.cpp
    src/TerrainHeightfieldVisibility.cpp
    src/ScreenSpaceVisibility.cpp
    src/SoftwareProxyVisibility.cpp
    src/HardwareRayQueryVisibility.cpp
    src/LightingScheduler.cpp
    src/RadianceClipmap.cpp
    src/RadianceClipmapResidency.cpp
    src/ScreenSpaceFinalGather.cpp
    src/RadianceEstimator.cpp
    src/SkyVisibility.cpp
    src/RadianceCacheSampler.cpp
    src/MaterialEmission.cpp
    src/MaterialEmissionSurfaceOverride.cpp
    src/EmissiveHierarchy.cpp
    src/EmissiveSampling.cpp
    src/EmissiveInvalidation.cpp
    src/RuntimeMaterialEmission.cpp
    src/RuntimeEmissiveSurface.cpp
    src/PlanetaryEmissionField.cpp
    src/PlanetaryEmissionAppearance.cpp
    src/PlanetaryEmissionService.cpp
    src/ReflectionPolicy.cpp
    src/HybridReflectionRenderer.cpp
    src/ExactReflectionQueryRenderer.cpp
    src/RepresentationLightingContinuity.cpp
)

add_library(Orbit::Lighting ALIAS OrbitLighting)

target_include_directories(OrbitLighting
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitLighting
    PUBLIC
        Orbit::Core
        Orbit::CelestialAppearance
        Orbit::Content
        Orbit::Frames
        Orbit::Math
        Orbit::RHI
        Orbit::Shader
        Orbit::Universe
        Orbit::Terrain
        Orbit::World
)

target_compile_features(OrbitLighting PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitLighting)

if(BUILD_TESTING)
    add_executable(OrbitLightingSurfaceDataTests
        tests/SurfaceDataTests.cpp
    )

    add_executable(OrbitLightingSurfaceBufferTests
        tests/SurfaceBufferTests.cpp
    )

    add_executable(OrbitLightingLocalLightRegistryTests
        tests/LocalLightRegistryTests.cpp
    )

    add_executable(OrbitLightingViewTests
        tests/LightingViewTests.cpp
    )

    add_executable(OrbitLightingVisibilityTests
        tests/VisibilityTests.cpp
    )

    add_executable(OrbitLightingAnalyticBodyVisibilityTests
        tests/AnalyticBodyVisibilityTests.cpp
    )

    add_executable(OrbitLightingTerrainHeightfieldVisibilityTests
        tests/TerrainHeightfieldVisibilityTests.cpp
    )

    add_executable(OrbitLightingScreenSpaceVisibilityTests
        tests/ScreenSpaceVisibilityTests.cpp
    )

    add_executable(OrbitLightingSoftwareProxyVisibilityTests
        tests/SoftwareProxyVisibilityTests.cpp
    )

    add_executable(OrbitLightingSchedulerTests
        tests/LightingSchedulerTests.cpp
    )

    add_executable(OrbitLightingRadianceClipmapTests
        tests/RadianceClipmapTests.cpp
    )

    add_executable(OrbitLightingRadianceClipmapResidencyTests
        tests/RadianceClipmapResidencyTests.cpp
    )

    add_executable(OrbitLightingScreenSpaceFinalGatherTests
        tests/ScreenSpaceFinalGatherTests.cpp
    )

    add_executable(OrbitLightingRadianceEstimatorTests
        tests/RadianceEstimatorTests.cpp
    )

    add_executable(OrbitLightingSkyVisibilityTests
        tests/SkyVisibilityTests.cpp
    )

    add_executable(OrbitLightingMaterialEmissionTests
        tests/MaterialEmissionTests.cpp
    )

    add_executable(OrbitLightingEmissiveHierarchyTests
        tests/EmissiveHierarchyTests.cpp
    )

    add_executable(OrbitLightingEmissiveSamplingTests
        tests/EmissiveSamplingTests.cpp
    )

    add_executable(OrbitLightingEmissiveEstimatorIntegrationTests
        tests/EmissiveEstimatorIntegrationTests.cpp
    )

    add_executable(OrbitLightingEmissiveInvalidationTests
        tests/EmissiveInvalidationTests.cpp
    )

    add_executable(OrbitLightingRuntimeMaterialEmissionTests
        tests/RuntimeMaterialEmissionTests.cpp
    )

    add_executable(OrbitLightingRuntimeEmissiveSurfaceTests
        tests/RuntimeEmissiveSurfaceTests.cpp
    )

    add_executable(OrbitLightingPlanetaryEmissionFieldTests
        tests/PlanetaryEmissionFieldTests.cpp
    )

    add_executable(OrbitLightingPlanetaryEmissionAppearanceTests
        tests/PlanetaryEmissionAppearanceTests.cpp
    )

    add_executable(OrbitLightingPlanetaryEmissionServiceTests
        tests/PlanetaryEmissionServiceTests.cpp
    )

    add_executable(OrbitLightingPlanetaryEmissionContinuityTests
        tests/PlanetaryEmissionContinuityTests.cpp
    )

    add_executable(OrbitLightingReflectionPolicyTests
        tests/ReflectionPolicyTests.cpp
    )

    add_executable(OrbitLightingRepresentationLightingContinuityTests
        tests/RepresentationLightingContinuityTests.cpp
    )

    add_executable(OrbitLightingRuntimeMaterialAssignmentIntegrationTests
        tests/RuntimeMaterialAssignmentIntegrationTests.cpp
    )

    target_link_libraries(OrbitLightingSurfaceDataTests
        PRIVATE
            Orbit::Lighting
    )

    target_compile_features(
        OrbitLightingSurfaceDataTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitLightingSurfaceDataTests)

    target_link_libraries(OrbitLightingSurfaceBufferTests
        PRIVATE
            Orbit::Lighting
    )
    target_compile_features(
        OrbitLightingSurfaceBufferTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitLightingSurfaceBufferTests)

    target_link_libraries(OrbitLightingLocalLightRegistryTests
        PRIVATE
            Orbit::Lighting
    )
    target_compile_features(
        OrbitLightingLocalLightRegistryTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitLightingLocalLightRegistryTests)

    target_link_libraries(OrbitLightingViewTests
        PRIVATE
            Orbit::Lighting
    )
    target_compile_features(
        OrbitLightingViewTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitLightingViewTests)

    target_link_libraries(OrbitLightingVisibilityTests
        PRIVATE
            Orbit::Lighting
    )
    target_compile_features(
        OrbitLightingVisibilityTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitLightingVisibilityTests)

    target_link_libraries(OrbitLightingAnalyticBodyVisibilityTests
        PRIVATE
            Orbit::Lighting
    )
    target_compile_features(
        OrbitLightingAnalyticBodyVisibilityTests PRIVATE cxx_std_23)
    orbit_enable_warnings(
        OrbitLightingAnalyticBodyVisibilityTests)

    target_link_libraries(OrbitLightingTerrainHeightfieldVisibilityTests
        PRIVATE
            Orbit::Lighting
    )
    target_compile_features(
        OrbitLightingTerrainHeightfieldVisibilityTests PRIVATE cxx_std_23)
    orbit_enable_warnings(
        OrbitLightingTerrainHeightfieldVisibilityTests)

    target_link_libraries(OrbitLightingScreenSpaceVisibilityTests
        PRIVATE
            Orbit::Lighting
    )
    target_compile_features(
        OrbitLightingScreenSpaceVisibilityTests PRIVATE cxx_std_23)
    orbit_enable_warnings(
        OrbitLightingScreenSpaceVisibilityTests)

    target_link_libraries(OrbitLightingSoftwareProxyVisibilityTests
        PRIVATE
            Orbit::Lighting
    )
    target_compile_features(
        OrbitLightingSoftwareProxyVisibilityTests PRIVATE cxx_std_23)
    orbit_enable_warnings(
        OrbitLightingSoftwareProxyVisibilityTests)

    target_link_libraries(OrbitLightingSchedulerTests
        PRIVATE
            Orbit::Lighting
    )
    target_compile_features(
        OrbitLightingSchedulerTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitLightingSchedulerTests)

    target_link_libraries(OrbitLightingRadianceClipmapTests
        PRIVATE
            Orbit::Lighting
    )
    target_compile_features(
        OrbitLightingRadianceClipmapTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitLightingRadianceClipmapTests)

    target_link_libraries(OrbitLightingRadianceClipmapResidencyTests
        PRIVATE
            Orbit::Lighting
    )
    target_compile_features(
        OrbitLightingRadianceClipmapResidencyTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitLightingRadianceClipmapResidencyTests)

    target_link_libraries(OrbitLightingScreenSpaceFinalGatherTests
        PRIVATE
            Orbit::Lighting
    )
    target_compile_features(
        OrbitLightingScreenSpaceFinalGatherTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitLightingScreenSpaceFinalGatherTests)

    target_link_libraries(OrbitLightingRadianceEstimatorTests
        PRIVATE
            Orbit::Lighting
    )
    target_compile_features(
        OrbitLightingRadianceEstimatorTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitLightingRadianceEstimatorTests)

    target_link_libraries(OrbitLightingSkyVisibilityTests
        PRIVATE
            Orbit::Lighting
    )
    target_compile_features(
        OrbitLightingSkyVisibilityTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitLightingSkyVisibilityTests)

    target_link_libraries(OrbitLightingMaterialEmissionTests
        PRIVATE
            Orbit::Lighting
    )
    target_compile_features(
        OrbitLightingMaterialEmissionTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitLightingMaterialEmissionTests)

    target_link_libraries(OrbitLightingEmissiveHierarchyTests
        PRIVATE
            Orbit::Lighting
    )
    target_compile_features(
        OrbitLightingEmissiveHierarchyTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitLightingEmissiveHierarchyTests)

    target_link_libraries(OrbitLightingEmissiveSamplingTests
        PRIVATE
            Orbit::Lighting
    )
    target_compile_features(
        OrbitLightingEmissiveSamplingTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitLightingEmissiveSamplingTests)

    target_link_libraries(OrbitLightingEmissiveEstimatorIntegrationTests
        PRIVATE
            Orbit::Lighting
    )
    target_compile_features(
        OrbitLightingEmissiveEstimatorIntegrationTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitLightingEmissiveEstimatorIntegrationTests)

    target_link_libraries(OrbitLightingEmissiveInvalidationTests
        PRIVATE
            Orbit::Lighting
    )
    target_compile_features(
        OrbitLightingEmissiveInvalidationTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitLightingEmissiveInvalidationTests)

    target_link_libraries(OrbitLightingRuntimeMaterialEmissionTests
        PRIVATE
            Orbit::Lighting
    )
    target_compile_features(
        OrbitLightingRuntimeMaterialEmissionTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitLightingRuntimeMaterialEmissionTests)

    target_link_libraries(OrbitLightingRuntimeEmissiveSurfaceTests
        PRIVATE
            Orbit::Lighting
    )
    target_compile_features(
        OrbitLightingRuntimeEmissiveSurfaceTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitLightingRuntimeEmissiveSurfaceTests)

    target_link_libraries(OrbitLightingPlanetaryEmissionFieldTests
        PRIVATE
            Orbit::Lighting
    )
    target_compile_features(
        OrbitLightingPlanetaryEmissionFieldTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitLightingPlanetaryEmissionFieldTests)

    target_link_libraries(OrbitLightingPlanetaryEmissionAppearanceTests
        PRIVATE
            Orbit::Lighting
    )
    target_compile_features(
        OrbitLightingPlanetaryEmissionAppearanceTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitLightingPlanetaryEmissionAppearanceTests)

    target_link_libraries(OrbitLightingPlanetaryEmissionServiceTests
        PRIVATE
            Orbit::Lighting
    )
    target_compile_features(
        OrbitLightingPlanetaryEmissionServiceTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitLightingPlanetaryEmissionServiceTests)

    target_link_libraries(OrbitLightingPlanetaryEmissionContinuityTests
        PRIVATE
            Orbit::Lighting
    )
    target_compile_features(
        OrbitLightingPlanetaryEmissionContinuityTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitLightingPlanetaryEmissionContinuityTests)

    target_link_libraries(OrbitLightingReflectionPolicyTests
        PRIVATE
            Orbit::Lighting
    )
    target_compile_features(
        OrbitLightingReflectionPolicyTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitLightingReflectionPolicyTests)

    target_link_libraries(OrbitLightingRepresentationLightingContinuityTests
        PRIVATE
            Orbit::Lighting
    )
    target_compile_features(
        OrbitLightingRepresentationLightingContinuityTests PRIVATE cxx_std_23)
    orbit_enable_warnings(
        OrbitLightingRepresentationLightingContinuityTests)

    target_link_libraries(OrbitLightingRuntimeMaterialAssignmentIntegrationTests
        PRIVATE
            Orbit::Commands
            Orbit::Documents
            Orbit::Lighting
            Orbit::WorldModel
    )
    target_compile_features(
        OrbitLightingRuntimeMaterialAssignmentIntegrationTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitLightingRuntimeMaterialAssignmentIntegrationTests)

    add_test(
        NAME Orbit.LightingView
        COMMAND OrbitLightingViewTests
    )

    add_test(
        NAME Orbit.LightingVisibility
        COMMAND OrbitLightingVisibilityTests
    )

    add_test(
        NAME Orbit.LightingAnalyticBodyVisibility
        COMMAND OrbitLightingAnalyticBodyVisibilityTests
    )

    add_test(
        NAME Orbit.LightingTerrainHeightfieldVisibility
        COMMAND OrbitLightingTerrainHeightfieldVisibilityTests
    )

    add_test(
        NAME Orbit.LightingScreenSpaceVisibility
        COMMAND OrbitLightingScreenSpaceVisibilityTests
    )

    add_test(
        NAME Orbit.LightingSoftwareProxyVisibility
        COMMAND OrbitLightingSoftwareProxyVisibilityTests
    )

    add_test(
        NAME Orbit.LightingScheduler
        COMMAND OrbitLightingSchedulerTests
    )

    add_test(
        NAME Orbit.LightingRadianceClipmap
        COMMAND OrbitLightingRadianceClipmapTests
    )

    add_test(
        NAME Orbit.LightingRadianceClipmapResidency
        COMMAND OrbitLightingRadianceClipmapResidencyTests
    )

    add_test(
        NAME Orbit.LightingScreenSpaceFinalGather
        COMMAND OrbitLightingScreenSpaceFinalGatherTests
    )

    add_test(
        NAME Orbit.LightingRadianceEstimator
        COMMAND OrbitLightingRadianceEstimatorTests
    )

    add_test(
        NAME Orbit.LightingSkyVisibility
        COMMAND OrbitLightingSkyVisibilityTests
    )

    add_test(
        NAME Orbit.LightingMaterialEmission
        COMMAND OrbitLightingMaterialEmissionTests
    )

    add_test(
        NAME Orbit.LightingEmissiveHierarchy
        COMMAND OrbitLightingEmissiveHierarchyTests
    )

    add_test(
        NAME Orbit.LightingEmissiveSampling
        COMMAND OrbitLightingEmissiveSamplingTests
    )

    add_test(
        NAME Orbit.LightingEmissiveEstimatorIntegration
        COMMAND OrbitLightingEmissiveEstimatorIntegrationTests
    )

    add_test(
        NAME Orbit.LightingEmissiveInvalidation
        COMMAND OrbitLightingEmissiveInvalidationTests
    )

    add_test(
        NAME Orbit.LightingRuntimeMaterialEmission
        COMMAND OrbitLightingRuntimeMaterialEmissionTests
    )

    add_test(
        NAME Orbit.LightingRuntimeEmissiveSurface
        COMMAND OrbitLightingRuntimeEmissiveSurfaceTests
    )

    add_test(
        NAME Orbit.LightingPlanetaryEmissionField
        COMMAND OrbitLightingPlanetaryEmissionFieldTests
    )

    add_test(
        NAME Orbit.LightingPlanetaryEmissionAppearance
        COMMAND OrbitLightingPlanetaryEmissionAppearanceTests
    )

    add_test(
        NAME Orbit.LightingPlanetaryEmissionService
        COMMAND OrbitLightingPlanetaryEmissionServiceTests
    )

    add_test(
        NAME Orbit.LightingPlanetaryEmissionContinuity
        COMMAND OrbitLightingPlanetaryEmissionContinuityTests
    )

    add_test(
        NAME Orbit.LightingReflectionPolicy
        COMMAND OrbitLightingReflectionPolicyTests
    )

    add_test(
        NAME Orbit.LightingRepresentationLightingContinuity
        COMMAND OrbitLightingRepresentationLightingContinuityTests
    )

    add_test(
        NAME Orbit.LightingRuntimeMaterialAssignmentIntegration
        COMMAND OrbitLightingRuntimeMaterialAssignmentIntegrationTests
    )

    add_test(
        NAME Orbit.LightingSurfaceData
        COMMAND OrbitLightingSurfaceDataTests
    )

    add_test(
        NAME Orbit.LightingSurfaceBuffer
        COMMAND OrbitLightingSurfaceBufferTests
    )

    add_test(
        NAME Orbit.LightingLocalLightRegistry
        COMMAND OrbitLightingLocalLightRegistryTests
    )
endif()
