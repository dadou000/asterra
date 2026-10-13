#include <orbit/studio_ui/SunAuditViewportIndicator.hpp>

#include <orbit/lighting/SunVisibilityShader.hpp>
#include <orbit/lighting/TerrainSunShadow.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <string>

namespace orbit::studio_ui
{
namespace
{
constexpr const char* kVs = R"(
struct VSOutput { float4 position : SV_Position; };
VSOutput main(uint id : SV_VertexID)
{
    float2 p[6] = {
        float2(-1,-1),float2(-1,1),float2(1,-1),
        float2(1,-1),float2(-1,1),float2(1,1)
    };
    VSOutput output;
    output.position = float4(p[id],0,1);
    return output;
}
)";
constexpr const char* kWitnessPs = R"(
struct Constants { float4 display; };
[[vk::push_constant]] Constants g;
float4 main(float4 position : SV_Position) : SV_Target0
{
    if (position.x >= min(g.display.z, 180.0) ||
        position.y >= min(g.display.w, 18.0)) discard;
    float3 c = g.display.x < 1.5 ? float3(1.0,0.16,0.07) :
        (g.display.x < 2.5 ? float3(0.06,0.9,0.22) :
         float3(0.12,0.38,1.0));
    if (position.y >= 12.0) c = g.display.y > 0.5
        ? float3(1.0,1.0,1.0) : c * 0.18;
    return float4(c, 1.0);
}
)";

constexpr const char* kDiagnosticPs = R"(
[[vk::binding(0, 0)]] ByteAddressBuffer g_terrain;
[[vk::binding(1, 0)]] [[vk::combinedImageSampler]] Texture2D g_scene;
[[vk::binding(1, 0)]] [[vk::combinedImageSampler]] SamplerState g_sceneSampler;
[[vk::binding(2, 0)]] [[vk::combinedImageSampler]] Texture2D g_metadata;
[[vk::binding(2, 0)]] [[vk::combinedImageSampler]] SamplerState g_metadataSampler;
[[vk::binding(3, 0)]] [[vk::combinedImageSampler]] Texture2D g_depth;
[[vk::binding(3, 0)]] [[vk::combinedImageSampler]] SamplerState g_depthSampler;

struct Constants {
    float4 cameraPositionRadius;
    float4 cameraForwardTanHalf;
    float4 cameraUpAspect;
    float4 sunDirectionAngularRadius;
    float4 dimensionsMethod;
    float4 depthRange;
    float4 ellipsoidRadii; // xyz real semi-axes, w reserved
};
[[vk::push_constant]] Constants g;

float4 main(float4 pixel : SV_Position) : SV_Target0
{
    const float2 uv = pixel.xy / max(g.dimensionsMethod.xy, float2(1.0, 1.0));
    const float4 metadata = g_metadata.SampleLevel(g_metadataSampler, uv, 0);
    const float depth = g_depth.SampleLevel(g_depthSampler, uv, 0).r;
    const float3 scene = max(g_scene.SampleLevel(g_sceneSampler, uv, 0).rgb, 0.0);
    const bool hasSurface = metadata.a > 0.001;
    const bool hasDepth = depth > 0.0;
    const float3 eye = g.cameraPositionRadius.xyz;
    const float radius = g.cameraPositionRadius.w;
    const float3 forward = normalize(g.cameraForwardTanHalf.xyz);
    const float3 upRequested = normalize(g.cameraUpAspect.xyz);
    const float3 right = normalize(cross(forward, upRequested));
    const float3 up = normalize(cross(right, forward));
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    const float3 ray = normalize(forward +
        ndc.x * g.cameraUpAspect.w * g.cameraForwardTanHalf.w * right +
        ndc.y * g.cameraForwardTanHalf.w * up);

    // This analytic body hit is independent of the geometry depth buffer.
    // Comparing it with metadata coverage reveals whether the direct-light
    // pass is discarding a visibly drawn terrain/globe pixel.
    const float3 semiAxes = max(g.ellipsoidRadii.xyz, float3(1.0, 1.0, 1.0));
    const float3 scaledEye = eye / semiAxes;
    const float3 scaledRay = ray / semiAxes;
    const float a = dot(scaledRay, scaledRay);
    const float b = dot(scaledEye, scaledRay);
    const float discriminant = b * b - a * (dot(scaledEye, scaledEye) - 1.0);
    float3 position = 0.0;
    bool sphereHit = false; // Legacy identifier: now the actual ellipsoid hit.
    if (a > 1.0e-20 && discriminant >= 0.0)
    {
        const float root = sqrt(discriminant);
        const float t = (-b - root) / a;
        if (t > 0.0)
        {
            position = eye + ray * t;
            sphereHit = true;
        }
    }
    if (hasDepth)
    {
        const float nearPlane = max(g.depthRange.x, 1.0e-5);
        const float farPlane = max(g.depthRange.y, nearPlane + 1.0e-4);
        const float viewDepth = nearPlane * farPlane /
            max(depth * (farPlane - nearPlane) + nearPlane, 1.0e-6);
        position = eye + ray * (viewDepth / max(dot(ray, forward), 1.0e-5));
    }
    else if (sphereHit)
    {
        // Avoid exact ellipsoid self-occultation at the surface.
        position *= (1.0 + max(1.0e-6, 0.5 / max(radius, 1.0)));
    }
    const bool hasPosition = hasDepth || sphereHit;
    const float3 sun = normalize(g.sunDirectionAngularRadius.xyz);
    const float sunRadius = max(g.sunDirectionAngularRadius.w, 0.0);
    const bool terrainReady = g_terrain.Load(12u) > 0u &&
        g_terrain.Load(16u) > 0u;
    // Occlusion in ellipsoid-scaled coordinates. At the physical surface,
    // this gives the true ellipsoid tangent plane instead of treating its
    // polar terrain as INSIDE a max-axis bounding sphere.
    // Finite-disc widths are approximate under nonuniform scaling; the
    // central ray and terminator sign are geometrically consistent.
    const float3 scaledPosition = position / semiAxes;
    const float scaledDistance = length(scaledPosition);
    const float3 scaledSun = normalize(sun / semiAxes);
    const float3 outside = scaledPosition *
        (max(scaledDistance, 1.0 + 1.0e-6) / max(scaledDistance, 1.0e-8));
    const float sphereVisibility = hasPosition
        ? SphereStellarVisibility(outside, scaledSun, 1.0, sunRadius)
        : 0.0;
    const float method = g.dimensionsMethod.z;

    if (method < 1.5)
    {
        if (hasSurface && !hasPosition) return float4(0.0, 1.0, 1.0, 1.0);
        if (sphereHit && !hasSurface) return float4(1.0, 0.0, 1.0, 1.0);
        if (!hasPosition) return float4(0.0, 0.0, 0.0, 1.0);
        // Red=blocked, green=visible, yellow=partial solar disc.
        const float partial = step(0.001, sphereVisibility) *
            (1.0 - step(0.999, sphereVisibility));
        return float4(lerp(float3(1.0 - sphereVisibility,
            sphereVisibility, 0.0), float3(1.0, 1.0, 0.0), partial), 1.0);
    }
    if (method < 2.5)
    {
        if (!hasPosition) return float4(0.0, 0.0, 0.0, 1.0);
        // Report readiness before metadata classification; otherwise the
        // entire nightside appeared magenta while the field was missing.
        if (!terrainReady) return float4(0.0, 0.0, 1.0, 1.0);
        if (hasSurface && !hasPosition) return float4(1.0, 0.0, 1.0, 1.0);
        if (sphereHit && !hasSurface) return float4(1.0, 0.0, 1.0, 1.0);
        const float terrainVisibility = TerrainSunVisibility(
            g_terrain, position, sun, radius, sunRadius);
        // R=terrain visibility, G=analytic planet visibility,
        // B=heightfield was ready. Their difference exposes disagreement.
        return float4(terrainVisibility, sphereVisibility, 1.0, 1.0);
    }

    // Provenance diagnosis, NOT a decomposition of radiometric direct / GI:
    // R=pre-output scene luminance (log), G=valid G-buffer surface metadata,
    // B=valid geometric depth. A bright scene with zero metadata explains
    // why the direct-light shader's discard leaves it unchanged.
    const float brightness = dot(scene, float3(0.2126, 0.7152, 0.0722));
    return float4(saturate(log2(1.0 + brightness) * 0.2),
        hasSurface ? 1.0 : 0.0, hasDepth ? 1.0 : 0.0, 1.0);
}
)";
} // namespace

SunAuditViewportIndicator::SunAuditViewportIndicator(
    rhi::Device& device, const shader::Compiler& compiler)
{
    const auto vs = compiler.Compile({
        .source = kVs, .entryPoint = "main",
        .stage = shader::Stage::Vertex, .debug = false});
    const auto ps = compiler.Compile({
        .source = kWitnessPs, .entryPoint = "main",
        .stage = shader::Stage::Pixel, .debug = false});
    const auto desc = [&] {
        rhi::GraphicsPipelineDesc d{};
        d.vertexShader = {.data = vs.bytecode.data(), .size = vs.bytecode.size()};
        d.vertexAttributes = {};
        d.vertexStrideBytes = 0U;
        d.topology = rhi::PrimitiveTopology::TriangleList;
        d.fillMode = rhi::FillMode::Solid;
        d.cullMode = rhi::CullMode::None;
        d.blendMode = rhi::BlendMode::Opaque;
        d.depthTest = false;
        d.depthWrite = false;
        d.colorAttachmentFormats = {rhi::TextureFormat::RGBA8_UNorm};
        d.colorAttachmentCount = 1U;
        return d;
    };
    auto witnessDesc = desc();
    witnessDesc.pixelShader = {.data = ps.bytecode.data(), .size = ps.bytecode.size()};
    witnessDesc.pushConstantDwords = 4U;
    pipeline_ = device.CreateGraphicsPipeline(witnessDesc);

    std::string diagnosticSource(kDiagnosticPs);
    const auto main = diagnosticSource.find("float4 main(float4 pixel : SV_Position)");
    if (main == std::string::npos)
        throw std::logic_error("Sun audit diagnostic shader entry point not found.");
    diagnosticSource.insert(main, lighting::kSunVisibilityShader);
    const auto diagnosticPs = compiler.Compile({
        .source = diagnosticSource, .entryPoint = "main",
        .stage = shader::Stage::Pixel, .debug = false});
    auto diagnosticDesc = desc();
    diagnosticDesc.pixelShader = {
        .data = diagnosticPs.bytecode.data(),
        .size = diagnosticPs.bytecode.size()};
    diagnosticDesc.pushConstantDwords = 28U;
    diagnosticDesc.shaderResourceBuffers = 1U;
    diagnosticDesc.sampledTextures = 3U;
    diagnosticPipeline_ = device.CreateGraphicsPipeline(diagnosticDesc);
    emptyTerrainShadow_ = lighting::CreateEmptyTerrainSunShadowBuffer(device);
}

void SunAuditViewportIndicator::Draw(
    rhi::CommandList& commands, rhi::Texture& display,
    const SunAuditViewportInputs& inputs,
    const u32 width, const u32 height, const u8 method, const bool debug) const
{
    if (width == 0U || height == 0U || method == 0U) return;
    const auto bits = [](const f32 v) { return std::bit_cast<u32>(v); };
    commands.SetRenderTarget(display);
    commands.SetViewport({.x = 0.0F, .y = 0.0F,
        .width = static_cast<f32>(width), .height = static_cast<f32>(height),
        .minDepth = 0.0F, .maxDepth = 1.0F});
    commands.SetScissor({.left = 0, .top = 0,
        .right = static_cast<i32>(width), .bottom = static_cast<i32>(height)});

    if (debug && inputs.scene != nullptr &&
        inputs.emissionMetadata != nullptr && inputs.depth != nullptr)
    {
        const auto& c = inputs.camera;
        const std::array<u32, 28> diagnosticConstants{
            bits(static_cast<f32>(c.localPositionMeters.x)),
            bits(static_cast<f32>(c.localPositionMeters.y)),
            bits(static_cast<f32>(c.localPositionMeters.z)),
            bits(std::max(inputs.planetRadiusMeters, 0.0F)),
            bits(c.forward.x), bits(c.forward.y), bits(c.forward.z),
            bits(std::tan(c.verticalFovRadians * 0.5F)),
            bits(c.up.x), bits(c.up.y), bits(c.up.z),
            bits(static_cast<f32>(width) / static_cast<f32>(height)),
            bits(inputs.sunDirection.x), bits(inputs.sunDirection.y),
            bits(inputs.sunDirection.z),
            bits(inputs.sunAngularRadiusRadians),
            bits(static_cast<f32>(width)), bits(static_cast<f32>(height)),
            bits(static_cast<f32>(method)), 1U,
            bits(c.nearPlaneMeters), bits(c.farPlaneMeters), 0U, 0U,
            bits(inputs.bodyRadiiMeters.x), bits(inputs.bodyRadiiMeters.y),
            bits(inputs.bodyRadiiMeters.z), 0U
        };
        commands.SetGraphicsPipeline(*diagnosticPipeline_);
        commands.SetGraphicsConstants(diagnosticConstants);
        commands.SetGraphicsBuffer(0U, inputs.terrainShadow != nullptr
            ? *inputs.terrainShadow : *emptyTerrainShadow_);
        commands.SetGraphicsTexture(0U, *inputs.scene);
        commands.SetGraphicsTexture(1U, *inputs.emissionMetadata);
        commands.SetGraphicsTexture(2U, *inputs.depth);
        commands.Draw(6U);
    }

    const std::array<u32, 4> constants{
        bits(static_cast<f32>(method)),
        bits(debug ? 1.0F : 0.0F),
        bits(static_cast<f32>(width)),
        bits(static_cast<f32>(height))
    };
    commands.SetGraphicsPipeline(*pipeline_);
    commands.SetGraphicsConstants(constants);
    commands.Draw(6U);
}
} // namespace orbit::studio_ui
