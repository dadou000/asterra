#include <orbit/volume_render/VolumeParticleRenderer.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace orbit::volume_render
{
namespace
{
constexpr const char* kVertexShader = R"(
struct Particle
{
    float3 positionMeters;
    float authority;
    float3 velocityMetersPerSecond;
    float density;
    float emission;
    float ageSeconds;
    float lifetimeSeconds;
    float linearDragPerSecond;
    float radiusMeters;
    float emissionScale;
    float gravityScale;
    float restitution;
    float3 baseColor;
    uint behaviorFlags;
    float3 emissionColor;
    uint generation;
};

[[vk::binding(2, 0)]]
StructuredBuffer<Particle> g_particles : register(t2);

struct Push
{
    float4 projection;
    float4 forward;
    float4 up;
    float4 camera;
    float4 viewport;
};

[[vk::push_constant]]
Push g;

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float authority : TEXCOORD1;
    float density : TEXCOORD2;
    float emission : TEXCOORD3;
    float life : TEXCOORD4;
    float3 baseColor : TEXCOORD5;
    float3 emissionColor : TEXCOORD6;
    float emissionScale : TEXCOORD7;
};

float4 Project(float3 relative)
{
    const float3 forward = normalize(g.forward.xyz);
    const float3 requestedUp = normalize(g.up.xyz);
    const float3 right = normalize(cross(forward, requestedUp));
    const float3 cameraUp = normalize(cross(right, forward));

    const float z = dot(relative, forward);
    if (z <= g.projection.z || z >= g.projection.w)
    {
        return float4(2.0, 2.0, 1.0, 1.0);
    }

    const float x = dot(relative, right);
    const float y = dot(relative, cameraUp);

    return float4(
        x / (max(g.projection.x, 0.001) * max(g.projection.y, 0.001)),
        -y / max(g.projection.y, 0.001),
        z * 0.5,
        z);
}

VSOutput main(uint vertexId : SV_VertexID)
{
    static const float2 corners[6] =
    {
        float2(-1.0, -1.0),
        float2( 1.0, -1.0),
        float2( 1.0,  1.0),
        float2(-1.0, -1.0),
        float2( 1.0,  1.0),
        float2(-1.0,  1.0)
    };

    const uint particleIndex = vertexId / 6u;
    const uint cornerIndex = vertexId % 6u;
    const Particle particle = g_particles[particleIndex];
    const uint currentGeneration = asuint(g.viewport.w);

    VSOutput output;
    if (particle.generation != currentGeneration ||
        particle.lifetimeSeconds <= 0.0 ||
        particle.ageSeconds >= particle.lifetimeSeconds)
    {
        output.position = float4(2.0, 2.0, 1.0, 1.0);
        output.uv = 0.0;
        output.authority = 0.0;
        output.density = 0.0;
        output.emission = 0.0;
        output.life = 0.0;
        output.baseColor = 0.0;
        output.emissionColor = 0.0;
        output.emissionScale = 0.0;
        return output;
    }

    const float4 center = Project(
        particle.positionMeters - g.camera.xyz);

    if (center.w <= 0.0)
    {
        output.position = center;
        output.uv = 0.0;
        output.authority = 0.0;
        output.density = 0.0;
        output.emission = 0.0;
        output.life = 0.0;
        output.baseColor = 0.0;
        output.emissionColor = 0.0;
        output.emissionScale = 0.0;
        return output;
    }

    const float2 ndcPerPixel = float2(
        2.0 / max(g.viewport.x, 1.0),
        2.0 / max(g.viewport.y, 1.0));
    const float2 corner = corners[cornerIndex];
    const float projectedRadiusPixels =
        max(particle.radiusMeters, 0.001) /
        max(center.w * max(g.projection.y, 0.001), 0.001) *
        max(g.viewport.y, 1.0) * 0.5;
    const float radiusPixels = max(projectedRadiusPixels, max(g.viewport.z, 0.5));

    output.position = center;
    output.position.xy += corner * ndcPerPixel * radiusPixels * center.w;
    output.uv = corner;
    output.authority = max(particle.authority, 0.0);
    output.density = max(particle.density, 0.0);
    output.emission = max(particle.emission, 0.0);
    output.life = saturate(
        1.0 - particle.ageSeconds /
            max(particle.lifetimeSeconds, 0.001));
    output.baseColor = max(particle.baseColor, 0.0);
    output.emissionColor = max(particle.emissionColor, 0.0);
    output.emissionScale = max(particle.emissionScale, 0.0);
    return output;
}
)";

constexpr const char* kPixelShader = R"(
struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float authority : TEXCOORD1;
    float density : TEXCOORD2;
    float emission : TEXCOORD3;
    float life : TEXCOORD4;
    float3 baseColor : TEXCOORD5;
    float3 emissionColor : TEXCOORD6;
    float emissionScale : TEXCOORD7;
};

float4 main(VSOutput input) : SV_Target0
{
    const float radius2 = dot(input.uv, input.uv);
    if (radius2 >= 1.0 || input.life <= 0.0)
    {
        discard;
    }

    const float soft = 1.0 - smoothstep(0.30, 1.0, radius2);
    const float density = saturate(input.density);
    const float emission = max(input.emission, 0.0);
    const float authority = saturate(input.authority);

    const float3 densityColor = input.baseColor * lerp(0.72, 1.0, density);
    const float3 emissiveColor = input.emissionColor * emission * input.emissionScale;
    const float3 color = densityColor + emissiveColor;
    const float alpha = soft * input.life *
        saturate(0.16 + 0.64 * authority + 0.20 * density);

    return float4(color, alpha);
}
)";
}

VolumeParticleRenderer::VolumeParticleRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler,
    const u32 framesInFlight)
    : state_(device, compiler, framesInFlight)
{
    const auto vertex = compiler.Compile({
        .source = kVertexShader,
        .entryPoint = "main",
        .stage = shader::Stage::Vertex,
        .debug = false
    });
    const auto pixel = compiler.Compile({
        .source = kPixelShader,
        .entryPoint = "main",
        .stage = shader::Stage::Pixel,
        .debug = false
    });

    if (vertex.bytecode.empty() || pixel.bytecode.empty())
    {
        throw std::runtime_error(
            "Orbit failed to compile M38 persistent particle shaders.");
    }

    pipeline_ = device.CreateGraphicsPipeline({
        .vertexShader = {
            .data = vertex.bytecode.data(),
            .size = vertex.bytecode.size()
        },
        .pixelShader = {
            .data = pixel.bytecode.data(),
            .size = pixel.bytecode.size()
        },
        .vertexAttributes = {},
        .vertexStrideBytes = 0U,
        .pushConstantDwords = 20U,
        .shaderResourceBuffers = 3U,
        .sampledTextures = 0U,
        .topology = rhi::PrimitiveTopology::TriangleList,
        .fillMode = rhi::FillMode::Solid,
        .cullMode = rhi::CullMode::None,
        .blendMode = rhi::BlendMode::Alpha,
        .depthCompare = rhi::DepthCompare::LessEqual,
        .depthTest = true,
        .depthWrite = false,
        .colorAttachmentFormats = {
            rhi::TextureFormat::RGBA16_Float
        },
        .colorAttachmentCount = 1U
    });
}

void VolumeParticleRenderer::SetSpawns(
    const std::span<const VolumeParticleGpuSpawn> spawns)
{
    state_.SetSpawns(spawns);
}

void VolumeParticleRenderer::Advance(
    rhi::CommandList& commands,
    const u32 frameIndex,
    const f64 deltaSeconds,
    const math::Double3 previousOriginMeters,
    const math::Double3 newOriginMeters,
    const VolumeParticleSimulationSettings settings)
{
    state_.Advance(
        commands,
        frameIndex,
        deltaSeconds,
        previousOriginMeters,
        newOriginMeters,
        settings);
}

void VolumeParticleRenderer::Draw(
    rhi::CommandList& commands,
    rhi::Texture& sceneColor,
    rhi::Texture& depth,
    const u32 width,
    const u32 height,
    const render_view::CameraState& camera,
    const math::Double3 cameraPositionRelativeToPresentationOriginMeters,
    const f32 radiusPixels)
{
    if (state_.Generation() == 0U || width == 0U || height == 0U)
    {
        return;
    }

    const f32 tanHalfFov = std::tan(camera.verticalFovRadians * 0.5F);
    const auto bits = [](const f32 value)
    {
        return std::bit_cast<u32>(value);
    };

    std::array<u32, 20> constants{};
    constants[0] = bits(
        static_cast<f32>(width) /
        static_cast<f32>(height));
    constants[1] = bits(tanHalfFov);
    constants[2] = bits(camera.nearPlaneMeters);
    constants[3] = bits(camera.farPlaneMeters);

    constants[4] = bits(camera.forward.x);
    constants[5] = bits(camera.forward.y);
    constants[6] = bits(camera.forward.z);

    constants[8] = bits(camera.up.x);
    constants[9] = bits(camera.up.y);
    constants[10] = bits(camera.up.z);

    constants[12] = bits(static_cast<f32>(
        cameraPositionRelativeToPresentationOriginMeters.x));
    constants[13] = bits(static_cast<f32>(
        cameraPositionRelativeToPresentationOriginMeters.y));
    constants[14] = bits(static_cast<f32>(
        cameraPositionRelativeToPresentationOriginMeters.z));

    constants[16] = bits(static_cast<f32>(width));
    constants[17] = bits(static_cast<f32>(height));
    constants[18] = bits(
        std::isfinite(radiusPixels)
            ? std::max(radiusPixels, 0.5F)
            : 3.0F);
    constants[19] = state_.Generation();

    commands.SetRenderTargets(sceneColor, depth);
    commands.SetViewport({
        .x = 0.0F,
        .y = 0.0F,
        .width = static_cast<f32>(width),
        .height = static_cast<f32>(height),
        .minDepth = 0.0F,
        .maxDepth = 1.0F
    });
    commands.SetScissor({
        .left = 0,
        .top = 0,
        .right = static_cast<i32>(width),
        .bottom = static_cast<i32>(height)
    });
    commands.SetGraphicsPipeline(*pipeline_);
    commands.SetGraphicsConstants(constants);
    state_.BindForGraphics(commands);
    commands.Draw(VolumeParticleGpuState::MaximumParticleCount * 6U);
}

void VolumeParticleRenderer::Reset() noexcept
{
    state_.Reset();
}

u32 VolumeParticleRenderer::Generation() const noexcept
{
    return state_.Generation();
}

u32 VolumeParticleRenderer::SubmittedSpawnCount() const noexcept
{
    return state_.SubmittedSpawnCount();
}
} // namespace orbit::volume_render
