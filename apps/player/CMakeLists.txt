add_executable(OrbitPlayer
    src/Main.cpp
)

target_link_libraries(OrbitPlayer
    PRIVATE
        Orbit::Core
        Orbit::Documents
        Orbit::Platform
        Orbit::RHI
        Orbit::Runtime
        Orbit::RuntimeProject
        Orbit::Scene
)

target_compile_features(OrbitPlayer PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitPlayer)

set_target_properties(OrbitPlayer PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
