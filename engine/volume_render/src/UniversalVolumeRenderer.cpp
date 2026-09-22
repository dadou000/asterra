#include <orbit/volume_render/UniversalVolumeRenderer.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace orbit::volume_render
{
namespace
{
constexpr u32 kInvalidSlot = ~0U;
constexpr u32 kMaximumVolumeLocalLights = 16U;

struct alignas(16) GpuVolumeParams
{
    math::Float4 cameraForwardAspect{};
    math::Float4 cameraUpTanHalfFov{};
    math::Float4 cameraPositionTemporal{};
    math::Float4 domainCenterExtinction{};
    math::Float4 domainHalfAlbedo{};
    math::Float4 scatteringColorAnisotropy{};
    math::Float4 emissionColorScale{};
    math::Float4 stellarDirectionScale{};
    math::Float4 stellarColorAmbient{};
    math::Float4 depthRangeHistory{};

    std::array<i32,4> minimumTile{};
    std::array<u32,4> tileLayout{};
    std::array<u32,4> logicalResolution{};
    std::array<u32,4> renderParams{};
};

static_assert(
    sizeof(GpuVolumeParams) == 224U);

[[nodiscard]] render_graph::BufferHandle
FindField(
    const volume_fields::ImportedVolumeFields& fields,
    const world_model::VolumeField field)
{
    for (const auto& channel :
         fields.channels)
    {
        if (channel.field == field)
        {
            return channel.buffer;
        }
    }

    return {};
}

[[nodiscard]] std::unique_ptr<rhi::GraphicsPipeline>
CompileFullscreen(
    rhi::Device& device,
    const shader::Compiler& compiler,
    const char* pixelSource,
    const u32 pushDwords,
    const u32 buffers,
    const u32 textures)
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

    const auto vs =
        compiler.Compile({
            .source = kVs,
            .entryPoint = "main",
            .stage = shader::Stage::Vertex,
            .debug = false
        });

    const auto ps =
        compiler.Compile({
            .source = pixelSource,
            .entryPoint = "main",
            .stage = shader::Stage::Pixel,
            .debug = false
        });

    if (vs.bytecode.empty() ||
        ps.bytecode.empty())
    {
        throw std::runtime_error(
            "Orbit failed to compile M35 universal volume shaders.");
    }

    return device.CreateGraphicsPipeline({
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
        .pushConstantDwords = pushDwords,
        .shaderResourceBuffers = buffers,
        .sampledTextures = textures,
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

constexpr const char* kRaymarchPs = R"(
struct GpuLocalLight
{
    float4 positionType;
    float4 directionRange;
    float4 colorFlux;
    float4 cone;
};

struct GpuVolumeParams
{
    float4 cameraForwardAspect;
    float4 cameraUpTanHalfFov;
    float4 cameraPositionTemporal;
    float4 domainCenterExtinction;
    float4 domainHalfAlbedo;
    float4 scatteringColorAnisotropy;
    float4 emissionColorScale;
    float4 stellarDirectionScale;
    float4 stellarColorAmbient;
    float4 depthRangeHistory;

    int4 minimumTile;
    uint4 tileLayout;
    uint4 logicalResolution;
    uint4 renderParams;
};

[[vk::binding(0, 0)]]
ByteAddressBuffer g_density : register(t0);
[[vk::binding(1, 0)]]
ByteAddressBuffer g_emission : register(t1);
[[vk::binding(2, 0)]]
StructuredBuffer<uint> g_slotMap : register(t2);
[[vk::binding(3, 0)]]
StructuredBuffer<GpuLocalLight> g_localLights : register(t3);
[[vk::binding(4, 0)]]
StructuredBuffer<GpuVolumeParams> g_params : register(t4);

[[vk::binding(5, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_depth;
[[vk::binding(5, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_depthSampler;

[[vk::binding(6, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_history;
[[vk::binding(6, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_historySampler;

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float SafeRcp(float x)
{
    return
        abs(x) > 1.0e-8
            ? 1.0 / x
            : (x >= 0.0
                ? 1.0e8
                : -1.0e8);
}

bool RayBox(
    float3 origin,
    float3 direction,
    float3 minimum,
    float3 maximum,
    out float nearDistance,
    out float farDistance)
{
    const float3 inv =
        float3(
            SafeRcp(direction.x),
            SafeRcp(direction.y),
            SafeRcp(direction.z));

    const float3 a =
        (minimum - origin) * inv;
    const float3 b =
        (maximum - origin) * inv;

    const float3 lo = min(a, b);
    const float3 hi = max(a, b);

    nearDistance =
        max(
            max(lo.x, lo.y),
            lo.z);
    farDistance =
        min(
            min(hi.x, hi.y),
            hi.z);

    return
        farDistance >=
            max(nearDistance, 0.0);
}

float ReverseZViewDepth(
    float depth,
    float nearPlane,
    float farPlane)
{
    nearPlane =
        max(nearPlane, 1.0e-5);
    farPlane =
        max(
            farPlane,
            nearPlane + 1.0e-4);

    return
        nearPlane * farPlane /
        max(
            depth *
                (farPlane - nearPlane) +
            nearPlane,
            1.0e-6);
}

// cell size/tile edge are packed into render-independent values reconstructed
// from domain extent and logical resolution. Keeping this helper explicit
// avoids a second coordinate convention for surface and Local3D volumes.
float SampleField(
    ByteAddressBuffer field,
    float3 worldPosition,
    GpuVolumeParams p,
    float3 cellSize,
    uint tileEdge)
{
    const float3 tileWorld =
        cellSize *
        float(tileEdge);

    const int3 tile =
        int3(
            floor(
                worldPosition /
                tileWorld));

    const int3 logical =
        tile -
        p.minimumTile.xyz;

    if (any(logical < 0) ||
        logical.x >= int(p.tileLayout.x) ||
        logical.y >= int(p.tileLayout.y) ||
        logical.z >= int(p.tileLayout.z))
    {
        return 0.0;
    }

    const uint mapIndex =
        (uint(logical.z) *
             p.tileLayout.y +
         uint(logical.y)) *
            p.tileLayout.x +
        uint(logical.x);

    const uint slot =
        g_slotMap[mapIndex];

    if (slot == 0xffffffffu)
    {
        return 0.0;
    }

    const float3 globalCell =
        floor(
            worldPosition /
            cellSize);

    int3 local =
        int3(globalCell) -
        tile *
            int(tileEdge);

    local =
        clamp(
            local,
            0,
            int(tileEdge) - 1);

    const uint localIndex =
        uint(local.z) *
            tileEdge *
            tileEdge +
        uint(local.y) *
            tileEdge +
        uint(local.x);

    const uint tileCellCount =
        tileEdge *
        tileEdge *
        tileEdge;

    return
        asfloat(
            field.Load(
                (slot *
                     tileCellCount +
                 localIndex) *
                    4u));
}

float HenyeyGreenstein(
    float cosineTheta,
    float anisotropy)
{
    const float g =
        clamp(
            anisotropy,
            -0.95,
            0.95);
    const float g2 =
        g * g;
    const float denominator =
        max(
            1.0 +
                g2 -
                2.0 *
                g *
                clamp(
                    cosineTheta,
                    -1.0,
                    1.0),
            1.0e-4);

    return
        (1.0 - g2) /
        (12.566370614359172 *
         pow(
             denominator,
             1.5));
}

float RangeAttenuation(
    float distanceMeters,
    float rangeMeters)
{
    const float normalized =
        distanceMeters /
        max(
            rangeMeters,
            1.0e-4);
    const float quartic =
        normalized *
        normalized *
        normalized *
        normalized;
    const float smooth =
        saturate(
            1.0 - quartic);

    return smooth * smooth;
}

float LocalLightScale(
    GpuLocalLight light,
    float distanceMeters,
    float3 sampleToLight)
{
    const float luminousFlux =
        max(
            light.colorFlux.w,
            0.0);

    const float radiantWatts =
        luminousFlux /
        683.0;

    const bool spot =
        light.positionType.w > 0.5;

    float solidAngle =
        12.566370614359172;
    float angular =
        1.0;

    if (spot)
    {
        const float outerCos =
            clamp(
                light.cone.y,
                -1.0,
                1.0);

        solidAngle =
            max(
                6.283185307179586 *
                    (1.0 -
                     outerCos),
                1.0e-4);

        angular =
            smoothstep(
                outerCos,
                max(
                    light.cone.x,
                    outerCos +
                        1.0e-5),
                dot(
                    -sampleToLight,
                    normalize(
                        light.directionRange.xyz)));
    }

    const float radiantIntensity =
        radiantWatts /
        solidAngle;

    return
        radiantIntensity /
        max(
            distanceMeters *
                distanceMeters,
            0.0025) *
        RangeAttenuation(
            distanceMeters,
            light.directionRange.w) *
        angular /
        1361.0;
}

float ShadowTransmittance(
    float3 start,
    float3 direction,
    float maximumDistance,
    GpuVolumeParams p,
    float3 cellSize,
    uint tileEdge)
{
    const uint steps =
        p.renderParams.y;

    if (steps == 0u ||
        maximumDistance <= 1.0e-4)
    {
        return 1.0;
    }

    float nearBox;
    float farBox;

    const float3 minimum =
        p.domainCenterExtinction.xyz -
        p.domainHalfAlbedo.xyz;
    const float3 maximum =
        p.domainCenterExtinction.xyz +
        p.domainHalfAlbedo.xyz;

    if (!RayBox(
            start,
            direction,
            minimum,
            maximum,
            nearBox,
            farBox))
    {
        return 1.0;
    }

    const float distance =
        min(
            maximumDistance,
            max(
                farBox,
                0.0));

    if (distance <= 1.0e-4)
    {
        return 1.0;
    }

    const float stepLength =
        distance /
        float(steps);

    float opticalDepth =
        0.0;

    [loop]
    for (uint index = 0u;
         index < steps;
         ++index)
    {
        const float t =
            (float(index) + 0.5) *
            stepLength;

        const float density =
            max(
                SampleField(
                    g_density,
                    start +
                        direction * t,
                    p,
                    cellSize,
                    tileEdge),
                0.0);

        opticalDepth +=
            density *
            p.domainCenterExtinction.w *
            stepLength;

        if (opticalDepth > 12.0)
        {
            return 0.0;
        }
    }

    return
        exp(-opticalDepth);
}

float4 main(VSOutput input) : SV_Target0
{
    const GpuVolumeParams p =
        g_params[0];

    const float3 forward =
        normalize(
            p.cameraForwardAspect.xyz);
    const float3 requestedUp =
        normalize(
            p.cameraUpTanHalfFov.xyz);
    const float3 right =
        normalize(
            cross(
                forward,
                requestedUp));
    const float3 up =
        normalize(
            cross(
                right,
                forward));

    const float2 ndc =
        float2(
            input.uv.x * 2.0 - 1.0,
            1.0 -
                input.uv.y * 2.0);

    const float3 rayDirection =
        normalize(
            forward +
            right *
                (ndc.x *
                 max(
                     p.cameraForwardAspect.w,
                     0.001) *
                 max(
                     p.cameraUpTanHalfFov.w,
                     0.001)) +
            up *
                (ndc.y *
                 max(
                     p.cameraUpTanHalfFov.w,
                     0.001)));

    const float3 rayOrigin =
        p.cameraPositionTemporal.xyz;

    const float3 minimum =
        p.domainCenterExtinction.xyz -
        p.domainHalfAlbedo.xyz;
    const float3 maximum =
        p.domainCenterExtinction.xyz +
        p.domainHalfAlbedo.xyz;

    float nearDistance;
    float farDistance;

    if (!RayBox(
            rayOrigin,
            rayDirection,
            minimum,
            maximum,
            nearDistance,
            farDistance))
    {
        return float4(
            0.0,0.0,0.0,1.0);
    }

    nearDistance =
        max(
            nearDistance,
            0.0);

    const float depth =
        g_depth.Sample(
            g_depthSampler,
            input.uv).r;

    if (depth > 0.0)
    {
        const float viewDepth =
            ReverseZViewDepth(
                depth,
                p.depthRangeHistory.x,
                p.depthRangeHistory.y);

        const float rayForward =
            max(
                dot(
                    rayDirection,
                    forward),
                1.0e-5);

        const float surfaceDistance =
            viewDepth /
            rayForward;

        farDistance =
            min(
                farDistance,
                surfaceDistance);
    }

    if (farDistance <= nearDistance)
    {
        return float4(
            0.0,0.0,0.0,1.0);
    }

    const uint raySteps =
        max(
            p.renderParams.x,
            1u);
    const uint tileEdge =
        max(
            p.renderParams.z,
            1u);

    const float3 logicalResolution =
        float3(
            max(
                p.logicalResolution.x,
                1u),
            max(
                p.logicalResolution.y,
                1u),
            max(
                p.logicalResolution.z,
                1u));

    const float3 cellSize =
        (p.domainHalfAlbedo.xyz *
         2.0) /
        logicalResolution;

    const float segmentLength =
        farDistance -
        nearDistance;
    const float stepLength =
        segmentLength /
        float(raySteps);

    float transmittance =
        1.0;
    float3 scatteringAccum =
        0.0;
    float3 emissionAccum =
        0.0;
    float shadowAccum =
        0.0;
    float shadowWeight =
        0.0;

    const float3 stellarDirection =
        normalize(
            p.stellarDirectionScale.xyz);
    const float stellarScale =
        max(
            p.stellarDirectionScale.w,
            0.0);
    const float3 stellarColor =
        max(
            p.stellarColorAmbient.rgb,
            0.0);
    const float ambient =
        max(
            p.stellarColorAmbient.w,
            0.0);

    [loop]
    for (uint step = 0u;
         step < raySteps;
         ++step)
    {
        const float distance =
            nearDistance +
            (float(step) + 0.5) *
                stepLength;

        const float3 worldPosition =
            rayOrigin +
            rayDirection *
                distance;

        const float density =
            max(
                SampleField(
                    g_density,
                    worldPosition,
                    p,
                    cellSize,
                    tileEdge),
                0.0);

        float emissionScalar =
            0.0;

        if (p.renderParams.w != 0u)
        {
            emissionScalar =
                max(
                    SampleField(
                        g_emission,
                        worldPosition,
                        p,
                        cellSize,
                        tileEdge),
                    0.0);
        }

        const float sigmaT =
            density *
            max(
                p.domainCenterExtinction.w,
                0.0);

        if (sigmaT <= 1.0e-8 &&
            emissionScalar <= 1.0e-8)
        {
            continue;
        }

        float3 scattering =
            0.0;
        float weightedShadow =
            1.0;
        float shadowSamples =
            1.0;

        if (sigmaT > 1.0e-8)
        {
            const float stellarShadow =
                ShadowTransmittance(
                    worldPosition +
                        stellarDirection *
                            0.01,
                    stellarDirection,
                    1.0e20,
                    p,
                    cellSize,
                    tileEdge);

            const float phaseStellar =
                HenyeyGreenstein(
                    dot(
                        stellarDirection,
                        -rayDirection),
                    p.scatteringColorAnisotropy.w);

            float3 incident =
                stellarColor *
                stellarScale *
                stellarShadow *
                phaseStellar;

            incident +=
                ambient *
                0.07957747154594767;

            const float3 sampleRelative =
                worldPosition -
                rayOrigin;

            const uint localCount =
                min(
                    p.tileLayout.w,
                    16u);

            weightedShadow =
                stellarShadow;

            [loop]
            for (uint lightIndex = 0u;
                 lightIndex < localCount;
                 ++lightIndex)
            {
                const GpuLocalLight light =
                    g_localLights[
                        lightIndex];

                const float3 delta =
                    light.positionType.xyz -
                    sampleRelative;
                const float lightDistance =
                    length(delta);

                if (lightDistance <= 1.0e-4 ||
                    lightDistance >=
                        light.directionRange.w)
                {
                    continue;
                }

                const float3 sampleToLight =
                    delta /
                    lightDistance;

                const float scale =
                    LocalLightScale(
                        light,
                        lightDistance,
                        sampleToLight);

                if (scale <= 0.0)
                {
                    continue;
                }

                const float shadow =
                    ShadowTransmittance(
                        worldPosition +
                            sampleToLight *
                                0.01,
                        sampleToLight,
                        lightDistance,
                        p,
                        cellSize,
                        tileEdge);

                const float phase =
                    HenyeyGreenstein(
                        dot(
                            sampleToLight,
                            -rayDirection),
                        p.scatteringColorAnisotropy.w);

                incident +=
                    max(
                        light.colorFlux.rgb,
                        0.0) *
                    scale *
                    shadow *
                    phase;

                weightedShadow +=
                    shadow;
                shadowSamples +=
                    1.0;
            }

            const float3 sigmaS =
                sigmaT *
                saturate(
                    p.domainHalfAlbedo.w) *
                max(
                    p.scatteringColorAnisotropy.rgb,
                    0.0);

            scattering =
                sigmaS *
                incident;
        }

        const float3 emission =
            emissionScalar *
            max(
                p.emissionColorScale.rgb,
                0.0) *
            max(
                p.emissionColorScale.w,
                0.0);

        const float segmentT =
            sigmaT > 1.0e-8
                ? exp(
                      -sigmaT *
                      stepLength)
                : 1.0;

        const float integral =
            sigmaT > 1.0e-8
                ? (1.0 -
                   segmentT) /
                      sigmaT
                : stepLength;

        scatteringAccum +=
            transmittance *
            scattering *
            integral;
        emissionAccum +=
            transmittance *
            emission *
            integral;

        shadowAccum +=
            weightedShadow /
            shadowSamples;
        shadowWeight +=
            1.0;

        transmittance *=
            segmentT;

        if (transmittance <
            0.002)
        {
            break;
        }
    }

    const uint debugMode =
        uint(
            p.depthRangeHistory.w +
            0.5);

    float4 current;

    if (debugMode == 1u)
    {
        current =
            float4(
                scatteringAccum,
                0.0);
    }
    else if (debugMode == 2u)
    {
        const float extinction =
            1.0 -
            transmittance;
        current =
            float4(
                extinction.xxx,
                0.0);
    }
    else if (debugMode == 3u)
    {
        current =
            float4(
                emissionAccum,
                0.0);
    }
    else if (debugMode == 4u)
    {
        const float shadow =
            shadowWeight > 0.0
                ? shadowAccum /
                    shadowWeight
                : 1.0;
        current =
            float4(
                shadow.xxx,
                0.0);
    }
    else
    {
        current =
            float4(
                scatteringAccum +
                    emissionAccum,
                transmittance);
    }

    const bool historyValid =
        p.depthRangeHistory.z >
        0.5;

    const bool temporalEnabled =
        p.cameraPositionTemporal.w >
        0.0 &&
        debugMode == 0u;

    if (historyValid &&
        temporalEnabled)
    {
        const float4 history =
            g_history.Sample(
                g_historySampler,
                input.uv);

        const float radianceDelta =
            length(
                current.rgb -
                history.rgb);
        const float transmittanceDelta =
            abs(
                current.a -
                history.a);

        const float rejection =
            saturate(
                radianceDelta * 2.5 +
                transmittanceDelta * 6.0);

        const float historyWeight =
            saturate(
                p.cameraPositionTemporal.w) *
            (1.0 -
             rejection);

        current =
            lerp(
                current,
                history,
                historyWeight);
    }

    return current;
}
)";

constexpr const char* kCompositePs = R"(
[[vk::binding(0, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_scene;
[[vk::binding(0, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_sceneSampler;

[[vk::binding(1, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_volume;
[[vk::binding(1, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_volumeSampler;

struct Constants
{
    uint debugMode;
};

[[vk::push_constant]]
Constants g;

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(VSOutput input) : SV_Target0
{
    const float4 volume =
        g_volume.Sample(
            g_volumeSampler,
            input.uv);

    if (g.debugMode != 0u)
    {
        return float4(
            max(volume.rgb, 0.0),
            1.0);
    }

    const float3 scene =
        g_scene.Sample(
            g_sceneSampler,
            input.uv).rgb;

    return float4(
        max(
            volume.rgb +
            scene *
                saturate(volume.a),
            0.0),
        1.0);
}
)";

constexpr const char* kCopyPs = R"(
[[vk::binding(0, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_source;
[[vk::binding(0, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_sampler;

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(VSOutput input) : SV_Target0
{
    return
        g_source.Sample(
            g_sampler,
            input.uv);
}
)";

void ConfigureFullscreen(
    rhi::CommandList& commands,
    rhi::Texture& target,
    const u32 width,
    const u32 height)
{
    commands.SetRenderTarget(target);
    commands.SetViewport({
        .x = 0.0F,
        .y = 0.0F,
        .width =
            static_cast<f32>(width),
        .height =
            static_cast<f32>(height),
        .minDepth = 0.0F,
        .maxDepth = 1.0F
    });
    commands.SetScissor({
        .left = 0,
        .top = 0,
        .right =
            static_cast<i32>(width),
        .bottom =
            static_cast<i32>(height)
    });
}
} // namespace

f32 HenyeyGreensteinPhase(
    const f32 cosineTheta,
    const f32 anisotropy) noexcept
{
    const f32 g =
        std::clamp(
            anisotropy,
            -0.95F,
            0.95F);
    const f32 g2 =
        g * g;
    const f32 denominator =
        std::max(
            1.0F +
                g2 -
                2.0F *
                    g *
                    std::clamp(
                        cosineTheta,
                        -1.0F,
                        1.0F),
            1.0e-4F);

    return
        (1.0F - g2) /
        (12.566370614359172F *
         std::pow(
             denominator,
             1.5F));
}

HomogeneousVolumeReference
IntegrateHomogeneousVolume(
    const f32 density,
    const f32 distanceMeters,
    const f32 extinctionScale,
    const f32 singleScatteringAlbedo,
    const math::Float3 scatteringColor,
    const math::Float3 incidentRadiance,
    const math::Float3 emissionColor,
    const f32 emissionScalar,
    const f32 emissionScale) noexcept
{
    const f32 length =
        std::max(
            distanceMeters,
            0.0F);
    const f32 sigmaT =
        std::max(
            density,
            0.0F) *
        std::max(
            extinctionScale,
            0.0F);

    if (length <= 0.0F)
    {
        return {};
    }

    const f32 transmittance =
        sigmaT > 1.0e-8F
            ? std::exp(
                  -sigmaT *
                  length)
            : 1.0F;
    const f32 integral =
        sigmaT > 1.0e-8F
            ? (1.0F -
               transmittance) /
                  sigmaT
            : length;
    const f32 albedo =
        std::clamp(
            singleScatteringAlbedo,
            0.0F,
            1.0F);

    const math::Float3 sigmaS{
        sigmaT *
            albedo *
            std::max(
                scatteringColor.x,
                0.0F),
        sigmaT *
            albedo *
            std::max(
                scatteringColor.y,
                0.0F),
        sigmaT *
            albedo *
            std::max(
                scatteringColor.z,
                0.0F)
    };

    HomogeneousVolumeReference result{
        .transmittance =
            transmittance,
        .scatteredRadiance = {
            sigmaS.x *
                std::max(
                    incidentRadiance.x,
                    0.0F) *
                integral,
            sigmaS.y *
                std::max(
                    incidentRadiance.y,
                    0.0F) *
                integral,
            sigmaS.z *
                std::max(
                    incidentRadiance.z,
                    0.0F) *
                integral
        },
        .emittedRadiance = {
            std::max(
                emissionColor.x,
                0.0F) *
                std::max(
                    emissionScalar,
                    0.0F) *
                std::max(
                    emissionScale,
                    0.0F) *
                integral,
            std::max(
                emissionColor.y,
                0.0F) *
                std::max(
                    emissionScalar,
                    0.0F) *
                std::max(
                    emissionScale,
                    0.0F) *
                integral,
            std::max(
                emissionColor.z,
                0.0F) *
                std::max(
                    emissionScalar,
                    0.0F) *
                std::max(
                    emissionScale,
                    0.0F) *
                integral
        }
    };

    result.totalRadiance = {
        result.scatteredRadiance.x +
            result.emittedRadiance.x,
        result.scatteredRadiance.y +
            result.emittedRadiance.y,
        result.scatteredRadiance.z +
            result.emittedRadiance.z
    };

    return result;
}

class UniversalVolumeRenderer::Impl
{
public:
    struct Presentation
    {
        scene::ObjectId volume{};
        std::string viewport;
        u32 width{0U};
        u32 height{0U};

        std::unique_ptr<rhi::Texture>
            current;
        std::unique_ptr<rhi::Texture>
            history;
        std::unique_ptr<rhi::Texture>
            composite;

        std::unique_ptr<rhi::Buffer>
            slotMap;
        std::unique_ptr<rhi::Buffer>
            localLights;
        std::unique_ptr<rhi::Buffer>
            params;

        u32 slotCapacity{0U};
        u32 lightCapacity{0U};
        bool historyValid{false};
    };

    struct Entry
    {
        scene::ObjectId volume{};
        VolumeRenderRuntimeSettings settings{};
        VolumeRenderDiagnostics diagnostics{};
    };

    Impl(
        rhi::Device& device,
        const shader::Compiler& compiler)
        : device(&device)
    {
        raymarchPipeline =
            CompileFullscreen(
                device,
                compiler,
                kRaymarchPs,
                0U,
                5U,
                2U);
        compositePipeline =
            CompileFullscreen(
                device,
                compiler,
                kCompositePs,
                1U,
                0U,
                2U);
        copyPipeline =
            CompileFullscreen(
                device,
                compiler,
                kCopyPs,
                0U,
                0U,
                1U);
    }

    [[nodiscard]] Entry&
    EnsureEntry(
        const scene::ObjectId volume)
    {
        auto found =
            std::find_if(
                entries.begin(),
                entries.end(),
                [volume](const Entry& entry)
                {
                    return entry.volume ==
                        volume;
                });

        if (found == entries.end())
        {
            entries.push_back({
                .volume = volume
            });
            return entries.back();
        }

        return *found;
    }

    [[nodiscard]] Presentation&
    EnsurePresentation(
        const scene::ObjectId volume,
        const std::string_view viewport,
        const u32 width,
        const u32 height)
    {
        auto found =
            std::find_if(
                presentations.begin(),
                presentations.end(),
                [volume, viewport](
                    const Presentation& item)
                {
                    return
                        item.volume ==
                            volume &&
                        item.viewport ==
                            viewport;
                });

        if (found ==
            presentations.end())
        {
            presentations.push_back({
                .volume = volume,
                .viewport =
                    std::string(viewport)
            });
            found =
                std::prev(
                    presentations.end());
        }

        if (found->width != width ||
            found->height != height ||
            found->current == nullptr ||
            found->history == nullptr ||
            found->composite == nullptr)
        {
            found->width = width;
            found->height = height;
            found->current =
                device->CreateTexture({
                    .width = width,
                    .height = height,
                    .format =
                        rhi::TextureFormat::
                            RGBA16_Float,
                    .initialState =
                        rhi::ResourceState::
                            ShaderResource
                });
            found->history =
                device->CreateTexture({
                    .width = width,
                    .height = height,
                    .format =
                        rhi::TextureFormat::
                            RGBA16_Float,
                    .initialState =
                        rhi::ResourceState::
                            ShaderResource
                });
            found->composite =
                device->CreateTexture({
                    .width = width,
                    .height = height,
                    .format =
                        rhi::TextureFormat::
                            RGBA16_Float,
                    .initialState =
                        rhi::ResourceState::
                            ShaderResource
                });
            found->historyValid =
                false;
        }

        return *found;
    }

    rhi::Device* device{nullptr};
    std::unique_ptr<rhi::GraphicsPipeline>
        raymarchPipeline;
    std::unique_ptr<rhi::GraphicsPipeline>
        compositePipeline;
    std::unique_ptr<rhi::GraphicsPipeline>
        copyPipeline;
    std::vector<Entry> entries;
    std::vector<Presentation>
        presentations;
};

UniversalVolumeRenderer::UniversalVolumeRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler)
    : impl_(
          std::make_unique<Impl>(
              device,
              compiler))
{
}

VolumeRenderRuntimeSettings&
UniversalVolumeRenderer::Settings(
    const scene::ObjectId volume)
{
    return
        impl_->EnsureEntry(
            volume).
            settings;
}

VolumeRenderDiagnostics
UniversalVolumeRenderer::Diagnostics(
    const scene::ObjectId volume) const noexcept
{
    const auto found =
        std::find_if(
            impl_->entries.begin(),
            impl_->entries.end(),
            [volume](const Impl::Entry& entry)
            {
                return entry.volume ==
                    volume;
            });

    return
        found != impl_->entries.end()
            ? found->diagnostics
            : VolumeRenderDiagnostics{};
}

void UniversalVolumeRenderer::RemoveMissing(
    const scene::ObjectStore& objects)
{
    std::erase_if(
        impl_->entries,
        [&objects](const Impl::Entry& entry)
        {
            const auto record =
                objects.Find(
                    entry.volume);
            return
                !record.has_value() ||
                record->type !=
                    world_model::
                        kVolumeType;
        });

    std::erase_if(
        impl_->presentations,
        [&objects](const Impl::Presentation& item)
        {
            const auto record =
                objects.Find(
                    item.volume);
            return
                !record.has_value() ||
                record->type !=
                    world_model::
                        kVolumeType;
        });
}

void UniversalVolumeRenderer::AddPasses(
    render_graph::RenderGraph& graph,
    const std::string_view prefix,
    const std::string_view viewportId,
    const render_graph::TextureHandle sceneColor,
    const render_graph::TextureHandle depth,
    const u32 width,
    const u32 height,
    const render_view::CameraState& camera,
    const lighting::LightingView& lightingView,
    const world_model::ResolvedVolumeDomain& domain,
    volume_fields::VolumeFieldStorage& storage,
    const volume_fields::ImportedVolumeFields& fields,
    const lighting::DirectionalLight& stellar,
    const std::span<const lighting::ResolvedLocalLight> localLights,
    const bool resetHistory)
{
    auto& entry =
        impl_->EnsureEntry(
            domain.object);

    entry.diagnostics = {};

    const auto density =
        FindField(
            fields,
            world_model::
                VolumeField::Density);

    if (!domain.enabled ||
        !domain.renderEnabled ||
        !density.IsValid() ||
        width == 0U ||
        height == 0U)
    {
        return;
    }

    auto& presentation =
        impl_->EnsurePresentation(
            domain.object,
            viewportId,
            width,
            height);

    if (resetHistory)
    {
        presentation.historyValid =
            false;
    }

    const auto& fieldDiagnostics =
        storage.Diagnostics();
    const auto tiles =
        storage.Tiles();

    const u32 mapCount =
        std::max(
            fieldDiagnostics.tilesX *
                fieldDiagnostics.tilesY *
                fieldDiagnostics.tilesZ,
            1U);

    if (presentation.slotCapacity <
            mapCount ||
        presentation.slotMap == nullptr)
    {
        presentation.slotCapacity =
            mapCount;
        presentation.slotMap =
            impl_->device->
                CreateBuffer({
                    .sizeBytes =
                        static_cast<u64>(
                            mapCount) *
                        sizeof(u32),
                    .usage =
                        rhi::BufferUsage::
                            Structured,
                    .memory =
                        rhi::MemoryUsage::
                            HostVisible,
                    .initialState =
                        rhi::ResourceState::
                            ShaderResource
                });
    }

    i32 minimumX =
        std::numeric_limits<i32>::max();
    i32 minimumY =
        std::numeric_limits<i32>::max();
    i32 minimumZ =
        std::numeric_limits<i32>::max();

    for (const auto& tile : tiles)
    {
        if (!tile.resident)
        {
            continue;
        }

        minimumX =
            std::min(
                minimumX,
                tile.coord.x);
        minimumY =
            std::min(
                minimumY,
                tile.coord.y);
        minimumZ =
            std::min(
                minimumZ,
                tile.coord.z);
    }

    if (minimumX ==
        std::numeric_limits<i32>::max())
    {
        return;
    }

    auto* slotMap =
        reinterpret_cast<u32*>(
            presentation.slotMap->
                Map());

    std::fill_n(
        slotMap,
        presentation.slotCapacity,
        kInvalidSlot);

    for (const auto& tile : tiles)
    {
        if (!tile.resident)
        {
            continue;
        }

        const i32 lx =
            tile.coord.x -
            minimumX;
        const i32 ly =
            tile.coord.y -
            minimumY;
        const i32 lz =
            tile.coord.z -
            minimumZ;

        if (lx < 0 ||
            ly < 0 ||
            lz < 0 ||
            lx >=
                static_cast<i32>(
                    fieldDiagnostics.tilesX) ||
            ly >=
                static_cast<i32>(
                    fieldDiagnostics.tilesY) ||
            lz >=
                static_cast<i32>(
                    fieldDiagnostics.tilesZ))
        {
            continue;
        }

        const u32 index =
            (static_cast<u32>(lz) *
                 fieldDiagnostics.tilesY +
             static_cast<u32>(ly)) *
                fieldDiagnostics.tilesX +
            static_cast<u32>(lx);

        slotMap[index] =
            tile.slot;
    }

    presentation.slotMap->Unmap();

    const math::Double3 domainDelta =
        domain.centerMeters -
        lightingView.
            cameraPositionInFrameMeters;

    const math::Float3 domainCenterRelative{
        static_cast<f32>(
            domainDelta.x),
        static_cast<f32>(
            domainDelta.y),
        static_cast<f32>(
            domainDelta.z)
    };

    struct RankedLight
    {
        f32 distance{0.0F};
        lighting::ResolvedLocalLight light{};
    };

    std::vector<RankedLight>
        rankedLights;

    const f32 domainRadius =
        static_cast<f32>(
            std::sqrt(
                domain.halfExtentsMeters.x *
                    domain.halfExtentsMeters.x +
                domain.halfExtentsMeters.y *
                    domain.halfExtentsMeters.y +
                domain.halfExtentsMeters.z *
                    domain.halfExtentsMeters.z));

    for (const auto& light :
         localLights)
    {
        const math::Float3 delta{
            light.positionCameraRelativeMeters.x -
                domainCenterRelative.x,
            light.positionCameraRelativeMeters.y -
                domainCenterRelative.y,
            light.positionCameraRelativeMeters.z -
                domainCenterRelative.z
        };

        const f32 distance =
            math::Length(delta);

        if (distance >
            light.rangeMeters +
                domainRadius)
        {
            continue;
        }

        rankedLights.push_back({
            .distance = distance,
            .light = light
        });
    }

    std::stable_sort(
        rankedLights.begin(),
        rankedLights.end(),
        [](const RankedLight& a,
           const RankedLight& b)
        {
            if (a.distance != b.distance)
            {
                return
                    a.distance <
                    b.distance;
            }

            return
                a.light.stableId <
                b.light.stableId;
        });

    if (rankedLights.size() >
        kMaximumVolumeLocalLights)
    {
        rankedLights.resize(
            kMaximumVolumeLocalLights);
    }

    const u32 lightCount =
        static_cast<u32>(
            rankedLights.size());
    const u32 lightCapacity =
        std::max(
            lightCount,
            1U);

    if (presentation.lightCapacity <
            lightCapacity ||
        presentation.localLights ==
            nullptr)
    {
        presentation.lightCapacity =
            std::max(
                lightCapacity,
                std::max(
                    presentation.
                        lightCapacity *
                        2U,
                    4U));

        presentation.localLights =
            impl_->device->
                CreateBuffer({
                    .sizeBytes =
                        static_cast<u64>(
                            presentation.
                                lightCapacity) *
                        sizeof(
                            lighting::
                                GpuLocalLight),
                    .usage =
                        rhi::BufferUsage::
                            Structured,
                    .memory =
                        rhi::MemoryUsage::
                            HostVisible,
                    .initialState =
                        rhi::ResourceState::
                            ShaderResource
                });
    }

    auto* gpuLights =
        reinterpret_cast<
            lighting::GpuLocalLight*>(
                presentation.localLights->
                    Map());

    std::fill_n(
        gpuLights,
        presentation.lightCapacity,
        lighting::GpuLocalLight{});

    for (u32 index = 0U;
         index < lightCount;
         ++index)
    {
        gpuLights[index] =
            lighting::EncodeGpuLocalLight(
                rankedLights[index].light);
    }

    presentation.localLights->Unmap();

    if (presentation.params == nullptr)
    {
        presentation.params =
            impl_->device->
                CreateBuffer({
                    .sizeBytes =
                        sizeof(
                            GpuVolumeParams),
                    .usage =
                        rhi::BufferUsage::
                            Structured,
                    .memory =
                        rhi::MemoryUsage::
                            HostVisible,
                    .initialState =
                        rhi::ResourceState::
                            ShaderResource
                });
    }

    const f32 aspect =
        static_cast<f32>(width) /
        static_cast<f32>(height);
    const f32 tanHalfFov =
        std::tan(
            camera.verticalFovRadians *
            0.5F);

    const auto& runtimeSettings =
        entry.settings;

    GpuVolumeParams params{
        .cameraForwardAspect = {
            camera.forward.x,
            camera.forward.y,
            camera.forward.z,
            aspect
        },
        .cameraUpTanHalfFov = {
            camera.up.x,
            camera.up.y,
            camera.up.z,
            tanHalfFov
        },
        .cameraPositionTemporal = {
            static_cast<f32>(
                camera.
                    localPositionMeters.x),
            static_cast<f32>(
                camera.
                    localPositionMeters.y),
            static_cast<f32>(
                camera.
                    localPositionMeters.z),
            runtimeSettings.temporalEnabled
                ? domain.temporalWeight
                : 0.0F
        },
        .domainCenterExtinction = {
            static_cast<f32>(
                domain.centerMeters.x),
            static_cast<f32>(
                domain.centerMeters.y),
            static_cast<f32>(
                domain.centerMeters.z),
            domain.extinctionScale
        },
        .domainHalfAlbedo = {
            static_cast<f32>(
                std::abs(
                    domain.halfExtentsMeters.x)),
            static_cast<f32>(
                std::abs(
                    domain.halfExtentsMeters.y)),
            static_cast<f32>(
                std::abs(
                    domain.halfExtentsMeters.z)),
            domain.singleScatteringAlbedo
        },
        .scatteringColorAnisotropy = {
            domain.scatteringColor.x,
            domain.scatteringColor.y,
            domain.scatteringColor.z,
            domain.anisotropy
        },
        .emissionColorScale = {
            domain.emissionColor.x,
            domain.emissionColor.y,
            domain.emissionColor.z,
            domain.emissionScale
        },
        .stellarDirectionScale = {
            stellar.directionToLight.x,
            stellar.directionToLight.y,
            stellar.directionToLight.z,
            stellar.irradianceScale
        },
        .stellarColorAmbient = {
            stellar.colorLinear.x,
            stellar.colorLinear.y,
            stellar.colorLinear.z,
            0.035F
        },
        .depthRangeHistory = {
            camera.nearPlaneMeters,
            camera.farPlaneMeters,
            presentation.historyValid
                ? 1.0F
                : 0.0F,
            static_cast<f32>(
                runtimeSettings.debugMode)
        },
        .minimumTile = {
            minimumX,
            minimumY,
            minimumZ,
            0
        },
        .tileLayout = {
            fieldDiagnostics.tilesX,
            fieldDiagnostics.tilesY,
            fieldDiagnostics.tilesZ,
            lightCount
        },
        .logicalResolution = {
            fieldDiagnostics.resolutionX,
            fieldDiagnostics.resolutionY,
            fieldDiagnostics.resolutionZ,
            0U
        },
        .renderParams = {
            domain.renderSteps,
            domain.shadowSteps,
            fieldDiagnostics.tileEdge,
            FindField(
                fields,
                world_model::
                    VolumeField::Emission).
                    IsValid()
                ? 1U
                : 0U
        }
    };

    auto* mappedParams =
        presentation.params->Map();

    std::memcpy(
        mappedParams,
        &params,
        sizeof(params));

    presentation.params->Unmap();

    const auto currentHandle =
        graph.ImportTexture(
            std::string(prefix) +
                ".VolumeCurrent",
            *presentation.current,
            rhi::ResourceState::
                ShaderResource);
    const auto historyHandle =
        graph.ImportTexture(
            std::string(prefix) +
                ".VolumeHistory",
            *presentation.history,
            rhi::ResourceState::
                ShaderResource);
    const auto compositeHandle =
        graph.ImportTexture(
            std::string(prefix) +
                ".VolumeComposite",
            *presentation.composite,
            rhi::ResourceState::
                ShaderResource);

    const auto slotMapHandle =
        graph.ImportBuffer(
            std::string(prefix) +
                ".VolumeSlotMap",
            *presentation.slotMap,
            rhi::ResourceState::
                ShaderResource);
    const auto localLightsHandle =
        graph.ImportBuffer(
            std::string(prefix) +
                ".VolumeLocalLights",
            *presentation.localLights,
            rhi::ResourceState::
                ShaderResource);
    const auto paramsHandle =
        graph.ImportBuffer(
            std::string(prefix) +
                ".VolumeParams",
            *presentation.params,
            rhi::ResourceState::
                ShaderResource);

    auto emission =
        FindField(
            fields,
            world_model::
                VolumeField::Emission);

    if (!emission.IsValid())
    {
        emission =
            density;
    }

    graph.AddPass(
        std::string(prefix) +
            ".VolumeRaymarch",
        {
            {
                .texture = depth,
                .state =
                    rhi::ResourceState::
                        DepthRead,
                .access =
                    render_graph::Access::
                        Read
            },
            {
                .texture =
                    historyHandle,
                .state =
                    rhi::ResourceState::
                        ShaderResource,
                .access =
                    render_graph::Access::
                        Read
            },
            {
                .texture =
                    currentHandle,
                .state =
                    rhi::ResourceState::
                        RenderTarget,
                .access =
                    render_graph::Access::
                        Write
            }
        },
        {
            {density,rhi::ResourceState::ShaderResource,render_graph::Access::Read},
            {emission,rhi::ResourceState::ShaderResource,render_graph::Access::Read},
            {slotMapHandle,rhi::ResourceState::ShaderResource,render_graph::Access::Read},
            {localLightsHandle,rhi::ResourceState::ShaderResource,render_graph::Access::Read},
            {paramsHandle,rhi::ResourceState::ShaderResource,render_graph::Access::Read}
        },
        [this,
         currentHandle,
         depth,
         historyHandle,
         density,
         emission,
         slotMapHandle,
         localLightsHandle,
         paramsHandle,
         width,
         height](
            rhi::CommandList& commands,
            const render_graph::Resources& resources)
        {
            ConfigureFullscreen(
                commands,
                resources.Texture(
                    currentHandle),
                width,
                height);
            commands.SetGraphicsPipeline(
                *impl_->raymarchPipeline);
            commands.SetGraphicsBuffer(
                0U,
                resources.Buffer(density));
            commands.SetGraphicsBuffer(
                1U,
                resources.Buffer(emission));
            commands.SetGraphicsBuffer(
                2U,
                resources.Buffer(
                    slotMapHandle));
            commands.SetGraphicsBuffer(
                3U,
                resources.Buffer(
                    localLightsHandle));
            commands.SetGraphicsBuffer(
                4U,
                resources.Buffer(
                    paramsHandle));
            commands.SetGraphicsTexture(
                0U,
                resources.Texture(depth));
            commands.SetGraphicsTexture(
                1U,
                resources.Texture(
                    historyHandle));
            commands.Draw(6U);
        });

    graph.AddPass(
        std::string(prefix) +
            ".VolumeComposite",
        {
            {
                .texture = sceneColor,
                .state =
                    rhi::ResourceState::
                        ShaderResource,
                .access =
                    render_graph::Access::
                        Read
            },
            {
                .texture =
                    currentHandle,
                .state =
                    rhi::ResourceState::
                        ShaderResource,
                .access =
                    render_graph::Access::
                        Read
            },
            {
                .texture =
                    compositeHandle,
                .state =
                    rhi::ResourceState::
                        RenderTarget,
                .access =
                    render_graph::Access::
                        Write
            }
        },
        [this,
         sceneColor,
         currentHandle,
         compositeHandle,
         width,
         height,
         debugMode =
             runtimeSettings.debugMode](
            rhi::CommandList& commands,
            const render_graph::Resources& resources)
        {
            ConfigureFullscreen(
                commands,
                resources.Texture(
                    compositeHandle),
                width,
                height);
            commands.SetGraphicsPipeline(
                *impl_->compositePipeline);
            const std::array<u32,1>
                constants{
                    static_cast<u32>(
                        debugMode)
                };
            commands.SetGraphicsConstants(
                constants);
            commands.SetGraphicsTexture(
                0U,
                resources.Texture(
                    sceneColor));
            commands.SetGraphicsTexture(
                1U,
                resources.Texture(
                    currentHandle));
            commands.Draw(6U);
        });

    graph.AddPass(
        std::string(prefix) +
            ".VolumeCopyBack",
        {
            {
                .texture =
                    compositeHandle,
                .state =
                    rhi::ResourceState::
                        ShaderResource,
                .access =
                    render_graph::Access::
                        Read
            },
            {
                .texture = sceneColor,
                .state =
                    rhi::ResourceState::
                        RenderTarget,
                .access =
                    render_graph::Access::
                        Write
            }
        },
        [this,
         sceneColor,
         compositeHandle,
         width,
         height](
            rhi::CommandList& commands,
            const render_graph::Resources& resources)
        {
            ConfigureFullscreen(
                commands,
                resources.Texture(
                    sceneColor),
                width,
                height);
            commands.SetGraphicsPipeline(
                *impl_->copyPipeline);
            commands.SetGraphicsTexture(
                0U,
                resources.Texture(
                    compositeHandle));
            commands.Draw(6U);
        });

    graph.AddPass(
        std::string(prefix) +
            ".VolumeHistory",
        {
            {
                .texture =
                    currentHandle,
                .state =
                    rhi::ResourceState::
                        ShaderResource,
                .access =
                    render_graph::Access::
                        Read
            },
            {
                .texture =
                    historyHandle,
                .state =
                    rhi::ResourceState::
                        RenderTarget,
                .access =
                    render_graph::Access::
                        Write
            }
        },
        [this,
         currentHandle,
         historyHandle,
         width,
         height](
            rhi::CommandList& commands,
            const render_graph::Resources& resources)
        {
            ConfigureFullscreen(
                commands,
                resources.Texture(
                    historyHandle),
                width,
                height);
            commands.SetGraphicsPipeline(
                *impl_->copyPipeline);
            commands.SetGraphicsTexture(
                0U,
                resources.Texture(
                    currentHandle));
            commands.Draw(6U);
        });

    graph.AddPass(
        std::string(prefix) +
            ".VolumeRestoreDepth",
        {
            {
                .texture = depth,
                .state =
                    rhi::ResourceState::
                        DepthWrite,
                .access =
                    render_graph::Access::
                        Write
            }
        },
        [](
            rhi::CommandList&,
            const render_graph::Resources&)
        {
        });

    presentation.historyValid =
        true;

    entry.diagnostics = {
        .rendered = true,
        .historyValid =
            presentation.historyValid,
        .raymarchSteps =
            domain.renderSteps,
        .shadowSteps =
            domain.shadowSteps,
        .localLightCount =
            lightCount,
        .residentTiles =
            fieldDiagnostics.
                residentTiles,
        .historyBytes =
            static_cast<u64>(
                width) *
            height *
            8U,
        .scratchBytes =
            static_cast<u64>(
                width) *
            height *
            8U *
            2U
    };
}

std::string_view
VolumeRenderDebugModeName(
    const VolumeRenderDebugMode mode) noexcept
{
    switch (mode)
    {
    case VolumeRenderDebugMode::Composite:
        return "Composite";
    case VolumeRenderDebugMode::Scattering:
        return "Scattering";
    case VolumeRenderDebugMode::Extinction:
        return "Extinction";
    case VolumeRenderDebugMode::Emission:
        return "Emission";
    case VolumeRenderDebugMode::Shadow:
        return "Shadow";
    }

    return "Unknown";
}
} // namespace orbit::volume_render
