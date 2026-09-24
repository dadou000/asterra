#include <orbit/lighting/DirectLighting.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>

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
struct GpuLocalLight
{
    float4 positionType;
    float4 directionRange;
    float4 colorFlux;
    float4 cone;
};

[[vk::binding(0, 0)]]
StructuredBuffer<GpuLocalLight> g_localLights;

[[vk::binding(1, 0)]]
StructuredBuffer<uint> g_tileOffsets;

[[vk::binding(2, 0)]]
StructuredBuffer<uint> g_tileLightIndices;
[[vk::binding(3, 0)]]
StructuredBuffer<uint4> g_particleLightGrid;

[[vk::binding(4, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_baseRoughness;
[[vk::binding(4, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_baseSampler;

[[vk::binding(5, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_normalMetallic;
[[vk::binding(5, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_normalSampler;

[[vk::binding(6, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_emissionClass;
[[vk::binding(6, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_emissionSampler;

[[vk::binding(7, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_depth;
[[vk::binding(7, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_depthSampler;

struct Constants
{
    float4 lightDirectionAndScale;
    float4 lightColorAndAmbient;
    float4 cameraForwardAndAspect;
    float4 cameraUpAndTanHalfFov;
    float4 depthRangeAndPhotometry;
    uint4 localGrid;
    float4 cameraFrameAndParticleGrid;
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

float3 EvaluateBrdf(
    float3 n,
    float3 v,
    float3 l,
    float3 baseColor,
    float roughness,
    float metallic)
{
    const float nDotL =
        saturate(dot(n, l));

    if (nDotL <= 0.0)
    {
        return 0.0;
    }

    const float nDotV =
        saturate(dot(n, v));
    const float3 h =
        normalize(l + v);
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

    return
        (diffuse + specular) *
        nDotL;
}

float ReverseZViewDepth(float depth)
{
    const float nearPlane =
        max(g.depthRangeAndPhotometry.x, 1.0e-5);
    const float farPlane =
        max(
            g.depthRangeAndPhotometry.y,
            nearPlane + 1.0e-4);

    return
        nearPlane * farPlane /
        max(
            depth * (farPlane - nearPlane) +
                nearPlane,
            1.0e-6);
}

float3 ReconstructSurfacePosition(
    float depth,
    float3 cameraRay,
    float3 cameraForward)
{
    const float viewDepth =
        ReverseZViewDepth(depth);

    const float rayForward =
        max(
            dot(cameraRay, cameraForward),
            1.0e-5);

    return
        cameraRay *
        (viewDepth / rayForward);
}

float RangeAttenuation(float distanceMeters, float rangeMeters)
{
    const float normalized =
        distanceMeters /
        max(rangeMeters, 1.0e-4);

    const float quartic =
        normalized *
        normalized *
        normalized *
        normalized;

    const float smooth =
        saturate(1.0 - quartic);

    return smooth * smooth;
}

float LocalLightIrradianceScale(
    GpuLocalLight light,
    float distanceMeters,
    float3 surfaceToLight)
{
    const float luminousFlux =
        max(light.colorFlux.w, 0.0);

    const float luminousEfficacy =
        max(
            g.depthRangeAndPhotometry.w,
            1.0);

    const float radiantWatts =
        luminousFlux /
        luminousEfficacy;

    const float isSpot =
        step(0.5, light.positionType.w);

    float solidAngle =
        4.0 * 3.14159265;

    float angular = 1.0;

    if (isSpot > 0.5)
    {
        const float outerCos =
            clamp(light.cone.y, -1.0, 1.0);

        solidAngle =
            max(
                2.0 * 3.14159265 *
                    (1.0 - outerCos),
                1.0e-4);

        const float3 lightDirection =
            normalize(light.directionRange.xyz);

        const float spotCos =
            dot(
                -surfaceToLight,
                lightDirection);

        angular =
            smoothstep(
                outerCos,
                max(light.cone.x, outerCos + 1.0e-5),
                spotCos);
    }

    const float radiantIntensity =
        radiantWatts /
        solidAngle;

    const float inverseSquare =
        1.0 /
        max(
            distanceMeters *
            distanceMeters,
            0.0025);

    const float referenceIrradiance =
        max(
            g.depthRangeAndPhotometry.z,
            1.0e-5);

    return
        radiantIntensity *
        inverseSquare *
        RangeAttenuation(
            distanceMeters,
            light.directionRange.w) *
        angular /
        referenceIrradiance;
}


float4 SampleParticleLightGrid(float3 framePosition)
{
    if (g.cameraFrameAndParticleGrid.w <= 0.0) return 0.0;
    const uint4 meta = g_particleLightGrid[1];
    const float3 origin = float3(asfloat(meta.x),asfloat(meta.y),asfloat(meta.z));
    const float cellSize = asfloat(meta.w);
    if (!(cellSize > 0.0)) return 0.0;
    const int3 cell = int3(floor((framePosition-origin)/cellSize));
    if (any(cell < 0) || any(cell >= int3(32,32,32))) return 0.0;
    const uint index = 2u + (uint(cell.z)*32u + uint(cell.y))*32u + uint(cell.x);
    const uint4 packed = g_particleLightGrid[index];
    return float4(float(packed.x)/4096.0, float3(packed.y,packed.z,packed.w)/1024.0);
}

float ParticleGridTransmittance(float3 framePosition,float3 direction,float maximumDistance)
{
    if (g.cameraFrameAndParticleGrid.w <= 0.0 || maximumDistance <= 1.0e-4) return 1.0;
    const float3 d=normalize(direction);
    const float distance=min(maximumDistance,192.0);
    const float stepLength=max(distance/6.0,8.0);
    float optical=0.0;
    [unroll] for(uint i=1u;i<=6u;++i)
    {
        const float t=min(stepLength*float(i),distance);
        optical += SampleParticleLightGrid(framePosition+d*t).x * 0.18;
    }
    return exp(-min(optical,20.0));
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

    // Metadata 0 means no physical surface was written at this pixel. Keep
    // whatever the forward passes produced there (space, stellar corona and
    // chromosphere, glare) instead of replacing it with a flat fill.
    if (emissionClass.a <= 0.0)
    {
        discard;
    }

    const float3 baseColor =
        max(baseRoughness.rgb, 0.0);
    const float roughness =
        saturate(baseRoughness.a);
    const float metallic =
        saturate(normalMetallic.a);
    const float3 n =
        normalize(normalMetallic.xyz);

    const float3 cameraForward =
        normalize(g.cameraForwardAndAspect.xyz);
    const float3 requestedUp =
        normalize(g.cameraUpAndTanHalfFov.xyz);
    const float3 cameraRight =
        normalize(cross(cameraForward, requestedUp));
    const float3 cameraUp =
        normalize(cross(cameraRight, cameraForward));
    const float aspect =
        max(g.cameraForwardAndAspect.w, 0.001);
    const float tanHalfFov =
        max(g.cameraUpAndTanHalfFov.w, 0.001);

    const float2 ndc =
        float2(
            input.uv.x * 2.0 - 1.0,
            1.0 - input.uv.y * 2.0);

    const float3 cameraRay =
        normalize(
            cameraForward +
            cameraRight *
                (ndc.x * aspect * tanHalfFov) +
            cameraUp *
                (ndc.y * tanHalfFov));

    const float3 v =
        normalize(-cameraRay);

    const float3 stellarDirection =
        normalize(g.lightDirectionAndScale.xyz);
    const float stellarIrradiance =
        max(g.lightDirectionAndScale.w, 0.0);
    const float3 stellarColor =
        max(g.lightColorAndAmbient.rgb, 0.0);

    float3 stellarLinear =
        EvaluateBrdf(
            n,
            v,
            stellarDirection,
            baseColor,
            roughness,
            metallic) *
        stellarColor *
        stellarIrradiance;

    const float ambient =
        max(g.lightColorAndAmbient.w, 0.0);
    const float3 ambientLinear =
        baseColor *
        (1.0 - metallic) *
        ambient;
    float3 sceneLinear = stellarLinear + ambientLinear;

    const float depth =
        g_depth.Sample(
            g_depthSampler,
            input.uv).r;

    if (depth > 0.0 && g.cameraFrameAndParticleGrid.w > 0.0)
    {
        const float3 surfaceRelative =
            ReconstructSurfacePosition(depth,cameraRay,cameraForward);
        const float3 framePosition =
            surfaceRelative + g.cameraFrameAndParticleGrid.xyz;
        const float stellarParticleT =
            ParticleGridTransmittance(framePosition,stellarDirection,192.0);
        const float3 particleEmission =
            SampleParticleLightGrid(framePosition).yzw;
        sceneLinear = stellarLinear * stellarParticleT + ambientLinear + particleEmission;
    }

    // Reverse-Z depth 0 is the untouched/far clear value. Analytic globe
    // representations currently do not populate geometric depth, so local
    // lights are intentionally skipped there while stellar lighting remains
    // common. Ground/local geometry receives exact camera-relative positions.
    if (depth > 0.0 &&
        g.localGrid.w > 0u &&
        g.localGrid.x > 0u &&
        g.localGrid.y > 0u)
    {
        const float3 surfacePosition =
            ReconstructSurfacePosition(
                depth,
                cameraRay,
                cameraForward);

        const uint2 pixel =
            uint2(
                max(input.position.x, 0.0),
                max(input.position.y, 0.0));

        const uint2 tile =
            min(
                pixel / g.localGrid.x,
                uint2(
                    g.localGrid.y - 1u,
                    g.localGrid.z - 1u));

        const uint tileIndex =
            tile.y * g.localGrid.y +
            tile.x;

        const uint first =
            g_tileOffsets[tileIndex];
        const uint end =
            g_tileOffsets[tileIndex + 1u];

        [loop]
        for (uint cursor = first;
             cursor < end;
             ++cursor)
        {
            const uint lightIndex =
                g_tileLightIndices[cursor];

            if (lightIndex >= g.localGrid.w)
            {
                continue;
            }

            const GpuLocalLight local =
                g_localLights[lightIndex];

            const float3 delta =
                local.positionType.xyz -
                surfacePosition;

            const float distanceMeters =
                length(delta);

            if (distanceMeters <= 1.0e-4 ||
                distanceMeters >=
                    local.directionRange.w)
            {
                continue;
            }

            const float3 l =
                delta /
                distanceMeters;

            const float irradianceScale =
                LocalLightIrradianceScale(
                    local,
                    distanceMeters,
                    l);

            if (irradianceScale <= 0.0)
            {
                continue;
            }

            const float3 framePosition =
                surfacePosition + g.cameraFrameAndParticleGrid.xyz;
            const float particleT =
                ParticleGridTransmittance(framePosition,l,distanceMeters);
            sceneLinear +=
                EvaluateBrdf(
                    n,
                    v,
                    l,
                    baseColor,
                    roughness,
                    metallic) *
                max(local.colorFlux.rgb, 0.0) *
                irradianceScale * particleT;
        }
    }

    sceneLinear +=
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
            .pushConstantDwords = 28U,
            .shaderResourceBuffers = 4U,
            .sampledTextures = 4U,
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

    dummyParticleLightGrid_ = device.CreateBuffer({
        .sizeBytes = 2U * sizeof(std::array<u32,4U>),
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::ShaderResource
    });
    std::memset(dummyParticleLightGrid_->Map(),0,static_cast<std::size_t>(dummyParticleLightGrid_->SizeBytes()));
    dummyParticleLightGrid_->Unmap();
}

void DirectLightingRenderer::Draw(
    rhi::CommandList& commands,
    rhi::Texture& surfaceBaseRoughness,
    rhi::Texture& surfaceNormalMetallic,
    rhi::Texture& surfaceEmissionClass,
    rhi::Texture& depth,
    rhi::Buffer& localLights,
    rhi::Buffer& tileOffsets,
    rhi::Buffer& tileLightIndices,
    rhi::Texture& targetSceneColor,
    const u32 width,
    const u32 height,
    const LightingView& view,
    const DirectionalLight& light,
    const TiledLightGrid& localLightGrid,
    rhi::Buffer* particleLightGrid,
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

    constexpr f32 kSolarReferenceIrradiance =
        1361.0F;
    constexpr f32 kPhotopicLuminousEfficacy =
        683.0F;

    const std::array<u32, 28> constants{
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
            0.0F)),

        bits(view.forward.x),
        bits(view.forward.y),
        bits(view.forward.z),
        bits(static_cast<f32>(width) /
             static_cast<f32>(height)),

        bits(view.up.x),
        bits(view.up.y),
        bits(view.up.z),
        bits(std::tan(
            view.verticalFovRadians * 0.5F)),

        bits(std::max(
            view.nearPlaneMeters,
            1.0e-5F)),
        bits(std::max(
            view.farPlaneMeters,
            view.nearPlaneMeters + 1.0e-4F)),
        bits(kSolarReferenceIrradiance),
        bits(kPhotopicLuminousEfficacy),

        localLightGrid.tileSizePixels,
        localLightGrid.tilesX,
        localLightGrid.tilesY,
        static_cast<u32>(
            localLightGrid.lights.size()),

        bits(static_cast<f32>(view.cameraPositionInFrameMeters.x)),
        bits(static_cast<f32>(view.cameraPositionInFrameMeters.y)),
        bits(static_cast<f32>(view.cameraPositionInFrameMeters.z)),
        bits(particleLightGrid != nullptr ? 1.0F : 0.0F)
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

    commands.SetGraphicsBuffer(
        0U,
        localLights);
    commands.SetGraphicsBuffer(
        1U,
        tileOffsets);
    commands.SetGraphicsBuffer(
        2U,
        tileLightIndices);
    commands.SetGraphicsBuffer(
        3U,
        particleLightGrid != nullptr
            ? *particleLightGrid
            : *dummyParticleLightGrid_);

    commands.SetGraphicsTexture(
        0U,
        surfaceBaseRoughness);
    commands.SetGraphicsTexture(
        1U,
        surfaceNormalMetallic);
    commands.SetGraphicsTexture(
        2U,
        surfaceEmissionClass);
    commands.SetGraphicsTexture(
        3U,
        depth);

    commands.Draw(6U);
}
} // namespace orbit::lighting
