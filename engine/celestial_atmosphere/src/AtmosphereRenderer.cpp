#include <orbit/celestial_atmosphere/AtmosphereRenderer.hpp>
#include <orbit/lighting/TerrainSunShadow.hpp>
#include <orbit/lighting/SunVisibilityShader.hpp>
#include <string>

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
    float4 sunOcclusion;        // stellar angular radius, ellipsoid axes xyz (m)
    float4 audit;               // x mode, y radiance-only, zw reserved
};

[[vk::push_constant]] Constants g;

[[vk::binding(1, 0)]] [[vk::combinedImageSampler]] Texture2D g_scene;
[[vk::binding(1, 0)]] [[vk::combinedImageSampler]] SamplerState g_sceneSampler;
[[vk::binding(2, 0)]] [[vk::combinedImageSampler]] Texture2D g_depth;
[[vk::binding(2, 0)]] [[vk::combinedImageSampler]] SamplerState g_depthSampler;
[[vk::binding(3, 0)]] [[vk::combinedImageSampler]] Texture2D g_transmittance;
[[vk::binding(3, 0)]] [[vk::combinedImageSampler]] SamplerState g_transmittanceSampler;
[[vk::binding(4, 0)]] [[vk::combinedImageSampler]] Texture2D g_multi;
[[vk::binding(4, 0)]] [[vk::combinedImageSampler]] SamplerState g_multiSampler;
[[vk::binding(0, 0)]] ByteAddressBuffer g_terrainSunShadow;

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

// Monotonic density-aware integration boundaries. A fixed 40-step linear
// march at orbital distance can stride across the entire dense atmosphere
// in one or two steps; its midpoint crosses the terminator in discrete
// arcs as the camera changes altitude. Cluster steps near the closest
// approach to the planet (or near the surface intersection when clipped).
float AtmosphereMarchBoundary(float u, float startT, float endT,
    float closestT, float centerIndex)
{
    // Invert a two-sided cubic density centred on the ray's nearest
    // approach. The split/index continuously varies from 0 to 1 as the
    // closest approach crosses either end of the segment. There must be
    // NO hard branch at a fixed percentage of the segment: the old 2.5%
    // cutoff jumped an integration boundary by ~10% of an orbital ray.
    if (u <= centerIndex)
    {
        const float q = saturate((centerIndex - u) /
            max(centerIndex, 1.0e-6));
        return startT + (closestT - startT) * (1.0 - q * q * q);
    }
    const float q = saturate((u - centerIndex) /
        max(1.0 - centerIndex, 1.0e-6));
    return closestT + (endT - closestT) * q * q * q;
}

// Exact zero-angular-radius eclipse interval for a parallel stellar ray.
// An ellipsoid becomes a sphere under axis scaling. Its infinite shadow
// is the intersection of a circular cylinder behind the planet and the
// nightside half-space. Both are convex, so along the camera ray their
// intersection is either empty or ONE contiguous interval [start,end].
// Clip the quadratic cylinder and linear half-space in normalized view
// segment coordinates to avoid tiny metre-scale quadratic coefficients.
float2 EllipsoidEclipseInterval(float3 origin, float3 viewDirection,
    float3 radii, float3 scaledSun, float startT, float endT)
{
    const float segment = endT - startT;
    const float3 q0 = (origin + viewDirection * startT) / radii;
    const float3 qv = (viewDirection * segment) / radii;
    const float3 perp0 = q0 - scaledSun * dot(q0, scaledSun);
    const float3 perpV = qv - scaledSun * dot(qv, scaledSun);
    const float a = dot(perpV, perpV);
    const float b = dot(perp0, perpV);
    const float c = dot(perp0, perp0) - 1.0;
    float lower = 0.0;
    float upper = 1.0;
    if (a <= 1.0e-16)
    {
        if (c > 0.0) return float2(endT, endT);
    }
    else
    {
        const float discriminant = b * b - a * c;
        if (discriminant < 0.0) return float2(endT, endT);
        const float root = sqrt(max(discriminant, 0.0));
        lower = max(lower, (-b - root) / a);
        upper = min(upper, (-b + root) / a);
    }

    // The front-facing half of the extended cylinder is sunlit. Only
    // points with dot(q,scaledSun) < 0 can be eclipsed by the planet.
    const float facing0 = dot(q0, scaledSun);
    const float facingV = dot(qv, scaledSun);
    if (facingV > 1.0e-9)
        upper = min(upper, -facing0 / facingV);
    else if (facingV < -1.0e-9)
        lower = max(lower, -facing0 / facingV);
    else if (facing0 >= 0.0)
        return float2(endT, endT);

    if (lower >= upper) return float2(endT, endT);
    return float2(startT + segment * lower,
                  startT + segment * upper);
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

    // Short paths (every surface pixel of a near-field scene) need few steps:
    // the medium barely changes over tens of metres. Steps stay at most ~300 m
    // apart, which is far below the scale heights, and long paths keep the
    // full count.
    const uint steps =
        clamp(uint(ceil((tEnd - tStart) / 300.0)) + 3u, 4u, kSteps);
    const float closestT = clamp(-dot(origin, direction), tStart, tEnd);
    const float closestFraction = saturate(
        (closestT - tStart) / max(tEnd - tStart, 1.0e-4));
    const float leftWeight = sqrt(closestFraction);
    const float rightWeight = sqrt(1.0 - closestFraction);
    const float centerIndex = leftWeight /
        max(leftWeight + rightWeight, 1.0e-6);
    float3 transmittance = 1.0;
    float3 radiance = 0.0;
    float largestSegment = 0.0;
    float visibilityNear = 1.0;
    float visibilityFar = 1.0;
    static const uint kShadowInterval = 16u;
    // The optional relief field only adds mountain occlusion. The
    // planetary solar shadow interval below is always evaluated.
    const bool terrainFieldReady =
        g_terrainSunShadow.Load(12u) > 0u &&
        g_terrainSunShadow.Load(16u) > 0u;
    const float3 bodyRadii = max(g.sunOcclusion.yzw, float3(1.0, 1.0, 1.0));
    const float3 scaledSun = normalize(sun / bodyRadii);
    // Solve once per view ray rather than sampling a hard terminator only
    // at each of 40 step midpoints. Each step will integrate its lit
    // subinterval(s) with the correct Beer-Lambert camera attenuation.
    const float2 eclipse = EllipsoidEclipseInterval(
        origin, direction, bodyRadii, scaledSun, tStart, tEnd);

    [loop]
    for (uint i = 0u; i < steps; ++i)
    {
        const float edgeA = AtmosphereMarchBoundary(
            float(i) / float(steps), tStart, tEnd, closestT, centerIndex);
        const float edgeB = AtmosphereMarchBoundary(
            float(i + 1u) / float(steps), tStart, tEnd, closestT, centerIndex);
        const float stepLength = max(edgeB - edgeA, 0.0);
        largestSegment = max(largestSegment, stepLength);
        const float t = 0.5 * (edgeA + edgeB);
        const float3 position = origin + direction * t;
        // Sparse view-ray knots bound terrain queries independently of
        // simulation-time delta. Body-fixed geometry is reused across timewarp.
        if (terrainFieldReady && (i % kShadowInterval) == 0u)
        {
            const uint endIndex = min(i + kShadowInterval, steps) - 1u;
            const float endA = AtmosphereMarchBoundary(
                float(endIndex) / float(steps), tStart, tEnd, closestT, centerIndex);
            const float endB = AtmosphereMarchBoundary(
                float(endIndex + 1u) / float(steps), tStart, tEnd, closestT, centerIndex);
            const float endT = 0.5 * (endA + endB);
            visibilityNear = TerrainSunVisibility(g_terrainSunShadow, position,
                sun, bottom, g.sunOcclusion.x);
            visibilityFar = TerrainSunVisibility(g_terrainSunShadow,
                origin + direction * endT, sun, bottom, g.sunOcclusion.x);
        }
        const uint remaining = steps - (i / kShadowInterval) * kShadowInterval;
        const float terrainVisibility = terrainFieldReady
            ? lerp(visibilityNear, visibilityFar,
                float(i % kShadowInterval) /
                float(max(min(kShadowInterval, remaining) - 1u, 1u)))
            : 1.0;
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
        const float3 multiResponse =
            g_multi.SampleLevel(g_multiSampler, lut, 0).rgb;

        const float3 stepTransmittance = exp(-extinction * stepLength);
        const float3 safeExtinction = max(extinction, 1.0e-12);
        // Multiple scattering is a separate (approximate) illumination
        // source. Never multiply it by the direct planet eclipse mask.
        const float3 multiSource =
            irradiance * multiResponse * (rayleigh + mie) / (4.0 * kPi);
        if (g.audit.x != 1.0)
            radiance += transmittance * multiSource *
                (1.0 - stepTransmittance) / safeExtinction;

        // Light may turn on or off INSIDE this segment. A single midpoint
        // sample switches the whole interval at once, causing nested rings
        // in the dawn sky and stripes around an orbital terminator.
        // Analytically subtract the planet's eclipse interval from this
        // segment. Most segments have exactly one full lit interval or
        // none; only a segment straddling both boundaries has two.
        const float3 directPhase =
            rayleigh * rayleighPhase + mie * miePhase;
        [unroll]
        for (uint region = 0u; region < 2u; ++region)
        {
            const float litStart = region == 0u
                ? edgeA : max(edgeA, eclipse.y);
            const float litEnd = region == 0u
                ? min(edgeB, eclipse.x) : edgeB;
            if (litEnd <= litStart + 1.0e-4) continue;

            // Sample the solar transmittance in the LIT portion rather
            // than the eclipsed step midpoint. In particular, the LUT
            // can be zero at a shadowed midpoint even if half the segment
            // receives sunlight.
            const float litMid = 0.5 * (litStart + litEnd);
            const float3 sunPoint = origin + direction * litMid;
            const float sunRadius = length(sunPoint);
            const float sunMuLit =
                dot(sunPoint / max(sunRadius, 1.0), sun);
            const float3 sunTransmission =
                g_transmittance.SampleLevel(g_transmittanceSampler,
                    LutUv(sunRadius, sunMuLit), 0).rgb;
            const float discVisibility = EllipsoidSunVisibility(
                sunPoint, bodyRadii, scaledSun, g.sunOcclusion.x);

            // Integral of T_camera(s) on the lit interval. The camera
            // extinction is evaluated at the original density midpoint;
            // its variation is a separate, smooth quadrature error.
            const float3 opticalStart =
                exp(-extinction * max(litStart - edgeA, 0.0));
            const float3 opticalEnd =
                exp(-extinction * max(litEnd - edgeA, 0.0));
            if (g.audit.x != 2.0)
                radiance += transmittance * irradiance * directPhase *
                    sunTransmission * discVisibility * terrainVisibility *
                    (opticalStart - opticalEnd) / safeExtinction;
        }
        transmittance *= stepTransmittance;
    }

    // Diagnostic modes override the atmosphere composite without affecting
    // underlying material, cloud, or planet lighting calculations.
    if (g.audit.x == 3.0)
    {
        // White: full ray is illuminated, black: eclipsed; gray:
        // fraction illuminated. A spatially stepped image here implicates
        // shadow geometry rather than the scattering LUT or integration.
        const float shadowLength = max(0.0, eclipse.y - eclipse.x);
        const float illumination = saturate(
            1.0 - shadowLength / max(tEnd - tStart, 1.0e-4));
        return float4(illumination.xxx, 1.0);
    }
    if (g.audit.x == 4.0)
    {
        // Encode worst single-step length relative to Rayleigh scale
        // height; dark is well resolved, bright indicates undersampling.
        const float relativeStep = largestSegment /
            max(g.radii.z, 1.0);
        return float4(saturate(relativeStep / 8.0),
            saturate(relativeStep / 2.0),
            saturate(relativeStep * 0.5), 1.0);
    }
    return float4(
        (g.audit.y > 0.5 ? float3(0.0, 0.0, 0.0) :
            scene.rgb * transmittance) + radiance, scene.a);
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

    std::string pixelSource(kPixelShader);
    const auto mainPosition = pixelSource.find("float4 main(VSOutput input) : SV_Target0");
    if (mainPosition == std::string::npos)
        throw std::logic_error("Atmosphere shader entry point not found.");
    // The density and transmittance LUT remain referenced to a sphere,
    // but direct-sun visibility must instead use the body's real ellipsoid.
    // The shadow cylinder integrates the point-sun occultation exactly
    // along each segment; this local tangent-disc factor smooths the
    // remaining narrow finite-solar-disc transition.
    pixelSource.insert(mainPosition,
        std::string(lighting::kSunVisibilityShader) + R"(
float EllipsoidSunVisibility(float3 p, float3 axes,
    float3 normalizedScaledSun, float angularRadius)
{
    const float3 q = p / axes;
    const float d2 = dot(q, q);
    if (d2 <= 1.0) return 0.0;
    const float inverseD = rsqrt(d2);
    const float mu = dot(q, normalizedScaledSun) * inverseD;
    const float horizonMu = -sqrt(max(1.0 - inverseD * inverseD, 0.0));
    const float clearance = mu - horizonMu;
    // Uniform solar disc above a locally straight ellipsoid horizon.
    // The curvature effect is negligible within the thin atmosphere;
    // the width correction also avoids calling asin on full day/night.
    const float spread = angularRadius * inverseD;
    if (spread <= 1.0e-7) return clearance >= 0.0 ? 1.0 : 0.0;
    if (clearance >= spread) return 1.0;
    if (clearance <= -spread) return 0.0;
    return StellarDiscFraction(clearance, spread);
}
)");
    const auto pixel =
        compiler.Compile({
            .source = pixelSource,
            .entryPoint = "main",
            .stage = shader::Stage::Pixel,
            .debug = false
        });

    emptyTerrainShadow_ = lighting::CreateEmptyTerrainSunShadowBuffer(device);
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
            .pushConstantDwords = 44U,
            .shaderResourceBuffers = 1U,
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

    const std::array<u32, 44> constants{
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
        Bits(std::max(view.irradianceScale, 0.0F)),
        Bits(std::max(view.sunAngularRadiusRadians, 0.0F)),
        Bits(view.bodyRadiiMeters.x > 0.0F
            ? view.bodyRadiiMeters.x : static_cast<f32>(p.bottomRadiusMeters)),
        Bits(view.bodyRadiiMeters.y > 0.0F
            ? view.bodyRadiiMeters.y : static_cast<f32>(p.bottomRadiusMeters)),
        Bits(view.bodyRadiiMeters.z > 0.0F
            ? view.bodyRadiiMeters.z : static_cast<f32>(p.bottomRadiusMeters)),
        Bits(static_cast<f32>(std::min<u8>(view.auditMode, 4U))),
        Bits(view.auditRadianceOnly ? 1.0F : 0.0F),
        0U, 0U
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
    commands.SetGraphicsBuffer(0,
        (view.terrainOcclusionEnabled && view.terrainSunShadow)
            ? *view.terrainSunShadow : *emptyTerrainShadow_);
    commands.Draw(6);
}
} // namespace orbit::celestial_atmosphere
