# Phase 4 of the storm domain plan: the fast storm core as Vulkan compute (see engine/weather_gpu/docs). The kernels are one HLSL file
# that is embedded into the library at configure time and can be reloaded at run time (GpuStormSolver::ReloadShaders).
set(ORBIT_STORM_SHADER "${CMAKE_CURRENT_SOURCE_DIR}/shaders/storm.hlsl")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ORBIT_STORM_SHADER}")
file(READ "${ORBIT_STORM_SHADER}" ORBIT_STORM_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," ORBIT_STORM_BYTES "${ORBIT_STORM_HEX}")
set(ORBIT_STORM_EMBED "${CMAKE_CURRENT_BINARY_DIR}/EmbeddedStormShader.cpp")
file(WRITE "${ORBIT_STORM_EMBED}.tmp"
"#include <orbit/weather_gpu/GpuStormSolver.hpp>
namespace orbit::weather_gpu
{
namespace
{
const unsigned char kStormShaderBytes[] = {${ORBIT_STORM_BYTES}0x00};
} // namespace
std::string_view EmbeddedStormShader()
{
    return {reinterpret_cast<const char*>(kStormShaderBytes), sizeof(kStormShaderBytes) - 1U};
}
} // namespace orbit::weather_gpu
")
file(COPY_FILE "${ORBIT_STORM_EMBED}.tmp" "${ORBIT_STORM_EMBED}" ONLY_IF_DIFFERENT)

add_library(OrbitWeatherGpu STATIC
    src/GpuStormSolver.cpp
    "${ORBIT_STORM_EMBED}"
)
add_library(Orbit::WeatherGpu ALIAS OrbitWeatherGpu)

target_include_directories(OrbitWeatherGpu
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitWeatherGpu
    PUBLIC
        Orbit::WeatherLab
        Orbit::RHI
        Orbit::Shader
        Orbit::Core
)

target_compile_features(OrbitWeatherGpu PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitWeatherGpu)

add_executable(orbit_weather_gpu tools/WeatherGpuCli.cpp)
target_link_libraries(orbit_weather_gpu PRIVATE Orbit::WeatherGpu Orbit::RhiVulkan Orbit::ShaderDxc)
target_compile_features(orbit_weather_gpu PRIVATE cxx_std_23)
orbit_enable_warnings(orbit_weather_gpu)
orbit_copy_dxc_runtime(orbit_weather_gpu)

if(BUILD_TESTING)
    add_executable(OrbitWeatherGpuTests tests/WeatherGpuTests.cpp)
    target_link_libraries(OrbitWeatherGpuTests PRIVATE Orbit::WeatherGpu Orbit::RhiVulkan Orbit::ShaderDxc)
    target_compile_features(OrbitWeatherGpuTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitWeatherGpuTests)
    orbit_copy_dxc_runtime(OrbitWeatherGpuTests)
    add_test(NAME Orbit.WeatherGpu COMMAND OrbitWeatherGpuTests)
endif()
