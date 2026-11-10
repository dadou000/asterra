#include <iostream>
#include <orbit/lighting/HybridReflectionRenderer.hpp>
#include <orbit/mesh_render/GlassSurface.hpp>
#include <orbit/shader/dxc/DxcShaderCompiler.hpp>
#include <stdexcept>

int main()
{
    using namespace orbit;
    try
    {
        shader::dxc::DxcShaderCompiler compiler;
        const auto compile = [&](const std::string &source, shader::Stage stage,
                                 bool hardware = false) {
            const auto result = compiler.Compile({.source = source,
                                                  .entryPoint = "main",
                                                  .stage = stage,
                                                  .shaderModelMajor = 6U,
                                                  .shaderModelMinor = hardware ? 5U : 0U,
                                                  .enableSpirvRayQuery = hardware,
                                                  .debug = false});
            if (result.bytecode.empty())
                throw std::runtime_error("Empty reflection shader");
        };
        compile(lighting::BuildHybridReflectionShaderSource(), shader::Stage::Compute);
        compile(lighting::BuildHybridReflectionShaderSource(true), shader::Stage::Compute, true);
        compile(lighting::BuildReflectionCompositeShaderSource(), shader::Stage::Compute);
        compile(mesh_render::BuildGlassPixelShaderSource(), shader::Stage::Pixel);
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
    return 0;
}
