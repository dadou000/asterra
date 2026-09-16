add_executable(OrbitStudio
    src/Main.cpp
)

target_link_libraries(OrbitStudio
    PRIVATE
        Orbit::Build
        Orbit::Core
        Orbit::Selection
        Orbit::Schema
        Orbit::Scene
        Orbit::Commands
        Orbit::Content
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
        Orbit::RenderGraph
        Orbit::RenderView
        Orbit::RhiVulkan
        Orbit::Runtime
        Orbit::ShaderDxc
        Orbit::Universe
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
)
