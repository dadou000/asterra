add_executable(OrbitStudio
    src/Main.cpp
    src/RelaunchStudio.cpp
    src/HotReloadBootstrap.cpp
)

add_dependencies(OrbitStudio
    OrbitPlayer
    OrbitHotReloadProbe
)

# Studio is a windowed application: no console window. Keep the ordinary
# main() entry point so startup code is unchanged.
if(WIN32)
    set_target_properties(OrbitStudio PROPERTIES
        WIN32_EXECUTABLE TRUE)
    target_link_options(OrbitStudio PRIVATE
        /ENTRY:mainCRTStartup
        /MANIFEST:EMBED
        "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/OrbitStudio.manifest")
endif()

target_link_libraries(OrbitStudio
    PRIVATE
        Orbit::Build
        Orbit::Core
        Orbit::Selection
        Orbit::Schema
        Orbit::Scene
        Orbit::Commands
        Orbit::Content
        Orbit::ContentWic
        Orbit::Documents
        Orbit::DevServer
        Orbit::EditorModel
        Orbit::EditorRpc
        Orbit::EditorUi
        Orbit::HotReload
        Orbit::Plugins
        Orbit::Frames
        Orbit::Jobs
        Orbit::PathGeometry
        Orbit::PathRouting
        Orbit::Paths
        Orbit::Platform
        Orbit::PlatformServices
        Orbit::RenderGraph
        Orbit::RenderView
        Orbit::RhiVulkan
        Orbit::Runtime
        Orbit::ShaderDxc
        Orbit::StudioSession
        Orbit::StudioUi
        Orbit::Universe
        Orbit::WorldModel
        Orbit::VolumeFields
        Orbit::VolumeSolver
)

target_compile_features(OrbitStudio PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitStudio)

target_compile_definitions(OrbitStudio
    PRIVATE
        ORBIT_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
        ORBIT_BINARY_ROOT="${CMAKE_BINARY_DIR}"
        ORBIT_BUILD_CONFIG="$<CONFIG>"
        ORBIT_HOT_RELOAD_PROBE_BINARY="$<TARGET_FILE:OrbitHotReloadProbe>"
)

set_target_properties(OrbitStudio PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

add_custom_command(TARGET OrbitStudio POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ORBIT_DXCOMPILER_DLL}"
        "$<TARGET_FILE_DIR:OrbitStudio>"

    # Canonical developer entry point: every successful Studio build publishes
    # the unified application directly at the repository root.
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:OrbitStudio>"
        "${CMAKE_SOURCE_DIR}/Orbit.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:OrbitPlayer>"
        "${CMAKE_SOURCE_DIR}/OrbitPlayer.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ORBIT_DXCOMPILER_DLL}"
        "${CMAKE_SOURCE_DIR}/dxcompiler.dll"
    COMMENT "Publishing unified Orbit Studio to repository root"
)

# Real-device Studio smoke gate. This is intentionally an explicit target
# rather than an unconditional CTest because it creates a native window and
# requires a working Vulkan graphics device.
add_custom_target(OrbitStudioTerrainUiSmoke
    COMMAND $<TARGET_FILE:OrbitStudio> --terrain-ui-smoke
    DEPENDS OrbitStudio
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL
)
