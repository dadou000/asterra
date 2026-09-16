add_executable(OrbitBuild
    src/Main.cpp
)

target_link_libraries(OrbitBuild
    PRIVATE
        Orbit::Build
        Orbit::Platform
)

target_compile_features(OrbitBuild PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitBuild)

set_target_properties(OrbitBuild PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
