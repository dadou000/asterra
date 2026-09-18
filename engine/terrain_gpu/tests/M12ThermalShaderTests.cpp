#include <orbit/shader/dxc/DxcShaderCompiler.hpp>

#include "../src/M12ThermalCompute.hpp"

#include <array>
#include <cstdlib>
#include <iostream>

int main()
{
    const orbit::shader::dxc::DxcShaderCompiler compiler;

    constexpr std::array<const char*, 3> shaders{
        orbit::terrain_gpu::detail::kM12ComputeTransferShader,
        orbit::terrain_gpu::detail::kM12ApplyTransferShader,
        orbit::terrain_gpu::detail::kM12SnapshotMaterialShader
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
                << "M12 thermal compute shader "
                << i
                << " produced empty bytecode.\n";

            return EXIT_FAILURE;
        }

        if (binary.stage !=
            orbit::shader::Stage::Compute)
        {
            std::cerr
                << "M12 thermal compute shader "
                << i
                << " returned incorrect stage metadata.\n";

            return EXIT_FAILURE;
        }
    }

    std::cout
        << "Orbit M12 thermal compute shaders compiled successfully.\n";

    return EXIT_SUCCESS;
}
