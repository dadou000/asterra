add_library(OrbitShaderD3D STATIC
    src/D3DShaderCompiler.cpp
)

add_library(Orbit::ShaderD3D ALIAS OrbitShaderD3D)

target_include_directories(OrbitShaderD3D
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitShaderD3D
    PUBLIC
        Orbit::Shader
    PRIVATE
        d3dcompiler
)

target_compile_features(OrbitShaderD3D PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitShaderD3D)
