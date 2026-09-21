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
)

add_library(Orbit::Lighting ALIAS OrbitLighting)

target_include_directories(OrbitLighting
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitLighting
    PUBLIC
        Orbit::Core
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
