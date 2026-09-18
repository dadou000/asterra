#include <orbit/shader/dxc/DxcShaderCompiler.hpp>

#include "../src/M17CoastalCompute.hpp"

#include <array>
#include <cstdlib>
#include <iostream>

int main()
{
    const orbit::shader::dxc::DxcShaderCompiler compiler;

    constexpr std::array<const char*, 7> shaders{
        orbit::terrain_gpu::detail::kM17InitializeShader,
        orbit::terrain_gpu::detail::kM17AdvanceShader,
        orbit::terrain_gpu::detail::kM17ShorelineShader,
        orbit::terrain_gpu::detail::kM17SedimentExchangeShader,
        orbit::terrain_gpu::detail::kM17SedimentTransportShader,
        orbit::terrain_gpu::detail::kM17SedimentDepositShader,
        orbit::terrain_gpu::detail::kM17SnapshotMaterialShader
    };

    for (std::size_t i = 0U;
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
                << "M17 coastal compute shader "
                << i
                << " produced empty bytecode.\n";

            return EXIT_FAILURE;
        }

        if (binary.stage !=
            orbit::shader::Stage::Compute)
        {
            std::cerr
                << "M17 coastal compute shader "
                << i
                << " returned incorrect stage metadata.\n";

            return EXIT_FAILURE;
        }
    }

    std::cout
        << "Orbit M17 coastal compute shaders compiled successfully.\n";

    return EXIT_SUCCESS;
}
