#include <orbit/shader/dxc/DxcShaderCompiler.hpp>

#include "../src/M13AeolianCompute.hpp"

#include <array>
#include <cstdlib>
#include <iostream>

int main()
{
    const orbit::shader::dxc::DxcShaderCompiler compiler;

    constexpr std::array<const char*, 7> shaders{
        orbit::terrain_gpu::detail::kM13InitializeAirborneShader,
        orbit::terrain_gpu::detail::kM13ExchangeShader,
        orbit::terrain_gpu::detail::kM13ApplyExchangeShader,
        orbit::terrain_gpu::detail::kM13AvalancheProposalShader,
        orbit::terrain_gpu::detail::kM13AvalancheApplyShader,
        orbit::terrain_gpu::detail::kM13SaltationShader,
        orbit::terrain_gpu::detail::kM13SnapshotMaterialShader
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
                << "M13 aeolian compute shader "
                << i
                << " produced empty bytecode.\n";

            return EXIT_FAILURE;
        }

        if (binary.stage !=
            orbit::shader::Stage::Compute)
        {
            std::cerr
                << "M13 aeolian compute shader "
                << i
                << " returned incorrect stage metadata.\n";

            return EXIT_FAILURE;
        }
    }

    std::cout
        << "Orbit M13 aeolian compute shaders compiled successfully.\n";

    return EXIT_SUCCESS;
}
