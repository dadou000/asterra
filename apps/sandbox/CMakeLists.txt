add_executable(OrbitSandbox
    src/Main.cpp
)

target_link_libraries(OrbitSandbox
    PRIVATE
        Orbit::Core
        Orbit::Platform
)

target_compile_features(OrbitSandbox PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitSandbox)

set_target_properties(OrbitSandbox PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
