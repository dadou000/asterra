#include <orbit/shader/dxc/DxcShaderCompiler.hpp>

#include "../src/M22ScatterCompute.hpp"

#include <cstdlib>
#include <iostream>

int main()
{
    const orbit::shader::dxc::DxcShaderCompiler compiler;

    const auto binary =
        compiler.Compile({
            .source =
                orbit::terrain_gpu::detail::
                    kM22BiomeScatterShader,
            .entryPoint = "main",
            .stage = orbit::shader::Stage::Compute,
            .debug = false
        });

    if (binary.bytecode.empty())
    {
        std::cerr
            << "M22 biome scatter compute shader produced empty bytecode.\n";

        return EXIT_FAILURE;
    }

    if (binary.stage !=
        orbit::shader::Stage::Compute)
    {
        std::cerr
            << "M22 biome scatter shader returned incorrect stage metadata.\n";

        return EXIT_FAILURE;
    }

    std::cout
        << "Orbit M22 biome scatter compute shader compiled successfully.\n";

    return EXIT_SUCCESS;
}
