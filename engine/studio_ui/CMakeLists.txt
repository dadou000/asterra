add_library(OrbitStudioUi STATIC
    src/ProjectAuthoringUi.cpp
    src/ProjectSettingsUi.cpp
    src/StudioRenderViewSet.cpp
    src/StudioSurfacePicking.cpp
    src/StudioUiBundle.cpp
    src/SurfaceAuthoringUi.cpp
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
        Orbit::EditorModel
        Orbit::EditorUi
        Orbit::RenderGraph
        Orbit::RenderView
        Orbit::StudioSession
        Orbit::TerrainDebug
        Orbit::TerrainGpu
        Orbit::TerrainRender
        Orbit::TerrainView
)

target_compile_features(OrbitStudioUi PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitStudioUi)

if(BUILD_TESTING)
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

    target_compile_features(OrbitStudioViewportNavigationTests PRIVATE cxx_std_23)
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

    target_compile_features(OrbitStudioViewportPresentationTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitStudioViewportPresentationTests)

    add_test(
        NAME Orbit.StudioViewportPresentation
        COMMAND OrbitStudioViewportPresentationTests
    )
endif()
