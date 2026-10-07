#include <orbit/mesh_render/MeshShadow.hpp>

#include "MeshDrawRecords.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <string>

namespace orbit::mesh_render
{
namespace
{
constexpr u64 kRecordRetireTicks = 24U;
constexpr u32 kShadowPushDwords = 16U;
constexpr u32 kResolvePushDwords = 28U;

constexpr const char* kShadowVertexShader = R"(
[[vk::binding(0, 0)]]
StructuredBuffer<float4> g_records : register(t0);

struct Constants
{
    float4 centerRadius;  // camera-relative window centre, radius
    float4 rightRecord;   // xyz window right, w draw record index
    float4 up;            // xyz window up
    float4 toSun;         // xyz unit vector toward the sun
};
[[vk::push_constant]] Constants g;

struct VSInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(3)]] float2 uv : TEXCOORD0;
};

struct VSOutput
{
    float4 position : SV_Position;
    float depth : TEXCOORD0;
    float2 uv : TEXCOORD1;
};

VSOutput main(VSInput input)
{
    const uint record = (uint)g.rightRecord.w * 6u;
    const float4 homogeneous = float4(input.position, 1.0);
    const float3 p = float3(
        dot(g_records[record + 0u], homogeneous),
        dot(g_records[record + 1u], homogeneous),
        dot(g_records[record + 2u], homogeneous));

    const float3 q = p - g.centerRadius.xyz;
    const float radius = g.centerRadius.w;
    // Depth grows away from the sun, 0 at the sun-side face of the window.
    const float depth = (radius - dot(q, g.toSun.xyz)) / (2.0 * radius);

    VSOutput output;
    output.position = float4(
        dot(q, g.rightRecord.xyz) / radius,
        dot(q, g.up.xyz) / radius,
        saturate(depth),
        1.0);
    output.depth = depth;
    output.uv = input.uv;
    return output;
}
)";

constexpr const char* kShadowPixelShader = R"(
[[vk::binding(0, 0)]]
StructuredBuffer<float4> g_records : register(t0);

[[vk::binding(1, 0)]] [[vk::combinedImageSampler]] Texture2D g_baseColor;
[[vk::binding(1, 0)]] [[vk::combinedImageSampler]] SamplerState g_baseColorSampler;

struct Constants
{
    float4 centerRadius;
    float4 rightRecord;
    float4 up;
    float4 toSun;
};
[[vk::push_constant]] Constants g;

struct VSOutput
{
    float4 position : SV_Position;
    float depth : TEXCOORD0;
    float2 uv : TEXCOORD1;
};

float main(VSOutput input) : SV_Target0
{
    const uint record = (uint)g.rightRecord.w * 6u;
    const float cutoff = g_records[record + 5u].z;
    if (cutoff > 0.0)
    {
        const float alpha =
            g_baseColor.Sample(g_baseColorSampler, input.uv).a *
            g_records[record + 3u].a;
        if (alpha < cutoff)
        {
            discard;
        }
    }
    return input.depth;
}
)";

constexpr const char* kResolveShader = R"(
[[vk::binding(0, 0)]]
StructuredBuffer<float4> g_params : register(t0);

[[vk::binding(1, 0)]]
RWTexture2D<float4> g_shadow : register(u1);

[[vk::binding(2, 0)]] [[vk::combinedImageSampler]] Texture2D g_normalMetallic;
[[vk::binding(2, 0)]] [[vk::combinedImageSampler]] SamplerState g_normalSampler;
[[vk::binding(3, 0)]] [[vk::combinedImageSampler]] Texture2D g_emissionClass;
[[vk::binding(3, 0)]] [[vk::combinedImageSampler]] SamplerState g_emissionSampler;
[[vk::binding(4, 0)]] [[vk::combinedImageSampler]] Texture2D g_depth;
[[vk::binding(4, 0)]] [[vk::combinedImageSampler]] SamplerState g_depthSampler;
[[vk::binding(5, 0)]] [[vk::combinedImageSampler]] Texture2D g_sunMap;
[[vk::binding(5, 0)]] [[vk::combinedImageSampler]] SamplerState g_sunSampler;
[[vk::binding(6, 0)]] [[vk::combinedImageSampler]] Texture2D g_skyMap;
[[vk::binding(6, 0)]] [[vk::combinedImageSampler]] SamplerState g_skySampler;

struct Constants
{
    uint width;
    uint height;
    uint initialize;
    uint mapSize;

    float4 forwardAspect;
    float4 upTanHalfFov;
    float4 toSunNear;
    float4 rightFar;
    float4 upRadius;
    float4 center;
};
[[vk::push_constant]] Constants g;

static const uint kSkyDirections = 16u;
static const uint kSkyTile = 256u;

float Hash12(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float ReverseZViewDepth(float depth)
{
    const float nearPlane = max(g.toSunNear.w, 1.0e-5);
    const float farPlane = max(g.rightFar.w, nearPlane + 1.0e-4);
    return nearPlane * farPlane /
        max(depth * (farPlane - nearPlane) + nearPlane, 1.0e-6);
}

float3 ReconstructPosition(float2 uv, float depth)
{
    const float3 forward = normalize(g.forwardAspect.xyz);
    const float3 requestedUp = normalize(g.upTanHalfFov.xyz);
    const float3 right = normalize(cross(forward, requestedUp));
    const float3 up = normalize(cross(right, forward));

    const float aspect = max(g.forwardAspect.w, 0.001);
    const float tanHalfFov = max(g.upTanHalfFov.w, 0.001);
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);

    const float3 ray = normalize(
        forward +
        right * (ndc.x * aspect * tanHalfFov) +
        up * (ndc.y * tanHalfFov));

    return ray * (ReverseZViewDepth(depth) / max(dot(ray, forward), 1.0e-5));
}

float SunDepthAt(int2 texel)
{
    if (texel.x < 0 || texel.y < 0 ||
        texel.x >= (int)g.mapSize || texel.y >= (int)g.mapSize)
    {
        return 1.0e9;
    }
    return g_sunMap.Load(int3(texel, 0)).r;
}

float2 VogelDisk(uint index, uint count, float rotation)
{
    const float radius = sqrt((float(index) + 0.5) / float(count));
    const float angle = float(index) * 2.39996323 + rotation;
    return radius * float2(cos(angle), sin(angle));
}

// Percentage-closer soft shadows: the sun is a disc, so the penumbra grows with
// the distance between occluder and receiver. 1) average the depth of the
// blockers around the receiver, 2) turn (receiver - blocker) distance into a
// penumbra width with the sun's angular size, 3) filter that wide. Contact
// points stay sharp, far shadows soften.
float SunShadowPcss(float2 t, float receiverDepth, float2 pixelSeed)
{
    const float radius = g.upRadius.w;
    const float depthToMeters = 2.0 * radius;
    const float texelMeters = depthToMeters / float(g.mapSize);
    const float tanSun = g.center.w > 0.0 ? g.center.w : 0.00465;
    const float bias = 1.5 / float(g.mapSize);
    const float rotation = Hash12(pixelSeed + 7.0) * 6.28318530718;

    // Widest penumbra this receiver could have (blocker at the sun-side face).
    const float searchRadius = clamp(
        max(receiverDepth, 0.0) * depthToMeters * tanSun / texelMeters,
        2.0, 24.0);

    float blockerSum = 0.0;
    float blockerCount = 0.0;

    [unroll]
    for (uint i = 0u; i < 16u; ++i)
    {
        const float stored = SunDepthAt(
            int2(floor(t + VogelDisk(i, 16u, rotation) * searchRadius)));
        if (stored < receiverDepth - bias)
        {
            blockerSum += stored;
            blockerCount += 1.0;
        }
    }

    if (blockerCount < 0.5)
    {
        return 1.0;
    }

    const float separationMeters =
        (receiverDepth - blockerSum / blockerCount) * depthToMeters;
    const float filterRadius = clamp(
        separationMeters * tanSun / texelMeters, 0.9, 24.0);

    float lit = 0.0;
    [unroll]
    for (uint j = 0u; j < 24u; ++j)
    {
        const float stored = SunDepthAt(
            int2(floor(t + VogelDisk(j, 24u, rotation + 1.7) * filterRadius)));
        lit += stored >= receiverDepth - bias ? 1.0 : 0.0;
    }
    return lit / 24.0;
}

// Same for one tile of the sky atlas (`origin` = tile corner in texels).
float CompareSky(int2 origin, float2 t, float receiverDepth)
{
    const float2 base = floor(t - 0.5);
    const float2 f = frac(t - 0.5);
    float lit[4];
    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        const int2 texel = int2(base) + int2(i & 1, i >> 1);
        float stored = 1.0e9;
        if (texel.x >= 0 && texel.y >= 0 &&
            texel.x < (int)kSkyTile && texel.y < (int)kSkyTile)
        {
            stored = g_skyMap.Load(int3(origin + texel, 0)).r;
        }
        lit[i] = stored >= receiverDepth ? 1.0 : 0.0;
    }
    return lerp(
        lerp(lit[0], lit[1], f.x),
        lerp(lit[2], lit[3], f.x),
        f.y);
}

// Must match MeshSkyDirection() in MeshShadow.cpp.
void SkyDirection(uint k, float3 up, out float3 L, out float3 R, out float3 U)
{
    L = up;
    if (k > 0u)
    {
        const float3 ref = abs(up.y) < 0.95 ? float3(0.0, 1.0, 0.0)
                                            : float3(1.0, 0.0, 0.0);
        const float3 e1 = normalize(cross(ref, up));
        const float3 e2 = cross(up, e1);
        // Ring A: 5 directions at 72 degrees elevation; ring B: 10 at 36.
        const bool ringA = k <= 5u;
        const float phi = ringA
            ? float(k - 1u) * 1.2566370614
            : (float(k - 6u) + 0.5) * 0.6283185307;
        const float elevation = ringA ? 1.2566370614 : 0.6283185307;
        L = normalize(
            cos(elevation) * (cos(phi) * e1 + sin(phi) * e2) +
            sin(elevation) * up);
    }
    const float3 refK = abs(L.y) < 0.95 ? float3(0.0, 1.0, 0.0)
                                        : float3(1.0, 0.0, 0.0);
    R = normalize(cross(refK, L));
    U = cross(L, R);
}

// Fraction of the cosine-weighted hemisphere that is above the local horizon
// and not blocked by the meshes: seven sky directions weighted by the
// surface cosine, each mirrored below the horizon as blocked ground.
float SkyOpenness(float3 position, float3 normal, float3 up, float2 pixelSeed)
{
    const float radius = g.upRadius.w;
    const float texel = 2.0 * radius / float(kSkyTile);
    const float3 origin = position + normal * (texel * 1.5);

    // Two taps per direction, offset by a per-pixel rotated vector a couple
    // of texels long, so each direction's hard edge becomes a soft gradient.
    const float spin = Hash12(pixelSeed) * 6.28318530718;
    const float2 tapA = 1.75 * float2(cos(spin), sin(spin));

    float total = 0.0;
    float open = 0.0;

    for (uint k = 0u; k < kSkyDirections; ++k)
    {
        float3 L, R, U;
        SkyDirection(k, up, L, R, U);

        const float weight = max(dot(normal, L), 0.0);
        const float3 mirrored = L - 2.0 * dot(L, up) * up;
        total += weight + max(dot(normal, mirrored), 0.0);

        if (weight <= 0.0)
        {
            continue;
        }

        float visibility = 1.0;
        const float3 q = origin - g.center.xyz;
        const float2 window = float2(dot(q, R), dot(q, U)) / radius;

        if (abs(window.x) < 1.0 && abs(window.y) < 1.0)
        {
            const float receiverDepth =
                (radius - dot(q, L)) / (2.0 * radius) -
                2.0 * texel / (2.0 * radius);
            const float2 t = float2(
                window.x * 0.5 + 0.5,
                0.5 - window.y * 0.5) * float(kSkyTile);
            const int2 tile = int2(
                int(k & 3u) * (int)kSkyTile,
                int(k >> 2u) * (int)kSkyTile);
            visibility = 0.5 * (
                CompareSky(tile, t + tapA, receiverDepth) +
                CompareSky(tile, t - tapA, receiverDepth));
        }

        open += weight * visibility;
    }

    return total > 0.0 ? open / total : 0.0;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g.width || id.y >= g.height)
    {
        return;
    }

    const uint2 pixel = id.xy;
    const float2 uv = (float2(pixel) + 0.5) / float2(g.width, g.height);
    const float depth = g_depth.SampleLevel(g_depthSampler, uv, 0).r;

    float visibility = 1.0;
    float3 skyFill = 0.0;
    bool meshPixel = false;

    if (depth > 0.0)
    {
        const float3 normal = normalize(
            g_normalMetallic.SampleLevel(g_normalSampler, uv, 0).xyz);
        const float3 toSun = normalize(g.toSunNear.xyz);
        const float3 position = ReconstructPosition(uv, depth);

        const float meta =
            g_emissionClass.SampleLevel(g_emissionSampler, uv, 0).a;
        meshPixel =
            floor(meta + 0.01) == 3.0 &&
            round(frac(meta + 0.01) * 16.0) == 8.0;

        if (meshPixel)
        {
            const float4 skyParameters = g_params[0];
            if (skyParameters.w > 0.0)
            {
                skyFill =
                    skyParameters.rgb *
                    SkyOpenness(
                        position, normal, normalize(g_params[1].xyz),
                        float2(pixel));
            }
        }

        if (dot(normal, toSun) > 0.0)
        {
            const float radius = g.upRadius.w;
            const float texel = 2.0 * radius / float(g.mapSize);

            // Offset along the normal and toward the sun by a couple of
            // texels (plus a view-distance term for far receivers).
            const float cosine = saturate(dot(normal, toSun));
            const float3 origin =
                position +
                normal * (texel * (1.5 + 2.0 * sqrt(1.0 - cosine * cosine)) +
                          ReverseZViewDepth(depth) * 1.0e-4) +
                toSun * texel * 1.0;

            const float3 q = origin - g.center.xyz;
            const float2 window = float2(
                dot(q, g.rightFar.xyz), dot(q, g.upRadius.xyz)) / radius;

            if (abs(window.x) < 1.0 && abs(window.y) < 1.0)
            {
                const float receiverDepth =
                    (radius - dot(q, toSun)) / (2.0 * radius);
                const float2 t = float2(
                    window.x * 0.5 + 0.5,
                    0.5 - window.y * 0.5) * float(g.mapSize);

                visibility = SunShadowPcss(t, receiverDepth, float2(pixel));
            }
        }
    }

    float4 value = float4(visibility, 0.0, 0.0, 0.0);
    if (g.initialize == 0u)
    {
        value = g_shadow[pixel];
        value.x = min(value.x, visibility);
    }
    if (meshPixel)
    {
        value.yzw = skyFill;
    }
    g_shadow[pixel] = value;
}
)";

using Vec3 = std::array<f64, 3>;

[[nodiscard]] f64 Dot(const Vec3& a, const Vec3& b) noexcept
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

[[nodiscard]] Vec3 Cross(const Vec3& a, const Vec3& b) noexcept
{
    return {
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0]};
}

[[nodiscard]] bool Normalize(Vec3& v) noexcept
{
    const f64 length = std::sqrt(Dot(v, v));
    if (!(length > 1.0e-12) || !std::isfinite(length))
    {
        return false;
    }
    v = {v[0] / length, v[1] / length, v[2] / length};
    return true;
}

[[nodiscard]] u32 AsBits(const f32 value) noexcept
{
    return std::bit_cast<u32>(value);
}
} // namespace

std::optional<MeshShadowFrame> BuildMeshShadowFrame(
    const std::span<const MeshInstance> instances,
    const math::Float3& directionToSun,
    const math::Double3& cameraPositionInFrameMeters,
    const u32 mapSize)
{
    // Union of the instances' bounding spheres (camera-relative).
    bool any = false;
    Vec3 center{};
    f64 radius = 0.0;

    for (const MeshInstance& instance : instances)
    {
        if (instance.model == nullptr ||
            instance.model->VertexBuffer() == nullptr)
        {
            continue;
        }

        const auto& lo = instance.model->BoundsMin();
        const auto& hi = instance.model->BoundsMax();
        const Vec3 mid{
            0.5 * (lo[0] + hi[0]),
            0.5 * (lo[1] + hi[1]),
            0.5 * (lo[2] + hi[2])};
        const f64 modelRadius = 0.5 * std::sqrt(
            (hi[0] - lo[0]) * (hi[0] - lo[0]) +
            (hi[1] - lo[1]) * (hi[1] - lo[1]) +
            (hi[2] - lo[2]) * (hi[2] - lo[2]));

        const auto& r = instance.rows;
        const f64 scale = std::sqrt(
            static_cast<f64>(r[0]) * r[0] +
            static_cast<f64>(r[4]) * r[4] +
            static_cast<f64>(r[8]) * r[8]);
        const Vec3 c{
            r[0] * mid[0] + r[1] * mid[1] + r[2] * mid[2] + r[3],
            r[4] * mid[0] + r[5] * mid[1] + r[6] * mid[2] + r[7],
            r[8] * mid[0] + r[9] * mid[1] + r[10] * mid[2] + r[11]};
        const f64 rr = modelRadius * scale;

        if (!any)
        {
            center = c;
            radius = rr;
            any = true;
            continue;
        }

        // Smallest sphere containing both.
        const Vec3 d{c[0] - center[0], c[1] - center[1], c[2] - center[2]};
        const f64 distance = std::sqrt(Dot(d, d));
        if (distance + rr <= radius)
        {
            continue;
        }
        if (distance + radius <= rr)
        {
            center = c;
            radius = rr;
            continue;
        }
        const f64 merged = 0.5 * (distance + radius + rr);
        const f64 shift = merged - radius;
        for (std::size_t i = 0U; i < 3U; ++i)
        {
            center[i] += d[i] / distance * shift;
        }
        radius = merged;
    }

    if (!any || !(radius > 0.0) || !std::isfinite(radius))
    {
        return std::nullopt;
    }

    Vec3 toSun{directionToSun.x, directionToSun.y, directionToSun.z};
    if (!Normalize(toSun))
    {
        return std::nullopt;
    }

    // Orthonormal (right, up, toSun).
    Vec3 reference{0.0, 1.0, 0.0};
    if (std::abs(Dot(reference, toSun)) > 0.95)
    {
        reference = {1.0, 0.0, 0.0};
    }
    Vec3 right = Cross(reference, toSun);
    if (!Normalize(right))
    {
        return std::nullopt;
    }
    const Vec3 up = Cross(toSun, right);

    // Snap the window centre to whole texels measured in the camera-
    // independent frame, then pad so snapping cannot expose a caster.
    const f64 texel = 2.0 * radius / static_cast<f64>(mapSize);
    const Vec3 camera{
        cameraPositionInFrameMeters.x,
        cameraPositionInFrameMeters.y,
        cameraPositionInFrameMeters.z};
    const Vec3 frameCenter{
        camera[0] + center[0], camera[1] + center[1], camera[2] + center[2]};
    const std::array<const Vec3*, 2> axes{&right, &up};
    for (const Vec3* axis : axes)
    {
        const f64 along = Dot(frameCenter, *axis);
        const f64 shift = std::round(along / texel) * texel - along;
        for (std::size_t i = 0U; i < 3U; ++i)
        {
            center[i] += (*axis)[i] * shift;
        }
    }
    radius += 4.0 * texel;

    const auto f = [](const f64 v) { return static_cast<f32>(v); };
    MeshShadowFrame frame;
    frame.center = {f(center[0]), f(center[1]), f(center[2])};
    frame.radius = f(radius);
    frame.right = {f(right[0]), f(right[1]), f(right[2])};
    frame.up = {f(up[0]), f(up[1]), f(up[2])};
    frame.toSun = {f(toSun[0]), f(toSun[1]), f(toSun[2])};
    frame.mapSize = mapSize;
    return frame;
}

MeshSkyView MeshSkyDirection(
    const u32 index,
    const std::array<f32, 3>& localUp)
{
    // Must match SkyDirection() in the resolve shader.
    const Vec3 up{localUp[0], localUp[1], localUp[2]};
    Vec3 toSky = up;

    if (index > 0U)
    {
        const Vec3 reference = std::abs(up[1]) < 0.95 ? Vec3{0.0, 1.0, 0.0}
                                                      : Vec3{1.0, 0.0, 0.0};
        Vec3 e1 = Cross(reference, up);
        static_cast<void>(Normalize(e1));
        const Vec3 e2 = Cross(up, e1);
        const bool ringA = index <= 5U;
        const f64 phi = ringA
            ? static_cast<f64>(index - 1U) * 1.2566370614
            : (static_cast<f64>(index - 6U) + 0.5) * 0.6283185307;
        const f64 kElevation = ringA ? 1.2566370614 : 0.6283185307;
        for (std::size_t i = 0U; i < 3U; ++i)
        {
            toSky[i] = std::cos(kElevation) *
                           (std::cos(phi) * e1[i] + std::sin(phi) * e2[i]) +
                       std::sin(kElevation) * up[i];
        }
        static_cast<void>(Normalize(toSky));
    }

    const Vec3 referenceK = std::abs(toSky[1]) < 0.95 ? Vec3{0.0, 1.0, 0.0}
                                                      : Vec3{1.0, 0.0, 0.0};
    Vec3 right = Cross(referenceK, toSky);
    static_cast<void>(Normalize(right));
    const Vec3 windowUp = Cross(toSky, right);

    const auto f = [](const f64 v) { return static_cast<f32>(v); };
    return {
        {f(toSky[0]), f(toSky[1]), f(toSky[2])},
        {f(right[0]), f(right[1]), f(right[2])},
        {f(windowUp[0]), f(windowUp[1]), f(windowUp[2])}};
}

std::array<f32, 3> MeshLocalUp(
    const MeshShadowFrame& frame,
    const math::Double3& cameraPositionInFrameMeters)
{
    Vec3 radial{
        cameraPositionInFrameMeters.x + frame.center[0],
        cameraPositionInFrameMeters.y + frame.center[1],
        cameraPositionInFrameMeters.z + frame.center[2]};
    if (!Normalize(radial))
    {
        return {0.0F, 1.0F, 0.0F};
    }
    return {
        static_cast<f32>(radial[0]),
        static_cast<f32>(radial[1]),
        static_cast<f32>(radial[2])};
}

MeshShadowMapRenderer::MeshShadowMapRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler)
    : device_(device)
{
    const auto vs = compiler.Compile({
        .source = kShadowVertexShader,
        .entryPoint = "main",
        .stage = shader::Stage::Vertex,
        .debug = false});
    const auto ps = compiler.Compile({
        .source = kShadowPixelShader,
        .entryPoint = "main",
        .stage = shader::Stage::Pixel,
        .debug = false});

    static constexpr std::array<rhi::VertexAttribute, 2> attributes{{
        {0U, rhi::VertexFormat::Float3,
         offsetof(mesh_import::MeshVertex, position)},
        {3U, rhi::VertexFormat::Float2,
         offsetof(mesh_import::MeshVertex, uv)}}};

    pipeline_ = device.CreateGraphicsPipeline({
        .vertexShader = {.data = vs.bytecode.data(), .size = vs.bytecode.size()},
        .pixelShader = {.data = ps.bytecode.data(), .size = ps.bytecode.size()},
        .vertexAttributes = attributes,
        .vertexStrideBytes = sizeof(mesh_import::MeshVertex),
        .pushConstantDwords = kShadowPushDwords,
        .shaderResourceBuffers = 1U,
        .sampledTextures = 1U,
        .topology = rhi::PrimitiveTopology::TriangleList,
        .fillMode = rhi::FillMode::Solid,
        .cullMode = rhi::CullMode::None,
        .blendMode = rhi::BlendMode::Opaque,
        .depthCompare = rhi::DepthCompare::LessEqual,
        .depthTest = true,
        .depthWrite = true,
        .colorAttachmentFormats = {
            rhi::TextureFormat::R32_Float,
            rhi::TextureFormat::RGBA8_UNorm,
            rhi::TextureFormat::RGBA8_UNorm,
            rhi::TextureFormat::RGBA8_UNorm},
        .colorAttachmentCount = 1U});
}

void MeshShadowMapRenderer::DrawViews(
    rhi::CommandList& commands,
    const MeshLibrary& library,
    const std::span<const MeshInstance> instances,
    const std::span<const View> views,
    rhi::Texture& colorMap,
    rhi::Texture& depthMap,
    const u32 width,
    const u32 height)
{
    ++tick_;
    std::erase_if(
        records_,
        [this](const RetiredRecords& records)
        {
            return tick_ >= records.retireAtTick;
        });

    if (pipeline_ == nullptr)
    {
        return;
    }

    std::vector<detail::DrawCall> draws;
    std::vector<f32> records;
    detail::BuildDrawRecords(instances, 0.0F, draws, records);

    // Always clear so a map with no casters reads as fully lit. (Far beyond
    // any receiver depth, which can exceed 1 behind the window.)
    commands.ClearColorTarget(colorMap, {1.0e9F, 1.0e9F, 1.0e9F, 1.0e9F});
    commands.ClearDepthTarget(depthMap, 1.0F);
    commands.SetRenderTargets(colorMap, depthMap);

    if (draws.empty())
    {
        return;
    }

    auto buffer = device_.CreateBuffer({
        .sizeBytes = records.size() * sizeof(f32),
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::ShaderResource});
    std::memcpy(buffer->Map(), records.data(), records.size() * sizeof(f32));
    buffer->Unmap();

    commands.SetScissor({
        .left = 0,
        .top = 0,
        .right = static_cast<i32>(width),
        .bottom = static_cast<i32>(height)});
    commands.SetGraphicsPipeline(*pipeline_);

    for (const View& view : views)
    {
        commands.SetViewport({
            .x = static_cast<f32>(view.originX),
            .y = static_cast<f32>(view.originY),
            .width = static_cast<f32>(view.size),
            .height = static_cast<f32>(view.size),
            .minDepth = 0.0F,
            .maxDepth = 1.0F});

        for (const detail::DrawCall& draw : draws)
        {
            const std::array<u32, kShadowPushDwords> constants{
                AsBits(view.center[0]), AsBits(view.center[1]),
                AsBits(view.center[2]), AsBits(view.radius),
                AsBits(view.right[0]), AsBits(view.right[1]),
                AsBits(view.right[2]),
                AsBits(static_cast<f32>(draw.record)),
                AsBits(view.up[0]), AsBits(view.up[1]), AsBits(view.up[2]),
                0U,
                AsBits(view.toSun[0]), AsBits(view.toSun[1]),
                AsBits(view.toSun[2]), 0U};

            const auto& model = *draw.model;
            const auto& material = model.Materials()[draw.part->material];
            rhi::Texture* base = model.Texture(
                material.texture[static_cast<std::size_t>(
                    MeshTextureRole::BaseColor)]);

            commands.SetGraphicsConstants(constants);
            commands.SetGraphicsBuffer(0U, *buffer);
            // Opaque parts never read the texture; any bound texture will do.
            commands.SetGraphicsTexture(
                0U,
                base != nullptr
                    ? *base
                    : library.DefaultTexture(MeshTextureRole::BaseColor));
            commands.SetVertexBuffer(
                *model.VertexBuffer(), sizeof(mesh_import::MeshVertex));
            commands.SetIndexBuffer(
                *model.IndexBuffer(), rhi::IndexFormat::UInt32);
            commands.DrawIndexed(
                draw.part->indexCount, draw.part->firstIndex, 0);
        }
    }

    records_.push_back({std::move(buffer), tick_ + kRecordRetireTicks});
}

void MeshShadowMapRenderer::Draw(
    rhi::CommandList& commands,
    const MeshLibrary& library,
    const std::span<const MeshInstance> instances,
    const MeshShadowFrame& frame,
    rhi::Texture& colorMap,
    rhi::Texture& depthMap)
{
    const View view{
        .center = frame.center,
        .radius = frame.radius,
        .right = frame.right,
        .up = frame.up,
        .toSun = frame.toSun,
        .originX = 0U,
        .originY = 0U,
        .size = frame.mapSize};
    DrawViews(
        commands, library, instances, std::span<const View>(&view, 1U),
        colorMap, depthMap, frame.mapSize, frame.mapSize);
}

void MeshShadowMapRenderer::DrawSky(
    rhi::CommandList& commands,
    const MeshLibrary& library,
    const std::span<const MeshInstance> instances,
    const MeshShadowFrame& frame,
    const std::array<f32, 3>& localUp,
    rhi::Texture& colorAtlas,
    rhi::Texture& depthAtlas)
{
    std::array<View, kMeshSkyDirections> views{};
    for (u32 k = 0U; k < kMeshSkyDirections; ++k)
    {
        const MeshSkyView sky = MeshSkyDirection(k, localUp);
        views[k] = {
            .center = frame.center,
            .radius = frame.radius,
            .right = sky.right,
            .up = sky.up,
            .toSun = sky.toSky,
            .originX = (k & 3U) * kMeshSkyTileSize,
            .originY = (k >> 2U) * kMeshSkyTileSize,
            .size = kMeshSkyTileSize};
    }
    DrawViews(
        commands, library, instances, views, colorAtlas, depthAtlas,
        kMeshSkyAtlasWidth, kMeshSkyAtlasHeight);
}

MeshSunShadowRenderer::MeshSunShadowRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler)
    : device_(device)
{
    const auto compute = compiler.Compile({
        .source = kResolveShader,
        .entryPoint = "main",
        .stage = shader::Stage::Compute,
        .debug = false});

    pipeline_ = device.CreateComputePipeline({
        .computeShader = {
            .data = compute.bytecode.data(),
            .size = compute.bytecode.size()},
        .pushConstantDwords = kResolvePushDwords,
        .shaderResourceBuffers = 1U,
        .storageTextures = 1U,
        .sampledTextures = 5U});
}

void MeshSunShadowRenderer::Draw(
    rhi::CommandList& commands,
    rhi::Texture& target,
    rhi::Texture& surfaceNormalMetallic,
    rhi::Texture& surfaceEmissionClass,
    rhi::Texture& depth,
    rhi::Texture& shadowMap,
    rhi::Texture& skyAtlas,
    const u32 width,
    const u32 height,
    const lighting::LightingView& view,
    const MeshShadowFrame& frame,
    const MeshSkyFill& sky,
    const bool initialize)
{
    if (pipeline_ == nullptr || width == 0U || height == 0U)
    {
        return;
    }

    ++tick_;
    std::erase_if(
        parameters_,
        [this](const RetiredBuffer& retired)
        {
            return tick_ >= retired.retireAtTick;
        });

    const bool skyEnabled =
        sky.irradiance.x > 0.0F || sky.irradiance.y > 0.0F ||
        sky.irradiance.z > 0.0F;
    const std::array<f32, 8> parameters{
        sky.irradiance.x, sky.irradiance.y, sky.irradiance.z,
        skyEnabled ? 1.0F : 0.0F,
        sky.localUp[0], sky.localUp[1], sky.localUp[2], 0.0F};
    auto buffer = device_.CreateBuffer({
        .sizeBytes = sizeof(parameters),
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::ShaderResource});
    std::memcpy(buffer->Map(), parameters.data(), sizeof(parameters));
    buffer->Unmap();

    const f32 aspect = static_cast<f32>(width) / static_cast<f32>(height);
    const f32 nearPlane = std::max(view.nearPlaneMeters, 1.0e-5F);
    const f32 farPlane = std::max(view.farPlaneMeters, nearPlane + 1.0e-4F);

    const std::array<u32, kResolvePushDwords> constants{
        width, height, initialize ? 1U : 0U, frame.mapSize,

        AsBits(view.forward.x), AsBits(view.forward.y),
        AsBits(view.forward.z), AsBits(aspect),

        AsBits(view.up.x), AsBits(view.up.y), AsBits(view.up.z),
        AsBits(std::tan(view.verticalFovRadians * 0.5F)),

        AsBits(frame.toSun[0]), AsBits(frame.toSun[1]),
        AsBits(frame.toSun[2]), AsBits(nearPlane),

        AsBits(frame.right[0]), AsBits(frame.right[1]),
        AsBits(frame.right[2]), AsBits(farPlane),

        AsBits(frame.up[0]), AsBits(frame.up[1]), AsBits(frame.up[2]),
        AsBits(frame.radius),

        AsBits(frame.center[0]), AsBits(frame.center[1]),
        AsBits(frame.center[2]), AsBits(frame.sunTanHalfAngle)};

    commands.SetComputePipeline(*pipeline_);
    commands.SetComputeConstants(constants);
    commands.SetComputeBuffer(0U, *buffer);
    commands.SetComputeStorageTexture(0U, target);
    commands.SetComputeTexture(0U, surfaceNormalMetallic);
    commands.SetComputeTexture(1U, surfaceEmissionClass);
    commands.SetComputeTexture(2U, depth);
    commands.SetComputeTexture(3U, shadowMap);
    commands.SetComputeTexture(4U, skyAtlas);
    commands.Dispatch((width + 7U) / 8U, (height + 7U) / 8U, 1U);

    parameters_.push_back({std::move(buffer), tick_ + kRecordRetireTicks});
}
} // namespace orbit::mesh_render
