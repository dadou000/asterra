#include <orbit/shader/dxc/DxcShaderCompiler.hpp>
#include <orbit/terrain_render/SurfaceEffectShader.hpp>
#include <orbit/terrain_render/WaterVolumeShader.hpp>

#include "../src/ClipmapVertexShader.hpp"

#include "../src/TerrainSurfaceShader.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

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

    // The clipmap variants: terrain draws the true bed; water is its own object
    // with a vertex and a pixel shader derived from the terrain vertex shader.
    const std::string vertexSource =
        orbit::terrain_render::detail::kClipmapVertexShader;
    const std::string waterVertexSource =
        orbit::terrain_render::BuildClipmapWaterVertexShader(vertexSource);
    const std::string waterPixelSource =
        orbit::terrain_render::BuildClipmapWaterPixelShader(waterVertexSource);
    const std::string bedSource =
        orbit::terrain_render::BuildSurfaceEffectPixelShader(
            orbit::terrain_render::BuildClipmapBedPixelShader(
                orbit::terrain_render::detail::kTerrainSurfacePixelShader));

    const struct
    {
        const char* name;
        const std::string* source;
        orbit::shader::Stage stage;
    } variants[] = {
        {"terrain vertex", &vertexSource, orbit::shader::Stage::Vertex},
        {"water vertex", &waterVertexSource, orbit::shader::Stage::Vertex},
        {"water pixel", &waterPixelSource, orbit::shader::Stage::Pixel},
        {"bed pixel", &bedSource, orbit::shader::Stage::Pixel}
    };

    for (const auto& variant : variants)
    {
        const auto compiled =
            compiler.Compile({
                .source = *variant.source,
                .entryPoint = "main",
                .stage = variant.stage,
                .debug = false
            });
        if (compiled.bytecode.empty() || compiled.stage != variant.stage)
        {
            std::cerr << "Clipmap " << variant.name
                      << " shader compilation failed.\n";
            return EXIT_FAILURE;
        }
    }

    // The terrain variants must no longer paint water; the water pass owns it.
    if (bedSource.find("smoothstep(0.0, 0.25, input.waterDepth)") !=
            std::string::npos ||
        waterPixelSource.find("g_terrainDepth") == std::string::npos)
    {
        std::cerr << "Clipmap water shaders are not separated.\n";
        return EXIT_FAILURE;
    }

    std::cout
        << "Orbit terrain base, M38 effect, bed and separate water pass shaders compiled successfully.\n";

    return EXIT_SUCCESS;
}
