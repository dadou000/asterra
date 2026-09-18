#include <orbit/shader/dxc/DxcShaderCompiler.hpp>

#include "../src/M11HydraulicCompute.hpp"

#include <array>
#include <cstdlib>
#include <iostream>

int main()
{
    const orbit::shader::dxc::DxcShaderCompiler compiler;

    constexpr std::array<const char*, 7> shaders{
        orbit::terrain_gpu::detail::kM11InitializeStateShader,
        orbit::terrain_gpu::detail::kM11RainFluxShader,
        orbit::terrain_gpu::detail::kM11WaterVelocityShader,
        orbit::terrain_gpu::detail::kM11ErodeDepositShader,
        orbit::terrain_gpu::detail::kM11SedimentTransportShader,
        orbit::terrain_gpu::detail::kM11InfiltrationEvaporationShader,
        orbit::terrain_gpu::detail::kM11SnapshotMaterialShader
    };

    for (std::size_t i = 0;
         i < shaders.size();
         ++i)
    {
        const auto binary =
            compiler.Compile({
                .source = shaders[i],
                .entryPoint = "main",
                .stage = orbit::shader::Stage::Compute,
                .debug = false
            });

        if (binary.bytecode.empty())
        {
            std::cerr
                << "M11 hydraulic compute shader "
                << i
                << " produced empty bytecode.\n";
            return EXIT_FAILURE;
        }

        if (binary.stage !=
            orbit::shader::Stage::Compute)
        {
            std::cerr
                << "M11 hydraulic compute shader "
                << i
                << " returned incorrect stage metadata.\n";
            return EXIT_FAILURE;
        }
    }

    std::cout
        << "Orbit M11 hydraulic compute shaders compiled successfully.\n";

    return EXIT_SUCCESS;
}
