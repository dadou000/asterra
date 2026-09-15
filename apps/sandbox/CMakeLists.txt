add_executable(OrbitSandbox
    src/Main.cpp
)

target_link_libraries(OrbitSandbox
    PRIVATE
        Orbit::Camera
        Orbit::Core
        Orbit::DebugRender
        Orbit::DevServer
        Orbit::Jobs
        Orbit::MapRender
        Orbit::Math
        Orbit::Platform
        Orbit::RhiVulkan
        Orbit::Runtime
        Orbit::ShaderDxc
        Orbit::Terrain
        Orbit::TerrainCache
        Orbit::TerrainRegion
        Orbit::WaterRender
        Orbit::TerrainErosion
        Orbit::TerrainHydrology
        Orbit::TerrainRender
        Orbit::TerrainStream
        Orbit::World
)

target_compile_features(OrbitSandbox PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitSandbox)

set_target_properties(OrbitSandbox PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

# dxcompiler.dll (the SPIR-V-capable DXC the shader compiler links
# against, see engine/shader/dxc) is loaded at runtime, not just link
# time -- it has to sit next to the executable.
add_custom_command(TARGET OrbitSandbox POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ORBIT_DXCOMPILER_DLL}"
        "$<TARGET_FILE_DIR:OrbitSandbox>"
)
