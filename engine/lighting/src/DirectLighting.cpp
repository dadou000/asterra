#include <orbit/lighting/DirectLighting.hpp>

#include <algorithm>
#include <array>
#include <bit>

namespace orbit::lighting
{
namespace
{
constexpr const char* kVs = R"(
struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput main(uint vertexId : SV_VertexID)
{
    const float2 p[6] =
    {
        float2(-1,-1),
        float2(-1, 1),
        float2( 1,-1),
        float2( 1,-1),
        float2(-1, 1),
        float2( 1, 1)
    };

    const float2 uv[6] =
    {
        float2(0,1),
        float2(0,0),
        float2(1,1),
        float2(1,1),
        float2(0,0),
        float2(1,0)
    };

    VSOutput o;
    o.position = float4(p[vertexId], 0, 1);
    o.uv = uv[vertexId];
    return o;
}
)";

constexpr const char* kPs = R"(
[[vk::binding(0, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_baseRoughness;
[[vk::binding(0, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_baseSampler;

[[vk::binding(1, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_normalMetallic;
[[vk::binding(1, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_normalSampler;

[[vk::binding(2, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_emissionClass;
[[vk::binding(2, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_emissionSampler;

struct Constants
{
    float4 lightDirectionAndScale;
    float4 lightColorAndAmbient;
};
[[vk::push_constant]] Constants g;

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float3 FresnelSchlick(float cosTheta, float3 f0)
{
    const float f =
        pow(1.0 - saturate(cosTheta), 5.0);
    return f0 + (1.0 - f0) * f;
}

float DistributionGGX(float nDotH, float roughness)
{
    const float a =
        max(roughness * roughness, 0.0025);
    const float a2 = a * a;
    const float d =
        nDotH * nDotH * (a2 - 1.0) + 1.0;
    return a2 /
        max(3.14159265 * d * d, 1.0e-5);
}

float GeometrySchlickGGX(float nDotV, float roughness)
{
    const float r = roughness + 1.0;
    const float k = (r * r) / 8.0;
    return nDotV /
        max(nDotV * (1.0 - k) + k, 1.0e-5);
}

float4 main(VSOutput input) : SV_Target0
{
    const float4 baseRoughness =
        g_baseRoughness.Sample(
            g_baseSampler,
            input.uv);
    const float4 normalMetallic =
        g_normalMetallic.Sample(
            g_normalSampler,
            input.uv);
    const float4 emissionClass =
        g_emissionClass.Sample(
            g_emissionSampler,
            input.uv);

    // Metadata 0 means no physical surface was written at this pixel.
    if (emissionClass.a <= 0.0)
    {
        return float4(0.006, 0.010, 0.018, 1.0);
    }

    const float3 baseColor =
        max(baseRoughness.rgb, 0.0);
    const float roughness =
        saturate(baseRoughness.a);
    const float metallic =
        saturate(normalMetallic.a);
    const float3 n =
        normalize(normalMetallic.xyz);

    const float3 l =
        normalize(g.lightDirectionAndScale.xyz);
    const float irradiance =
        max(g.lightDirectionAndScale.w, 0.0);
    const float3 lightColor =
        max(g.lightColorAndAmbient.rgb, 0.0);
    const float ambient =
        max(g.lightColorAndAmbient.w, 0.0);

    // Until M04 depth reconstruction is consumed here, use the directional
    // light as the half-vector reference. This still yields one shared BRDF
    // and removes renderer-local suns; M05 local-light/view terms refine it.
    const float3 v =
        normalize(float3(0.0, 0.0, 1.0));
    const float3 h =
        normalize(l + v);

    const float nDotL =
        saturate(dot(n, l));
    const float nDotV =
        saturate(dot(n, v));
    const float nDotH =
        saturate(dot(n, h));
    const float vDotH =
        saturate(dot(v, h));

    const float3 dielectricF0 =
        float3(0.04, 0.04, 0.04);
    const float3 f0 =
        lerp(
            dielectricF0,
            baseColor,
            metallic);

    const float3 f =
        FresnelSchlick(vDotH, f0);
    const float d =
        DistributionGGX(nDotH, roughness);
    const float gVis =
        GeometrySchlickGGX(nDotV, roughness) *
        GeometrySchlickGGX(nDotL, roughness);

    const float3 specular =
        (d * gVis * f) /
        max(4.0 * nDotV * nDotL, 1.0e-4);

    const float3 kd =
        (1.0 - f) *
        (1.0 - metallic);

    const float3 diffuse =
        kd * baseColor / 3.14159265;

    const float3 direct =
        (diffuse + specular) *
        lightColor *
        irradiance *
        nDotL;

    const float3 ambientTerm =
        baseColor *
        (1.0 - metallic) *
        ambient;

    const float3 sceneLinear =
        direct +
        ambientTerm +
        max(emissionClass.rgb, 0.0);

    return float4(sceneLinear, 1.0);
}
)";
} // namespace

DirectLightingRenderer::DirectLightingRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler)
{
    const auto vs =
        compiler.Compile({
            .source = kVs,
            .entryPoint = "main",
            .stage = shader::Stage::Vertex,
            .debug = false
        });

    const auto ps =
        compiler.Compile({
            .source = kPs,
            .entryPoint = "main",
            .stage = shader::Stage::Pixel,
            .debug = false
        });

    pipeline_ =
        device.CreateGraphicsPipeline({
            .vertexShader = {
                .data = vs.bytecode.data(),
                .size = vs.bytecode.size()
            },
            .pixelShader = {
                .data = ps.bytecode.data(),
                .size = ps.bytecode.size()
            },
            .vertexAttributes = {},
            .vertexStrideBytes = 0U,
            .pushConstantDwords = 8U,
            .sampledTextures = 3U,
            .topology =
                rhi::PrimitiveTopology::TriangleList,
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

void DirectLightingRenderer::Draw(
    rhi::CommandList& commands,
    rhi::Texture& surfaceBaseRoughness,
    rhi::Texture& surfaceNormalMetallic,
    rhi::Texture& surfaceEmissionClass,
    rhi::Texture& targetSceneColor,
    const u32 width,
    const u32 height,
    const DirectionalLight& light,
    const DirectLightingSettings& settings)
{
    if (width == 0U || height == 0U)
    {
        return;
    }

    const auto bits =
        [](const f32 value)
        {
            return std::bit_cast<u32>(value);
        };

    const std::array<u32, 8> constants{
        bits(light.directionToLight.x),
        bits(light.directionToLight.y),
        bits(light.directionToLight.z),
        bits(std::max(
            light.irradianceScale,
            0.0F)),

        bits(std::max(light.colorLinear.x, 0.0F)),
        bits(std::max(light.colorLinear.y, 0.0F)),
        bits(std::max(light.colorLinear.z, 0.0F)),
        bits(std::max(
            settings.ambientIrradianceScale,
            0.0F))
    };

    commands.SetRenderTarget(targetSceneColor);
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
    commands.SetGraphicsTexture(
        0U,
        surfaceBaseRoughness);
    commands.SetGraphicsTexture(
        1U,
        surfaceNormalMetallic);
    commands.SetGraphicsTexture(
        2U,
        surfaceEmissionClass);
    commands.Draw(6U);
}
} // namespace orbit::lighting
