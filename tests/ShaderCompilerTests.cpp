#include <orbit/shader/d3d/D3DShaderCompiler.hpp>

#include <iostream>

int main()
{
    const orbit::shader::d3d::D3DShaderCompiler compiler;

    constexpr const char* vertexSource = R"(
struct VSInput
{
    float3 position : TEXCOORD0;
};

struct VSOutput
{
    float4 position : SV_Position;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    output.position = float4(input.position, 1.0);
    return output;
}
)";

    constexpr const char* pixelSource = R"(
float4 main() : SV_Target0
{
    return float4(0.2, 0.6, 0.9, 1.0);
}
)";

    const auto vertex = compiler.Compile({
        .source = vertexSource,
        .entryPoint = "main",
        .stage = orbit::shader::Stage::Vertex,
        .debug = false
    });

    const auto pixel = compiler.Compile({
        .source = pixelSource,
        .entryPoint = "main",
        .stage = orbit::shader::Stage::Pixel,
        .debug = false
    });

    if (vertex.bytecode.empty() ||
        pixel.bytecode.empty())
    {
        std::cerr << "Compiled Orbit shader bytecode is empty.\n";
        return 1;
    }

    if (vertex.stage != orbit::shader::Stage::Vertex ||
        pixel.stage != orbit::shader::Stage::Pixel)
    {
        std::cerr << "Orbit shader stage metadata is wrong.\n";
        return 1;
    }

    return 0;
}
