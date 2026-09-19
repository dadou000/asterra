#include <orbit/shader/dxc/DxcShaderCompiler.hpp>

#include "../src/TerrainSurfaceShader.hpp"

#include <cstdlib>
#include <iostream>

int main()
{
    const orbit::shader::dxc::DxcShaderCompiler compiler;

    const auto binary =
        compiler.Compile({
            .source =
                orbit::terrain_render::detail::
                    kTerrainSurfacePixelShader,
            .entryPoint = "main",
            .stage = orbit::shader::Stage::Pixel,
            .debug = false
        });

    if (binary.bytecode.empty())
    {
        std::cerr
            << "M21 terrain surface pixel shader produced empty bytecode.\n";

        return EXIT_FAILURE;
    }

    if (binary.stage !=
        orbit::shader::Stage::Pixel)
    {
        std::cerr
            << "M21 terrain surface shader returned incorrect stage metadata.\n";

        return EXIT_FAILURE;
    }

    std::cout
        << "Orbit M21 terrain surface pixel shader compiled successfully.\n";

    return EXIT_SUCCESS;
}
