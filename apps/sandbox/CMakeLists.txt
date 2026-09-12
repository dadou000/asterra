add_executable(OrbitSandbox
    src/Main.cpp
)

target_link_libraries(OrbitSandbox
    PRIVATE
        Orbit::Camera
        Orbit::Core
        Orbit::DebugRender
        Orbit::Jobs
        Orbit::Math
        Orbit::Platform
        Orbit::RhiD3D12
        Orbit::ShaderD3D
        Orbit::Terrain
        Orbit::TerrainCache
        Orbit::TerrainRegion
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
