add_library(OrbitShading STATIC
    src/ShadingContract.cpp
    src/ShaderProgram.cpp
    src/ShadingCapture.cpp
    src/ShadingWorkspace.cpp
    src/ShaderPreviewRenderer.cpp
    src/ShadingRpc.cpp
)

add_library(Orbit::Shading ALIAS OrbitShading)

target_include_directories(OrbitShading
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitShading
    PUBLIC
        Orbit::Content
        Orbit::Core
        Orbit::Math
        Orbit::RHI
        Orbit::Rpc
        Orbit::Shader
)

target_compile_features(OrbitShading PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitShading)

if(BUILD_TESTING)
    add_executable(OrbitShadingTests
        tests/ShadingTests.cpp
    )

    target_link_libraries(OrbitShadingTests
        PRIVATE
            Orbit::Shading
            Orbit::ShaderDxc
    )

    target_compile_features(OrbitShadingTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitShadingTests)
    orbit_copy_dxc_runtime(OrbitShadingTests)

    add_test(NAME Orbit.Shading COMMAND OrbitShadingTests)
endif()
