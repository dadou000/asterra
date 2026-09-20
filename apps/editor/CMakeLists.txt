add_executable(OrbitStudio
    src/Main.cpp
)

add_dependencies(OrbitStudio OrbitPlayer)

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
)

target_compile_features(OrbitStudio PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitStudio)

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
