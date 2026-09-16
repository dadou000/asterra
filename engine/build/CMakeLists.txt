add_library(OrbitBuildService STATIC
    src/BuildService.cpp
)

add_library(Orbit::Build ALIAS OrbitBuildService)

target_include_directories(OrbitBuildService
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitBuildService
    PUBLIC
        Orbit::Content
        Orbit::Core
        Orbit::Documents
    PRIVATE
        Luau.Compiler
        Luau.VM
        Orbit::PluginManifest
        Orbit::ShaderDxc
        tomlplusplus::tomlplusplus
)

target_compile_features(OrbitBuildService PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitBuildService)

if(MSVC)
    # Luau public headers expose intentionally unused visitor parameters.
    target_compile_options(OrbitBuildService PRIVATE /wd4100)
endif()

if(BUILD_TESTING)
    add_executable(OrbitBuildServiceTests
        tests/BuildServiceTests.cpp
    )

    target_link_libraries(OrbitBuildServiceTests
        PRIVATE
            Orbit::Build
    )

    target_compile_features(OrbitBuildServiceTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitBuildServiceTests)

    orbit_copy_dxc_runtime(
        OrbitBuildServiceTests
    )

    add_test(
        NAME Orbit.BuildService
        COMMAND OrbitBuildServiceTests
    )
endif()
