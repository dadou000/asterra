add_executable(OrbitBuild
    src/Main.cpp
)

add_dependencies(OrbitBuild OrbitPlayer)

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


orbit_copy_dxc_runtime(OrbitBuild)


if(BUILD_TESTING)
    add_executable(OrbitBuildCliTests
        tests/OrbitBuildCliTests.cpp
    )

    target_link_libraries(OrbitBuildCliTests
        PRIVATE
            Orbit::Documents
    )

    target_compile_features(OrbitBuildCliTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitBuildCliTests)

    add_dependencies(
        OrbitBuildCliTests
        OrbitBuild
        OrbitPlayer
    )

    add_test(
        NAME Orbit.BuildCli
        COMMAND OrbitBuildCliTests
            "$<TARGET_FILE:OrbitBuild>"
    )
endif()
