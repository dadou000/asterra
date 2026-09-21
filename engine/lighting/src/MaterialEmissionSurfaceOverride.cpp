#include <orbit/lighting/MaterialEmissionSurfaceOverride.hpp>

#include <algorithm>
#include <array>
#include <bit>

namespace orbit::lighting
{
namespace
{
constexpr const char* kCs = R"(
[[vk::binding(0, 0)]]
RWTexture2D<float4> g_surfaceEmissionClass : register(u0);

struct Constants
{
    uint width;
    uint height;
    float emissionX;
    float emissionY;
    float emissionZ;
    uint reserved0;
    uint reserved1;
    uint reserved2;
};

[[vk::push_constant]]
Constants g;

[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
    if (dispatchId.x >= g.width ||
        dispatchId.y >= g.height)
    {
        return;
    }

    const uint2 pixel = dispatchId.xy;
    float4 value = g_surfaceEmissionClass[pixel];

    if (value.a <= 0.0)
    {
        return;
    }

    value.rgb += max(
        float3(
            g.emissionX,
            g.emissionY,
            g.emissionZ),
        0.0);

    g_surfaceEmissionClass[pixel] = value;
}
)";
} // namespace

MaterialEmissionSurfaceOverrideRenderer::
MaterialEmissionSurfaceOverrideRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler)
{
    const auto shader =
        compiler.Compile({
            .source = kCs,
            .entryPoint = "main",
            .stage = shader::Stage::Compute,
            .debug = false
        });

    pipeline_ =
        device.CreateComputePipeline({
            .computeShader = {
                .data = shader.bytecode.data(),
                .size = shader.bytecode.size()
            },
            .pushConstantDwords = 8U,
            .shaderResourceBuffers = 0U,
            .storageTextures = 1U,
            .sampledTextures = 0U
        });
}

void MaterialEmissionSurfaceOverrideRenderer::Apply(
    rhi::CommandList& commands,
    rhi::Texture& surfaceEmissionClass,
    const u32 width,
    const u32 height,
    const math::Float3 emissionRadiance)
{
    if (width == 0U ||
        height == 0U)
    {
        return;
    }

    const auto bits =
        [](const f32 value)
        {
            return std::bit_cast<u32>(value);
        };

    const std::array<u32, 8> constants{
        width,
        height,
        bits(std::max(emissionRadiance.x, 0.0F)),
        bits(std::max(emissionRadiance.y, 0.0F)),
        bits(std::max(emissionRadiance.z, 0.0F)),
        0U,
        0U,
        0U
    };

    commands.SetComputePipeline(*pipeline_);
    commands.SetComputeConstants(constants);
    commands.SetComputeStorageTexture(
        0U,
        surfaceEmissionClass);
    commands.Dispatch(
        (width + 7U) / 8U,
        (height + 7U) / 8U,
        1U);
}
} // namespace orbit::lighting
