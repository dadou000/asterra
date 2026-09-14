find_package(Vulkan REQUIRED)

add_library(OrbitShaderDxc STATIC
    src/DxcShaderCompiler.cpp
)

add_library(Orbit::ShaderDxc ALIAS OrbitShaderDxc)

target_include_directories(OrbitShaderDxc
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# The Vulkan SDK bundles a SPIR-V-capable DXC (dxcompiler.lib/.dll,
# dxc/dxcapi.h) alongside the core Vulkan headers -- reuse it directly
# instead of fetching a separate DirectXShaderCompiler release.
find_library(ORBIT_DXCOMPILER_LIBRARY
    NAMES dxcompiler
    HINTS "${Vulkan_INCLUDE_DIR}/../Lib"
    REQUIRED
)

find_file(ORBIT_DXCOMPILER_DLL
    NAMES dxcompiler.dll
    HINTS "${Vulkan_INCLUDE_DIR}/../Bin"
    REQUIRED
)

target_link_libraries(OrbitShaderDxc
    PUBLIC
        Orbit::Shader
    PRIVATE
        Vulkan::Vulkan
        ${ORBIT_DXCOMPILER_LIBRARY}
)

target_compile_features(OrbitShaderDxc PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitShaderDxc)
