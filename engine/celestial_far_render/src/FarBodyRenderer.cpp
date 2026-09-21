#include <orbit/celestial_far_render/FarBodyRenderer.hpp>

#include <orbit/terrain/TerrainContracts.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <type_traits>
#include <variant>

namespace orbit::celestial_far_render
{
namespace
{
[[nodiscard]] math::Double3 DiscDirection(
    const f64 x,
    const f64 y)
{
    const f64 r2 =
        x * x + y * y;

    if (r2 > 1.0)
    {
        return {};
    }

    return math::Normalize(
        math::Double3{
            x,
            y,
            std::sqrt(
                std::max(
                    1.0 - r2,
                    0.0))
        });
}

[[nodiscard]] const celestial_appearance::AppearanceTexel&
SampleAppearance(
    const celestial_appearance::PlanetaryAppearanceProduct& appearance,
    const math::Double3 d)
{
    const f64 ax = std::abs(d.x);
    const f64 ay = std::abs(d.y);
    const f64 az = std::abs(d.z);

    u32 face = 0U;
    f64 u = 0.0;
    f64 v = 0.0;

    if (ax >= ay && ax >= az)
    {
        if (d.x >= 0.0)
        {
            face = 0U;
            u = -d.z / ax;
            v = d.y / ax;
        }
        else
        {
            face = 1U;
            u = d.z / ax;
            v = d.y / ax;
        }
    }
    else if (ay >= ax && ay >= az)
    {
        if (d.y >= 0.0)
        {
            face = 2U;
            u = d.x / ay;
            v = -d.z / ay;
        }
        else
        {
            face = 3U;
            u = d.x / ay;
            v = d.z / ay;
        }
    }
    else
    {
        if (d.z >= 0.0)
        {
            face = 4U;
            u = d.x / az;
            v = d.y / az;
        }
        else
        {
            face = 5U;
            u = -d.x / az;
            v = d.y / az;
        }
    }

    const f64 fx =
        (u * 0.5 + 0.5) *
        static_cast<f64>(
            appearance.faceResolution - 1U);
    const f64 fy =
        (v * 0.5 + 0.5) *
        static_cast<f64>(
            appearance.faceResolution - 1U);

    const u32 x =
        std::min(
            static_cast<u32>(
                std::llround(fx)),
            appearance.faceResolution - 1U);
    const u32 y =
        std::min(
            static_cast<u32>(
                std::llround(fy)),
            appearance.faceResolution - 1U);

    return appearance.At(
        face,
        x,
        y);
}

[[nodiscard]] u8 ToLinearUnorm8(
    const f32 linear) noexcept
{
    return static_cast<u8>(
        std::clamp(
            std::lround(
                std::clamp(
                    linear,
                    0.0F,
                    1.0F) *
                255.0F),
            0L,
            255L));
}

[[nodiscard]] u64 DiscFingerprint(
    const celestial_appearance::PlanetaryAppearanceProduct& appearance,
    const CachedDiscConfig& config)
{
    u64 value =
        0x4D31384449534331ULL;
    value =
        terrain::StableCombine64(
            value,
            appearance.fingerprint);
    value =
        terrain::StableCombine64(
            value,
            config.resolution);
    return value;
}

[[nodiscard]] universe::EllipsoidShape AsEllipsoid(
    const universe::BodyShape& shape)
{
    return std::visit(
        [](const auto& value)
        {
            using Shape =
                std::decay_t<
                    decltype(value)>;

            if constexpr (
                std::is_same_v<
                    Shape,
                    universe::SphereShape>)
            {
                return universe::EllipsoidShape{
                    .radiiMeters = {
                        value.radiusMeters,
                        value.radiusMeters,
                        value.radiusMeters
                    }
                };
            }
            else
            {
                return value;
            }
        },
        shape);
}

constexpr const char* kQuadVs = R"(
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

    VSOutput o;
    o.position = float4(p[vertexId], 0, 1);
    o.uv = p[vertexId];
    return o;
}
)";

constexpr const char* kAnalyticPs = R"(
struct Constants
{
    float4 radiiAndAspect;
    float4 cameraAndTanHalfFov;
    float4 forward;
    float4 up;
    float4 albedoAndRoughness;
    float4 material;
    float4 emissionAndOpacity;
    float4 proxy;
};
[[vk::push_constant]] Constants g;

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(VSOutput input) : SV_Target0
{
    const uint mode = (uint)round(g.proxy.x);
    const float opacity = saturate(g.emissionAndOpacity.w);
    const float2 p = input.uv;

    if (mode == 1u)
    {
        const float radiusNdc =
            max(g.proxy.y, 0.00025);
        const float2 q =
            p / radiusNdc;
        const float r2 =
            dot(q, q);

        if (r2 > 1.0)
            discard;

        const float z =
            sqrt(
                max(
                    1.0 - r2,
                    0.0));

        const float3 n =
            normalize(
                float3(
                    q.x,
                    -q.y,
                    z));

        const float3 l =
            normalize(
                float3(
                    0.55,
                    0.72,
                    0.48));

        const float ndl =
            saturate(
                dot(n, l));

        const float roughness =
            saturate(
                g.albedoAndRoughness.w);
        const float ocean =
            saturate(
                g.material.x);
        const float ice =
            saturate(
                g.material.y);
        const float limb =
            pow(
                1.0 - saturate(z),
                3.0);

        const float stellar =
            saturate(g.material.z);
        const float radiometricIntensity =
            max(g.proxy.w, 0.0);

        float3 color =
            g.albedoAndRoughness.xyz *
                (0.05 + 0.95 * ndl) +
            ocean *
                limb *
                (1.0 - roughness) *
                float3(
                    0.20,
                    0.32,
                    0.45) +
            ice * 0.025 +
            g.emissionAndOpacity.xyz;

        if (stellar > 0.5)
        {
            const float limbDarkening =
                0.58 + 0.42 * z;
            color =
                g.albedoAndRoughness.xyz *
                radiometricIntensity *
                limbDarkening;
        }

        return float4(
            color,
            opacity);
    }

    if (mode >= 2u)
    {
        const float radiusNdc =
            max(g.proxy.y, 0.00025);
        const float2 q =
            p / radiusNdc;
        const float r2 = dot(q, q);

        if (r2 > 1.0)
            discard;

        const float edge =
            saturate((1.0 - r2) * 4.0);

        const float fluxScale =
            saturate(g.proxy.z);
        const float radiometricIntensity =
            max(g.proxy.w, 0.0);

        float3 color =
            (g.albedoAndRoughness.xyz +
             g.emissionAndOpacity.xyz) *
            fluxScale *
            radiometricIntensity;

        if (mode == 3u)
        {
            color +=
                float3(1.0, 0.88, 0.62) *
                (0.4 + 0.6 * edge) *
                fluxScale;
        }

        return float4(
            color,
            opacity * edge);
    }

    const float3 radii =
        max(
            g.radiiAndAspect.xyz,
            float3(0.001, 0.001, 0.001));
    const float3 camera =
        g.cameraAndTanHalfFov.xyz;
    const float3 forward =
        normalize(g.forward.xyz);
    const float3 requestedUp =
        normalize(g.up.xyz);
    const float3 right =
        normalize(cross(forward, requestedUp));
    const float3 cameraUp =
        normalize(cross(right, forward));
    const float tanHalf =
        max(g.cameraAndTanHalfFov.w, 0.001);

    const float3 ray =
        normalize(
            forward +
            right *
                (p.x *
                 g.radiiAndAspect.w *
                 tanHalf) -
            cameraUp *
                (p.y * tanHalf));

    const float3 ro = camera / radii;
    const float3 rd = ray / radii;
    const float a = dot(rd, rd);
    const float b = 2.0 * dot(ro, rd);
    const float c = dot(ro, ro) - 1.0;
    const float disc =
        b * b - 4.0 * a * c;

    if (disc < 0.0)
        discard;

    const float t =
        (-b - sqrt(disc)) /
        (2.0 * a);

    if (t < 0.0)
        discard;

    const float3 hit =
        camera + ray * t;

    const float3 n =
        normalize(float3(
            hit.x / (radii.x * radii.x),
            hit.y / (radii.y * radii.y),
            hit.z / (radii.z * radii.z)));

    const float3 l =
        normalize(float3(0.55, 0.72, -0.48));

    const float ndl =
        saturate(dot(n, l));
    const float roughness =
        saturate(g.albedoAndRoughness.w);
    const float ocean =
        saturate(g.material.x);
    const float ice =
        saturate(g.material.y);

    const float diffuse =
        0.045 + 0.955 * ndl;
    const float rim =
        pow(1.0 - saturate(abs(dot(n, -ray))), 4.0);

    float3 color =
        g.albedoAndRoughness.xyz * diffuse +
        ocean * rim *
            (1.0 - roughness) *
            float3(0.20, 0.32, 0.45) +
        ice * 0.025 +
        g.emissionAndOpacity.xyz;

    if (g.material.z > 0.5)
    {
        const float radiometricIntensity =
            max(g.proxy.w, 0.0);
        const float centerToLimb =
            saturate(abs(dot(n, -ray)));
        color =
            g.albedoAndRoughness.xyz *
            radiometricIntensity *
            (0.58 + 0.42 * centerToLimb);
    }

    return float4(
        color,
        opacity);
}
)";

constexpr const char* kCachedPs = R"(
[[vk::binding(0, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_disc;

[[vk::binding(0, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_sampler;

struct Constants
{
    float4 proxy;
};
[[vk::push_constant]] Constants g;

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(VSOutput input) : SV_Target0
{
    const float radiusNdc =
        max(g.proxy.x, 0.00025);
    const float2 q =
        input.uv / radiusNdc;

    if (dot(q, q) > 1.0)
        discard;

    const float2 uv =
        float2(
            q.x * 0.5 + 0.5,
            0.5 - q.y * 0.5);

    float4 color =
        g_disc.Sample(
            g_sampler,
            uv);

    color.a *=
        saturate(g.proxy.y);

    return color;
}
)";
} // namespace

AppearanceSummary SummarizeAppearance(
    const celestial_appearance::PlanetaryAppearanceProduct& appearance)
{
    if (appearance.texels.empty())
    {
        throw std::invalid_argument(
            "Cannot summarize an empty planetary appearance product.");
    }

    math::Double3 albedo{};
    math::Double3 emission{};
    f64 roughness = 0.0;
    f64 ocean = 0.0;
    f64 ice = 0.0;

    for (const auto& texel :
         appearance.texels)
    {
        albedo =
            albedo +
            math::Double3{
                texel.albedoLinear.x,
                texel.albedoLinear.y,
                texel.albedoLinear.z
            };
        emission =
            emission +
            math::Double3{
                texel.emissionLinear.x,
                texel.emissionLinear.y,
                texel.emissionLinear.z
            };
        roughness += texel.roughness;
        ocean += texel.oceanMask;
        ice += texel.iceMask;
    }

    const f64 inv =
        1.0 /
        static_cast<f64>(
            appearance.texels.size());

    return {
        .albedoLinear = {
            static_cast<f32>(albedo.x * inv),
            static_cast<f32>(albedo.y * inv),
            static_cast<f32>(albedo.z * inv)
        },
        .roughness =
            static_cast<f32>(
                roughness * inv),
        .oceanFraction =
            static_cast<f32>(
                ocean * inv),
        .iceFraction =
            static_cast<f32>(
                ice * inv),
        .emissionLinear = {
            static_cast<f32>(emission.x * inv),
            static_cast<f32>(emission.y * inv),
            static_cast<f32>(emission.z * inv)
        }
    };
}

CachedDiscProduct BuildCachedDisc(
    const celestial_appearance::PlanetaryAppearanceProduct& appearance,
    const CachedDiscConfig& config)
{
    if (config.resolution < 8U ||
        appearance.texels.empty())
    {
        throw std::invalid_argument(
            "Cached disc input is invalid.");
    }

    CachedDiscProduct result;
    result.resolution =
        config.resolution;
    result.appearanceFingerprint =
        appearance.fingerprint;
    result.fingerprint =
        DiscFingerprint(
            appearance,
            config);
    result.rgba8.resize(
        static_cast<std::size_t>(
            config.resolution) *
        config.resolution *
        4U,
        0U);

    const math::Double3 light =
        math::Normalize(
            math::Double3{
                0.55, 0.72, 0.48});

    for (u32 y = 0;
         y < config.resolution;
         ++y)
    {
        for (u32 x = 0;
             x < config.resolution;
             ++x)
        {
            const f64 px =
                (2.0 *
                 (static_cast<f64>(x) + 0.5) /
                 static_cast<f64>(config.resolution)) -
                1.0;
            const f64 py =
                1.0 -
                (2.0 *
                 (static_cast<f64>(y) + 0.5) /
                 static_cast<f64>(config.resolution));

            const f64 r2 =
                px * px + py * py;

            if (r2 > 1.0)
            {
                continue;
            }

            const auto direction =
                DiscDirection(px, py);

            const auto& sample =
                SampleAppearance(
                    appearance,
                    direction);

            const f32 ndl =
                static_cast<f32>(
                    std::clamp(
                        math::Dot(
                            direction,
                            light),
                        0.0,
                        1.0));

            math::Float3 color =
                sample.albedoLinear *
                    (0.05F +
                     0.95F * ndl) +
                sample.emissionLinear;

            const std::size_t offset =
                (static_cast<std::size_t>(y) *
                     config.resolution +
                 x) *
                4U;

            result.rgba8[offset] =
                ToLinearUnorm8(color.x);
            result.rgba8[offset + 1U] =
                ToLinearUnorm8(color.y);
            result.rgba8[offset + 2U] =
                ToLinearUnorm8(color.z);

            const f64 edge =
                std::clamp(
                    (1.0 - r2) *
                        static_cast<f64>(
                            config.resolution) *
                        0.5,
                    0.0,
                    1.0);

            result.rgba8[offset + 3U] =
                static_cast<u8>(
                    std::lround(
                        edge * 255.0));
        }
    }

    return result;
}

GpuCachedDiscProduct::GpuCachedDiscProduct(
    rhi::Device& device,
    const CachedDiscProduct& product)
    : fingerprint_(product.fingerprint)
{
    if (product.resolution == 0U ||
        product.rgba8.size() !=
            static_cast<std::size_t>(
                product.resolution) *
                product.resolution *
                4U)
    {
        throw std::invalid_argument(
            "GPU cached disc product is invalid.");
    }

    staging_ =
        device.CreateBuffer({
            .sizeBytes =
                static_cast<u64>(
                    product.rgba8.size()),
            .usage =
                rhi::BufferUsage::Generic,
            .memory =
                rhi::MemoryUsage::HostVisible,
            .initialState =
                rhi::ResourceState::CopySource
        });

    texture_ =
        device.CreateTexture({
            .width = product.resolution,
            .height = product.resolution,
            .format =
                rhi::TextureFormat::RGBA8_UNorm,
            .initialState =
                rhi::ResourceState::
                    CopyDestination
        });

    if (!staging_ || !texture_)
    {
        throw std::runtime_error(
            "Failed to allocate cached disc GPU resources.");
    }

    std::memcpy(
        staging_->Map(),
        product.rgba8.data(),
        product.rgba8.size());
    staging_->Unmap();
}

void GpuCachedDiscProduct::EnsureUploaded(
    rhi::CommandList& commands)
{
    if (uploaded_)
    {
        return;
    }

    commands.CopyBufferToTexture(
        *staging_,
        0,
        *texture_);

    commands.Transition(
        *texture_,
        rhi::ResourceState::CopyDestination,
        rhi::ResourceState::ShaderResource);

    uploaded_ = true;
}

rhi::Texture&
GpuCachedDiscProduct::Texture() noexcept
{
    return *texture_;
}

u64 GpuCachedDiscProduct::Fingerprint() const noexcept
{
    return fingerprint_;
}

FarBodyRenderer::FarBodyRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler)
{
    const auto vs =
        compiler.Compile({
            .source = kQuadVs,
            .entryPoint = "main",
            .stage = shader::Stage::Vertex,
            .debug = false
        });

    const auto analyticPs =
        compiler.Compile({
            .source = kAnalyticPs,
            .entryPoint = "main",
            .stage = shader::Stage::Pixel,
            .debug = false
        });

    const auto cachedPs =
        compiler.Compile({
            .source = kCachedPs,
            .entryPoint = "main",
            .stage = shader::Stage::Pixel,
            .debug = false
        });

    analyticPipeline_ =
        device.CreateGraphicsPipeline({
            .vertexShader = {
                .data = vs.bytecode.data(),
                .size = vs.bytecode.size()
            },
            .pixelShader = {
                .data =
                    analyticPs.bytecode.data(),
                .size =
                    analyticPs.bytecode.size()
            },
            .vertexAttributes = {},
            .vertexStrideBytes = 0,
            .pushConstantDwords = 32,
            .topology =
                rhi::PrimitiveTopology::
                    TriangleList,
            .fillMode = rhi::FillMode::Solid,
            .cullMode = rhi::CullMode::None,
            .blendMode = rhi::BlendMode::Alpha,
            .depthTest = false,
            .depthWrite = false
        });

    cachedPipeline_ =
        device.CreateGraphicsPipeline({
            .vertexShader = {
                .data = vs.bytecode.data(),
                .size = vs.bytecode.size()
            },
            .pixelShader = {
                .data =
                    cachedPs.bytecode.data(),
                .size =
                    cachedPs.bytecode.size()
            },
            .vertexAttributes = {},
            .vertexStrideBytes = 0,
            .pushConstantDwords = 4,
            .sampledTextures = 1,
            .topology =
                rhi::PrimitiveTopology::
                    TriangleList,
            .fillMode = rhi::FillMode::Solid,
            .cullMode = rhi::CullMode::None,
            .blendMode = rhi::BlendMode::Alpha,
            .depthTest = false,
            .depthWrite = false
        });
}

void FarBodyRenderer::Draw(
    rhi::CommandList& commands,
    rhi::Texture& target,
    const u32 width,
    const u32 height,
    const FarBodyDraw& draw,
    GpuCachedDiscProduct* cachedDisc)
{
    if (width == 0U ||
        height == 0U ||
        draw.opacity <= 0.0F)
    {
        return;
    }

    const auto ellipsoid =
        AsEllipsoid(draw.shape);

    const f64 scale =
        std::max({
            ellipsoid.radiiMeters.x,
            ellipsoid.radiiMeters.y,
            ellipsoid.radiiMeters.z,
            1.0
        });

    const auto bits =
        [](const f32 value)
        {
            return std::bit_cast<u32>(value);
        };

    const f64 minimumRasterRadiusPixels =
        0.5;
    const f64 rasterRadiusPixels =
        std::max(
            draw.projectedRadiusPixels,
            minimumRasterRadiusPixels);

    const f32 radiusNdc =
        static_cast<f32>(
            2.0 *
            rasterRadiusPixels /
            static_cast<f64>(
                std::max(
                    height,
                    1U)));

    const f32 pointFluxScale =
        static_cast<f32>(
            std::clamp(
                (draw.projectedRadiusPixels *
                 draw.projectedRadiusPixels) /
                    (rasterRadiusPixels *
                     rasterRadiusPixels),
                0.0,
                1.0));

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

    if (draw.representation ==
            celestial_representation::
                Representation::
                    CachedDiscImpostor &&
        cachedDisc != nullptr)
    {
        cachedDisc->EnsureUploaded(
            commands);

        const std::array<u32, 4>
            constants{
                bits(radiusNdc),
                bits(std::clamp(
                    draw.opacity,
                    0.0F,
                    1.0F)),
                0U,
                0U
            };

        commands.SetGraphicsPipeline(
            *cachedPipeline_);
        commands.SetGraphicsConstants(
            constants);
        commands.SetGraphicsTexture(
            0,
            cachedDisc->Texture());
        commands.Draw(6);
        return;
    }

    u32 mode = 0U;

    switch (draw.representation)
    {
    case celestial_representation::
        Representation::SmoothGlobe:
        mode = 0U;
        break;
    case celestial_representation::
        Representation::
            AnalyticDiscImpostor:
    case celestial_representation::
        Representation::
            CachedDiscImpostor:
        mode = 1U;
        break;
    case celestial_representation::
        Representation::PointProxy:
        mode = 2U;
        break;
    case celestial_representation::
        Representation::StellarPointProxy:
        mode = 3U;
        break;
    default:
        mode = 0U;
        break;
    }

    const f32 tanHalfFov =
        std::tan(
            draw.camera.verticalFovRadians *
            0.5F);

    const std::array<u32, 32>
        constants{
            bits(static_cast<f32>(
                ellipsoid.radiiMeters.x /
                scale)),
            bits(static_cast<f32>(
                ellipsoid.radiiMeters.y /
                scale)),
            bits(static_cast<f32>(
                ellipsoid.radiiMeters.z /
                scale)),
            bits(static_cast<f32>(width) /
                 static_cast<f32>(height)),

            bits(static_cast<f32>(
                draw.camera.localPositionMeters.x /
                scale)),
            bits(static_cast<f32>(
                draw.camera.localPositionMeters.y /
                scale)),
            bits(static_cast<f32>(
                draw.camera.localPositionMeters.z /
                scale)),
            bits(tanHalfFov),

            bits(draw.camera.forward.x),
            bits(draw.camera.forward.y),
            bits(draw.camera.forward.z),
            0U,

            bits(draw.camera.up.x),
            bits(draw.camera.up.y),
            bits(draw.camera.up.z),
            0U,

            bits(draw.appearance.albedoLinear.x),
            bits(draw.appearance.albedoLinear.y),
            bits(draw.appearance.albedoLinear.z),
            bits(draw.appearance.roughness),

            bits(draw.appearance.oceanFraction),
            bits(draw.appearance.iceFraction),
            bits(draw.stellar ? 1.0F : 0.0F),
            0U,

            bits(draw.appearance.emissionLinear.x),
            bits(draw.appearance.emissionLinear.y),
            bits(draw.appearance.emissionLinear.z),
            bits(std::clamp(
                draw.opacity,
                0.0F,
                1.0F)),

            bits(static_cast<f32>(mode)),
            bits(radiusNdc),
            bits(pointFluxScale),
            bits(std::max(
                draw.radiometricIntensity,
                0.0F))
        };

    commands.SetGraphicsPipeline(
        *analyticPipeline_);
    commands.SetGraphicsConstants(
        constants);
    commands.Draw(6);
}
} // namespace orbit::celestial_far_render
