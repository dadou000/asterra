add_executable(OrbitLauncher WIN32
    src/Main.cpp
)

add_dependencies(OrbitLauncher OrbitSandbox)

target_link_libraries(OrbitLauncher
    PRIVATE
        Orbit::Platform
        shell32
        user32
)

target_compile_features(OrbitLauncher PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitLauncher)

set_target_properties(OrbitLauncher PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
