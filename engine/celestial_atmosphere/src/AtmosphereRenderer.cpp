#include <orbit/celestial_atmosphere/AtmosphereRenderer.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace orbit::celestial_atmosphere
{
namespace
{
constexpr const char* kVertexShader = R"(
struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput main(uint vertexId : SV_VertexID)
{
    const float2 positions[6] =
    {
        float2(-1.0, -1.0),
        float2(-1.0,  1.0),
        float2( 1.0, -1.0),
        float2( 1.0, -1.0),
        float2(-1.0,  1.0),
        float2( 1.0,  1.0)
    };

    const float2 uvs[6] =
    {
        float2(0.0, 1.0),
        float2(0.0, 0.0),
        float2(1.0, 1.0),
        float2(1.0, 1.0),
        float2(0.0, 0.0),
        float2(1.0, 0.0)
    };

    VSOutput output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    output.uv = uvs[vertexId];
    return output;
}
)";

constexpr const char* kPixelShader = R"(
struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

struct Constants
{
    float4 cameraAspect;        // body-frame camera position (m), aspect
    float4 forwardTanHalfFov;   // camera forward, tan(vfov/2)
    float4 upNear;              // camera up, near plane (m)
    float4 sunFar;              // unit sun direction, far plane (m)
    float4 radii;               // bottom, top, Rayleigh H, Mie H (m)
    float4 rayleighMieG;        // Rayleigh scattering (1/m), Mie g
    float4 mieScatterAbsCenter; // Mie scattering (1/m), absorber centre (m)
    float4 mieExtinctAbsHalf;   // Mie extinction (1/m), absorber half width
    float4 absorptionE;         // absorber extinction (1/m), irradiance scale
};

[[vk::push_constant]] Constants g;

[[vk::binding(0, 0)]] [[vk::combinedImageSampler]] Texture2D g_scene;
[[vk::binding(0, 0)]] [[vk::combinedImageSampler]] SamplerState g_sceneSampler;
[[vk::binding(1, 0)]] [[vk::combinedImageSampler]] Texture2D g_depth;
[[vk::binding(1, 0)]] [[vk::combinedImageSampler]] SamplerState g_depthSampler;
[[vk::binding(2, 0)]] [[vk::combinedImageSampler]] Texture2D g_transmittance;
[[vk::binding(2, 0)]] [[vk::combinedImageSampler]] SamplerState g_transmittanceSampler;
[[vk::binding(3, 0)]] [[vk::combinedImageSampler]] Texture2D g_multi;
[[vk::binding(3, 0)]] [[vk::combinedImageSampler]] SamplerState g_multiSampler;

static const uint kSteps = 40u;
static const float kPi = 3.14159265358979;

float3 ViewRay(float2 uv)
{
    const float3 forward = normalize(g.forwardTanHalfFov.xyz);
    const float3 requestedUp = normalize(g.upNear.xyz);
    const float3 right = normalize(cross(forward, requestedUp));
    const float3 up = normalize(cross(right, forward));
    const float tanHalf = max(g.forwardTanHalfFov.w, 0.001);
    const float aspect = max(g.cameraAspect.w, 0.001);
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    return normalize(
        forward +
        right * (ndc.x * aspect * tanHalf) +
        up * (ndc.y * tanHalf));
}

// Numerically stable ray/sphere interval: uses the perpendicular distance
// instead of b*b - c, which cancels badly for a distant orbital observer.
bool RaySphere(float3 origin, float3 direction, float radius, out float t0, out float t1)
{
    const float b = dot(origin, direction);
    const float3 perpendicular = origin - b * direction;
    const float h2 = radius * radius - dot(perpendicular, perpendicular);
    t0 = 0.0;
    t1 = -1.0;
    if (h2 < 0.0)
    {
        return false;
    }
    const float h = sqrt(h2);
    t0 = -b - h;
    t1 = -b + h;
    return true;
}

float ReverseZViewDepth(float depth)
{
    const float nearPlane = max(g.upNear.w, 1.0e-5);
    const float farPlane = max(g.sunFar.w, nearPlane + 1.0e-4);
    return nearPlane * farPlane /
        max(depth * (farPlane - nearPlane) + nearPlane, 1.0e-6);
}

float2 LutUv(float radius, float sunMu)
{
    const float rb = g.radii.x;
    const float rt = g.radii.y;
    const float unit = saturate(
        (radius * radius - rb * rb) / max(rt * rt - rb * rb, 1.0));
    return float2(saturate(sunMu * 0.5 + 0.5), unit);
}

float RayleighPhase(float c)
{
    return 3.0 / (16.0 * kPi) * (1.0 + c * c);
}

float MiePhase(float c, float gAnisotropy)
{
    const float gg = gAnisotropy * gAnisotropy;
    const float d = max(1.0 + gg - 2.0 * gAnisotropy * c, 1.0e-4);
    return (1.0 - gg) / (4.0 * kPi * pow(d, 1.5));
}

float4 main(VSOutput input) : SV_Target0
{
    const int2 pixel = int2(input.position.xy);
    const float4 scene = g_scene.Load(int3(pixel, 0));

    const float3 origin = g.cameraAspect.xyz;
    const float3 direction = ViewRay(input.uv);
    const float bottom = g.radii.x;
    const float top = g.radii.y;

    float topNear;
    float topFar;
    if (!RaySphere(origin, direction, top, topNear, topFar) ||
        topFar <= 0.0)
    {
        return scene;
    }

    const float tStart = max(topNear, 0.0);
    float tEnd = topFar;

    float groundNear;
    float groundFar;
    if (RaySphere(origin, direction, bottom, groundNear, groundFar) &&
        groundFar > 0.0)
    {
        const float ground = groundNear > 0.0 ? groundNear : groundFar;
        if (ground > tStart)
        {
            tEnd = min(tEnd, ground);
        }
    }

    // The rendered surface (terrain relief, macro-globe displacement) can
    // sit above the reference sphere; the depth buffer bounds the segment.
    const float depth = g_depth.Load(int3(pixel, 0)).r;
    if (depth > 0.0)
    {
        const float3 forward = normalize(g.forwardTanHalfFov.xyz);
        const float surfaceT =
            ReverseZViewDepth(depth) / max(dot(direction, forward), 1.0e-5);
        tEnd = min(tEnd, max(surfaceT, tStart));
    }

    if (tEnd <= tStart)
    {
        return scene;
    }

    const float3 sun = normalize(g.sunFar.xyz);
    const float cosine = dot(direction, sun);
    const float rayleighPhase = RayleighPhase(cosine);
    const float miePhase = MiePhase(cosine, g.rayleighMieG.w);

    const float3 rayleighScattering = g.rayleighMieG.xyz;
    const float3 mieScattering = g.mieScatterAbsCenter.xyz;
    const float3 mieExtinction = g.mieExtinctAbsHalf.xyz;
    const float3 absorptionExtinction = g.absorptionE.xyz;
    const float irradiance = max(g.absorptionE.w, 0.0);

    const float step = (tEnd - tStart) / float(kSteps);
    float3 transmittance = 1.0;
    float3 radiance = 0.0;

    [loop]
    for (uint i = 0u; i < kSteps; ++i)
    {
        const float t = tStart + (float(i) + 0.5) * step;
        const float3 position = origin + direction * t;
        const float radius = length(position);
        const float altitude = max(radius - bottom, 0.0);

        const float rayleighDensity = exp(-altitude / max(g.radii.z, 1.0));
        const float mieDensity = exp(-altitude / max(g.radii.w, 1.0));
        const float absorberDensity = saturate(
            1.0 - abs(altitude - g.mieScatterAbsCenter.w) /
                max(g.mieExtinctAbsHalf.w, 1.0));

        const float3 rayleigh = rayleighScattering * rayleighDensity;
        const float3 mie = mieScattering * mieDensity;
        const float3 extinction =
            rayleigh +
            mieExtinction * mieDensity +
            absorptionExtinction * absorberDensity;

        const float sunMu = dot(position / max(radius, 1.0), sun);
        const float2 lut = LutUv(radius, sunMu);
        const float3 sunTransmittance =
            g_transmittance.SampleLevel(g_transmittanceSampler, lut, 0).rgb;
        const float3 multiResponse =
            g_multi.SampleLevel(g_multiSampler, lut, 0).rgb;

        const float3 source =
            irradiance *
            (sunTransmittance * (rayleigh * rayleighPhase + mie * miePhase) +
             multiResponse * (rayleigh + mie) / (4.0 * kPi));

        const float3 stepTransmittance = exp(-extinction * step);
        // Integrate the source exactly across the step for a constant medium.
        const float3 integral =
            (1.0 - stepTransmittance) / max(extinction, 1.0e-12);
        radiance += transmittance * source * integral;
        transmittance *= stepTransmittance;
    }

    return float4(scene.rgb * transmittance + radiance, scene.a);
}
)";

[[nodiscard]] u32 Bits(const f32 value) noexcept
{
    return std::bit_cast<u32>(value);
}
} // namespace

AtmosphereRenderer::AtmosphereRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler)
{
    const auto vertex =
        compiler.Compile({
            .source = kVertexShader,
            .entryPoint = "main",
            .stage = shader::Stage::Vertex,
            .debug = false
        });

    const auto pixel =
        compiler.Compile({
            .source = kPixelShader,
            .entryPoint = "main",
            .stage = shader::Stage::Pixel,
            .debug = false
        });

    pipeline_ =
        device.CreateGraphicsPipeline({
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
            .pushConstantDwords = 36U,
            .shaderResourceBuffers = 0U,
            .sampledTextures = 4U,
            .topology = rhi::PrimitiveTopology::TriangleList,
            .fillMode = rhi::FillMode::Solid,
            .cullMode = rhi::CullMode::None,
            .blendMode = rhi::BlendMode::Opaque,
            .depthTest = false,
            .depthWrite = false,
            .colorAttachmentFormats = {
                rhi::TextureFormat::RGBA16_Float
            },
            .colorAttachmentCount = 1U
        });
}

AtmosphereRenderer::~AtmosphereRenderer() = default;

void AtmosphereRenderer::Draw(
    rhi::CommandList& commands,
    rhi::Texture& sceneColor,
    rhi::Texture& depth,
    GpuAtmosphereLuts& luts,
    rhi::Texture& target,
    const u32 width,
    const u32 height,
    const AtmosphereParameters& p,
    const AtmosphereRenderView& view)
{
    if (width == 0U || height == 0U)
    {
        return;
    }

    const auto f =
        [](const f64 value)
        {
            return Bits(static_cast<f32>(value));
        };

    const std::array<u32, 36> constants{
        f(view.cameraPositionMeters.x),
        f(view.cameraPositionMeters.y),
        f(view.cameraPositionMeters.z),
        Bits(static_cast<f32>(width) / static_cast<f32>(height)),

        Bits(view.forward.x),
        Bits(view.forward.y),
        Bits(view.forward.z),
        Bits(std::tan(view.verticalFovRadians * 0.5F)),

        Bits(view.up.x),
        Bits(view.up.y),
        Bits(view.up.z),
        Bits(std::max(view.nearPlaneMeters, 1.0e-5F)),

        Bits(view.sunDirection.x),
        Bits(view.sunDirection.y),
        Bits(view.sunDirection.z),
        Bits(std::max(view.farPlaneMeters, view.nearPlaneMeters + 1.0e-4F)),

        f(p.bottomRadiusMeters),
        f(p.topRadiusMeters),
        f(p.rayleighScaleHeightMeters),
        f(p.mieScaleHeightMeters),

        f(p.rayleighScatteringPerMeter.x),
        f(p.rayleighScatteringPerMeter.y),
        f(p.rayleighScatteringPerMeter.z),
        f(std::clamp(p.mieAnisotropy, -0.999, 0.999)),

        f(p.mieScatteringPerMeter.x),
        f(p.mieScatteringPerMeter.y),
        f(p.mieScatteringPerMeter.z),
        f(p.absorptionCenterHeightMeters),

        f(p.mieExtinctionPerMeter.x),
        f(p.mieExtinctionPerMeter.y),
        f(p.mieExtinctionPerMeter.z),
        f(p.absorptionHalfWidthMeters),

        f(p.absorptionExtinctionPerMeter.x),
        f(p.absorptionExtinctionPerMeter.y),
        f(p.absorptionExtinctionPerMeter.z),
        Bits(std::max(view.irradianceScale, 0.0F))
    };

    commands.SetRenderTarget(target);
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
    commands.SetGraphicsTexture(0, sceneColor);
    commands.SetGraphicsTexture(1, depth);
    commands.SetGraphicsTexture(2, luts.Transmittance());
    commands.SetGraphicsTexture(3, luts.MultiScattering());
    commands.Draw(6);
}
} // namespace orbit::celestial_atmosphere
