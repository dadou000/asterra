#include <orbit/shader/dxc/DxcShaderCompiler.hpp>
#include <orbit/terrain_render/SurfaceEffectShader.hpp>

#include "../src/TerrainSurfaceShader.hpp"

#include <cstdlib>
#include <iostream>

int main()
{
    const orbit::shader::dxc::DxcShaderCompiler compiler;

    const auto base =
        compiler.Compile({
            .source =
                orbit::terrain_render::detail::
                    kTerrainSurfacePixelShader,
            .entryPoint = "main",
            .stage = orbit::shader::Stage::Pixel,
            .debug = false
        });

    if (base.bytecode.empty() ||
        base.stage != orbit::shader::Stage::Pixel)
    {
        std::cerr
            << "M21 terrain surface pixel shader compilation failed.\n";
        return EXIT_FAILURE;
    }

    const auto effectSource =
        orbit::terrain_render::BuildSurfaceEffectPixelShader(
            orbit::terrain_render::detail::
                kTerrainSurfacePixelShader);

    const auto effects =
        compiler.Compile({
            .source = effectSource,
            .entryPoint = "main",
            .stage = orbit::shader::Stage::Pixel,
            .debug = false
        });

    if (effects.bytecode.empty() ||
        effects.stage != orbit::shader::Stage::Pixel)
    {
        std::cerr
            << "M38 terrain surface effect pixel shader compilation failed.\n";
        return EXIT_FAILURE;
    }

    std::cout
        << "Orbit terrain base and M38 effect pixel shaders compiled successfully.\n";

    return EXIT_SUCCESS;
}
