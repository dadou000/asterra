#include <orbit/celestial_clouds/CloudRenderer.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace orbit::celestial_clouds
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
        float2(-1.0, -1.0), float2(-1.0, 1.0), float2(1.0, -1.0),
        float2(1.0, -1.0), float2(-1.0, 1.0), float2(1.0, 1.0)
    };
    const float2 uvs[6] =
    {
        float2(0.0, 1.0), float2(0.0, 0.0), float2(1.0, 1.0),
        float2(1.0, 1.0), float2(0.0, 0.0), float2(1.0, 0.0)
    };
    VSOutput output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    output.uv = uvs[vertexId];
    return output;
}
)";

constexpr const char* kCloudCommon = R"(
struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

struct Constants
{
    float4 cameraAspect;      // body-frame camera position (m), aspect
    float4 forwardTanHalfFov; // camera forward, tan(vfov/2)
    float4 upNear;            // camera up, near plane (m)
    float4 sunFar;            // unit sun direction, far plane (m)
    float4 shell;             // planet radius, base alt, top alt, face resolution
    float4 optics;            // albedo, anisotropy, irradiance scale, opacity scale
    float4 atmosphere;        // atmosphere bottom radius, top radius, detail scale, depth pixels per output pixel
    float4 lod;               // x: angular size of one output pixel (rad), y: coverage threshold, z: peak optical depth, w: density exponent
    float4 lab;               // cloud lab: centre direction (xyz), radius in metres (w, 0 = off)
    float4 labParams;         // cloud lab: type, coverage, cirrus, precipitation
    float4 labLife;           // cloud lab: maturity, organisation, density, seed
    float4 atmR;              // Rayleigh extinction rgb (1/m), scale height (m)
    float4 atmM;              // Mie extinction rgb (1/m), scale height (m)
    float4 atmG;              // Mie scattering (1/m), Mie anisotropy g
    float4 temporal;          // x: frame index (varies the march jitter so accumulated frames differ), y: god-ray strength, z: lab cirrus sheet coverage, w: light volume generation
    float3 volumeAnchor;      // light volume anchor (unit 'up' of its tangent frame); zero = light volume off
    float volumeDebug;        // light volume debug slice altitude (m); 0 = off
};

[[vk::push_constant]] Constants g;

// GpuCloudTexel: raw weather value, high cloud (cirrus / anvil) coverage, precipitation, cloudType (16 B)
[[vk::binding(0, 0)]] ByteAddressBuffer g_clouds : register(t0);
// 32^3 tileable cellular noise: R base shape, G/B/A erosion octaves (16 B)
[[vk::binding(1, 0)]] ByteAddressBuffer g_noise : register(t1);

// Marching budget. Steps are distance-adaptive (fine near the camera, coarse far
// away) but never fewer than the budget needs to cross the whole shell segment.
// Life cycle of the cloud at the current sample (set by ColumnData): 0 towering cumulus,
// 0.3 growing, 0.6 mature with anvil, 0.9 dissipating; and its density multiplier.
static float gMature = 0.6;
static float gDensity = 1.0;
// Density used for distant samples (footprint > ~150 m): natural storms are dense up close
// but a continent-wide convective region must stay translucent from orbit.
static float gDensityFar = 1.0;
// Surface relief of the last sample (set by Extinction): crevices between billows read darker.
static float gRelief = 1.0;
// 1 where the high layer is a cumulonimbus anvil (lab cloud, or natural weather directly
// above a tower); 0 where it is plain cirrus.
static float gAnvilGate = 1.0;
// Anvil optical-depth multiplier: a lab storm is a thick anvil; whole-planet anvils are
// hundreds of km wide and must stay translucent or they fog the planet.
static float gHighDensity = 1.0;
// Pixel footprint (m) over which the near-field cloud density fades to the thin far-field
// weather density. A pixel is ~2.2 mrad at the default resolution, so 100 m is about 45 km:
// clouds seen at ordinary flying distances stay opaque (starting the fade at 40 m, ~18 km,
// let the terrain and horizon show through clouds 20-60 km away).
static const float kFarFadeStart = 100.0;
static const float kFarFadeEnd = 400.0;
// Opacity-weighted distance of the cloud along the ray the march just traced (1e9 = none);
// the god-ray pass reads it to know which air lies behind the cloud.
static float gCloudDistance = 1.0e9;
static const uint kMaxSteps = 224u;
// Transmittance below which the march stops and the pixel counts as opaque.
static const float kOpaqueCutoff = 0.004;
// The geometric step schedule is solved to cross the segment in this many steps,
// leaving the rest of kMaxSteps for the fine steps taken inside cloud. Solving it
// for all of kMaxSteps let in-cloud steps near the camera use up the budget before
// a ray reached distant cloud (they vanished from altitude).
static const uint kScheduleSteps = 140u;
// Smallest step as a fraction of the footprint of one pixel at the sample distance:
// finer than that cannot be seen. Without it a camera far from the shell (orbit)
// marched empty space in 60 m steps for pixels hundreds of metres wide.
static const float kFootprintStep = 0.5;
// Inside cloud the step is at least this fraction of the schedule step, so a dense
// deck does not spend the whole budget before the distance is covered.
static const float kInCloudScheduleFraction = 0.35;
static const float kBaseStep = 60.0;
static const float kAdaptiveStep = 0.006;
static const float kMaxStep = 12000.0;
static const float kEmptySkip = 4.0;
static const float kBaseVoxelMeters = 700.0;   // 22 km noise period
static const float kDetailVoxelMeters = 78.0;  // 2.5 km noise period at detailScale 14
static const uint kShadowSteps = 16u;
static const float kPi = 3.14159265358979;

float3 ViewRay(float2 uv)
{
    const float3 forward = normalize(g.forwardTanHalfFov.xyz);
    const float3 right = normalize(cross(forward, normalize(g.upNear.xyz)));
    const float3 up = normalize(cross(right, forward));
    const float tanHalf = max(g.forwardTanHalfFov.w, 0.001);
    const float aspect = max(g.cameraAspect.w, 0.001);
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    return normalize(forward + right * (ndc.x * aspect * tanHalf) + up * (ndc.y * tanHalf));
}

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
    return nearPlane * farPlane / max(depth * (farPlane - nearPlane) + nearPlane, 1.0e-6);
}

// Mirrors CloudField.cpp DirectionToCube; returns face and (u, v) in [-1, 1].
void DirectionToCube(float3 d, out uint face, out float2 uv)
{
    const float3 a = abs(d);
    if (a.x >= a.y && a.x >= a.z)
    {
        if (d.x >= 0.0) { face = 0u; uv = float2(-d.z, d.y) / a.x; }
        else { face = 1u; uv = float2(d.z, d.y) / a.x; }
    }
    else if (a.y >= a.x && a.y >= a.z)
    {
        if (d.y >= 0.0) { face = 2u; uv = float2(d.x, -d.z) / a.y; }
        else { face = 3u; uv = float2(d.x, d.z) / a.y; }
    }
    else
    {
        if (d.z >= 0.0) { face = 4u; uv = float2(d.x, d.y) / a.z; }
        else { face = 5u; uv = float2(-d.x, d.y) / a.z; }
    }
}

float SmoothRange(float a, float b, float x);

// (coverage, high cloud / anvil coverage, cloud type, precipitation) of the layer in direction d,
// bilinear in a cube face: one 16-byte load per corner.
float4 LabColumn(float3 d);

float4 ColumnData(float3 d)
{
    gMature = 0.6;
    gDensity = 1.0;
    gDensityFar = 1.0;
    gRelief = 1.0;
    gAnvilGate = 1.0;
    gHighDensity = g.labLife.z;
    if (g.lab.w > 0.0)
    {
        return LabColumn(d);
    }
    // The weather field has ~50 km texels; bilinear interpolation of a thresholded
    // field leaves axis-aligned rectangles in the sky. Warp the lookup direction with
    // a cheap multi-frequency field (wavelengths 0.4-0.9 thousand km, up to ~60 km of
    // displacement) so cell and cloud edges are organic. Free: just a few sines.
    const float3 warp3 = float3(
        sin(d.y * 43.0 + 1.7) + 0.6 * sin(d.z * 97.0 + 4.1) + 0.35 * sin(d.x * 211.0 + 0.4),
        sin(d.z * 47.0 + 0.3) + 0.6 * sin(d.x * 89.0 + 2.2) + 0.35 * sin(d.y * 197.0 + 3.3),
        sin(d.x * 41.0 + 5.1) + 0.6 * sin(d.y * 101.0 + 0.9) + 0.35 * sin(d.z * 223.0 + 2.0));
    d = normalize(d + warp3 * 0.0042);
    const uint resolution = (uint)g.shell.w;
    uint face;
    float2 uv;
    DirectionToCube(d, face, uv);
    const float last = float(resolution - 1u);
    const float2 f = clamp((uv * 0.5 + 0.5) * last, 0.0, last);
    const uint2 p0 = (uint2)floor(f);
    const uint2 p1 = min(p0 + 1u, resolution - 1u);
    // Smoothstep weights remove the diamond-shaped contours plain bilinear
    // interpolation leaves in a thresholded field.
    const float2 tl = f - float2(p0);
    const float2 t = tl * tl * (3.0 - 2.0 * tl);
    const uint faceBase = face * resolution * resolution;
    const uint4 i = faceBase + uint4(p0.y * resolution + p0.x, p0.y * resolution + p1.x,
                                     p1.y * resolution + p0.x, p1.y * resolution + p1.x);
    const float4 a = asfloat(g_clouds.Load4(i.x * 16u));
    const float4 b = asfloat(g_clouds.Load4(i.y * 16u));
    const float4 c = asfloat(g_clouds.Load4(i.z * 16u));
    const float4 e = asfloat(g_clouds.Load4(i.w * 16u));
    const float4 m = lerp(lerp(a, b, t.x), lerp(c, e, t.x), t.y);
    // Threshold the interpolated weather value (same ramp as the CPU field), so
    // cloud edges are smooth contours instead of texel-sized steps.
    const float ramp = saturate((m.x - (g.lod.y - 0.26)) / 0.52);
    const float coverage = pow(ramp * ramp * (3.0 - 2.0 * ramp), g.lod.w);
    // Natural weather carries no per-cell age; infer it: a tower with anvil outflow
    // (high cloud coverage) is mature, a bare tower is still growing.
    // Storms differ in age: vary it by region (a few hundred km), so a continent carries
    // growing, mature and decaying cells instead of one age everywhere.
    const float ageRegion = 0.5 + 0.5 * sin(dot(d, float3(137.0, 211.0, 173.0)) + 0.7 * sin(dot(d, float3(331.0, 97.0, 257.0))));
    gMature = saturate(lerp(0.3, 0.55, saturate(m.y * 1.6)) + 0.4 * ageRegion);
    // Whole-planet weather is far less dense than a lab storm: the model's convective
    // columns cover hundreds of kilometres, so scale extinction down to keep them
    // translucent (a 6 km storm is thick, a 300 km convective region is not solid).
    // Near the camera a cumulus interior is opaque (extinction 0.01-0.05 per metre: you
    // see a few tens of metres into it). At 1.0 the interior was translucent: sky showed
    // through it and the per-pixel march jitter read as grain. The factor fades to
    // gDensityFar with the pixel footprint (see Extinction), so distant weather is unchanged.
    gDensity = 8.0;
    gDensityFar = 0.4;
    gAnvilGate = smoothstep(0.02, 0.25, coverage);
    gHighDensity = 0.03;
    // Faint planet-wide cirrus (coverage below ~0.1) is not worth a layer: it would only
    // fog the sky. Keep patches of real cirrus and let the rest be clear.
    return float4(coverage, 0.42 * smoothstep(0.12, 0.8, m.y), m.w, m.z);
}

float4 NoiseVoxel(int3 c)
{
    const uint3 w = uint3(c) & 31u;
    return asfloat(g_noise.Load4(((w.z * 32u + w.y) * 32u + w.x) * 16u));
}

// Trilinear lookup of the tileable noise; p is in voxel units.
float4 SampleNoise(float3 p)
{
    const float3 fl = floor(p);
    // Smoothstep weights: plain linear interpolation of the baked noise shows its
    // voxel lattice as axis-aligned blocks on cloud tops.
    const float3 tl = p - fl;
    const float3 t = tl * tl * (3.0 - 2.0 * tl);
    const int3 c = int3(fl);
    const float4 n000 = NoiseVoxel(c);
    const float4 n100 = NoiseVoxel(c + int3(1, 0, 0));
    const float4 n010 = NoiseVoxel(c + int3(0, 1, 0));
    const float4 n110 = NoiseVoxel(c + int3(1, 1, 0));
    const float4 n001 = NoiseVoxel(c + int3(0, 0, 1));
    const float4 n101 = NoiseVoxel(c + int3(1, 0, 1));
    const float4 n011 = NoiseVoxel(c + int3(0, 1, 1));
    const float4 n111 = NoiseVoxel(c + int3(1, 1, 1));
    return lerp(lerp(lerp(n000, n100, t.x), lerp(n010, n110, t.x), t.y),
                lerp(lerp(n001, n101, t.x), lerp(n011, n111, t.x), t.y), t.z);
}

float3 RotateNoiseSpace(float3 p)
{
    return float3(
        dot(p, float3(0.36, 0.48, -0.80)),
        dot(p, float3(-0.80, 0.60, 0.0)),
        dot(p, float3(0.48, 0.64, 0.60)));
}

// Interleaved gradient noise: well distributed per-pixel offsets, so the march
// grain is fine and uncorrelated instead of blotchy.
float InterleavedGradient(float2 pixel)
{
    // PCG integer hash: uncorrelated white noise per pixel. (Interleaved gradient
    // noise has diagonal structure that shows as stripes on long horizontal rays.)
    uint v = uint(pixel.x) * 747796405u + uint(pixel.y) * 2891336453u + 12345u;
    v = ((v >> ((v >> 28u) + 4u)) ^ v) * 277803737u;
    v = (v >> 22u) ^ v;
    return float(v) * (1.0 / 4294967296.0);
}

float Hash13(float3 p)
{
    p = frac(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return frac((p.x + p.y) * p.z);
}

float SmoothRange(float a, float b, float x)
{
    const float t = saturate((x - a) / (b - a));
    return t * t * (3.0 - 2.0 * t);
}

)";

constexpr const char* kCloudCommonB = R"(// Cloud type parameters, interpolated between anchor types along the type axis:
//   stratus .05, stratocumulus .2, nimbostratus .32, cumulus .5, cumulus congestus .72,
//   cumulonimbus 1.0.
// x: top of the cloud as a fraction of the (extended) shell, y: how much the cloud
// narrows with height (towers and domes), z: erosion depth (billowing vs smooth),
// w: edge hardness of the base shape.
float4 TypeParams(float cloudType)
{
    const float anchors[6] = { 0.05, 0.20, 0.32, 0.50, 0.72, 1.00 };
    const float4 params[6] = {
        float4(0.10, 0.00, 0.20, 1.2),  // stratus: thin, low, smooth
        float4(0.17, 0.15, 0.50, 1.8),  // stratocumulus: low lumpy deck
        float4(0.50, 0.05, 0.15, 1.0),  // nimbostratus: deep, layered, smooth
        float4(0.28, 0.70, 0.70, 2.6),  // cumulus: flat base, domed billows
        float4(0.52, 0.45, 0.74, 2.8),  // congestus: tall cumulus tower
        float4(0.95, 0.35, 0.78, 3.0)   // cumulonimbus: full-height cauliflower tower
    };
    float4 result = params[0];
    [unroll]
    for (uint i = 0u; i < 5u; ++i)
    {
        result = lerp(result, params[i + 1u], SmoothRange(anchors[i], anchors[i + 1u], cloudType));
    }
    return result;
}

// Rain shaft below the cloud base: vertical grey curtains under precipitating cores,
// thinning towards the ground (virga) and breaking into streaks.
float RainExtinction(float4 column, float3 position, float radius, float footprint)
{
    const float altitude = radius - g.shell.x;
    // Only under real precipitating cloud (dense, heavy rain), never under thin weather.
    const float rainGate = smoothstep(0.35, 0.75, column.w) * smoothstep(0.45, 0.8, column.x);
    if (rainGate <= 0.0 || altitude <= 0.0 || altitude >= g.shell.y)
    {
        return 0.0;
    }
    const float3 up = position / max(radius, 1.0);
    const float3 ground = position - up * altitude;
    // Streaks: horizontal cells ~300 m, very tall vertical cells so they fall straight.
    const float4 n = SampleNoise(float3(ground.x, ground.z + ground.y, ground.y * 0.5) / (kBaseVoxelMeters * 0.45)
        + float3(0.0, altitude / (kBaseVoxelMeters * 6.0), 0.0) + 41.0);
    const float coarse = 0.55 * n.g + 0.45 * n.b;
    const float streakBlend = SmoothRange(0.4, 1.4, footprint / (kBaseVoxelMeters * 0.45 * (32.0 / 6.0)));
    const float streaks = lerp(SmoothRange(0.42, 0.62, coarse), 0.5, streakBlend);
    const float fall = SmoothRange(0.0, 0.12, altitude / g.shell.y) * 0.6 + 0.4;
    const float top = SmoothRange(1.0, 0.8, altitude / g.shell.y);
    return 0.0016 * rainGate * streaks * lerp(fall, 1.0, 0.5) * (0.4 + 0.6 * top) * gDensity;
}

)";

constexpr const char* kCloudCommonC = R"(
)";

constexpr const char* kCloudCommonD = R"(
// Wall cloud: a lowered, ragged cloud base that hangs under the updraft of a mature
// supercell-like storm, in patches beside the rain. Only strong mature towers have one.
float WallCloud(float4 column, float3 position, float radius, float footprint)
{
    const float towerness = SmoothRange(0.7, 0.95, column.z);
    const float gate = towerness * SmoothRange(0.55, 0.85, column.x)
        * SmoothRange(0.4, 0.6, gMature) * (1.0 - SmoothRange(0.78, 0.95, gMature));
    const float altitude = radius - g.shell.x;
    const float depth = 1100.0;
    if (gate <= 0.0 || altitude <= g.shell.y - depth || altitude >= g.shell.y)
    {
        return 0.0;
    }
    const float3 up = position / max(radius, 1.0);
    const float3 ground = position - up * altitude;
    // Patches of ~3 km, kept off the rain core so the wall cloud sits beside the shaft.
    const float patch = SmoothRange(0.42, 0.58, SampleNoise(ground / (kBaseVoxelMeters * 4.0) + 77.0).g);
    const float turbulence = lerp(SampleNoise(position / (kBaseVoxelMeters * 0.6) + 91.0).r, 0.5,
        SmoothRange(0.4, 1.2, footprint / (kBaseVoxelMeters * 0.6)));
    const float below = (g.shell.y - altitude) / depth;     // 0 at the base, 1 at the tip
    const float profile = SmoothRange(0.0, 0.12, below) * (1.0 - SmoothRange(0.35, 1.0, below));
    return 0.012 * gate * patch * profile * (0.35 + 0.65 * turbulence) * gDensity;
}

)";

constexpr const char* kCloudCommonE = R"(
// Extinction (1/m) at a point: the main cloud (type dependent profile, taper and
// erosion) plus the high layer (cirrus and cumulonimbus anvil outflow).
// 'footprint' is the metres one sample covers; it filters the noise so far
// clouds are smooth instead of speckled.
float Extinction(float4 column, float3 position, float radius, float footprint)
{
    const float thickness = max(g.shell.z - g.shell.y, 1.0);
    const float h = (radius - g.shell.x - g.shell.y) / thickness;
    if (h <= 0.0)
    {
        return RainExtinction(column, position, radius, footprint) + WallCloud(column, position, radius, footprint);
    }
    if (h >= 1.0 || (column.x <= 1.0e-4 && column.y <= 1.0e-4))
    {
        return 0.0;
    }

    const float4 type = TypeParams(column.z);
    const float towerness = SmoothRange(0.55, 0.95, column.z);
    const float altitude = radius - g.shell.x;
    const float3 up = position / max(radius, 1.0);
    // A tower grows with its age: low and cauliflower-topped while young, full height
    // when mature.
    const float ageGrowth = SmoothRange(0.0, 0.55, gMature);
    // An overshooting top: the strongest updraft core punches above the rest of the
    // tower (and above the anvil) while the storm is growing or mature.
    const float overshoot = towerness * SmoothRange(0.9, 1.0, column.x) * SmoothRange(0.35, 0.6, gMature)
        * (1.0 - SmoothRange(0.75, 0.92, gMature));
    const float topFrac = type.x * lerp(1.0, lerp(0.34, 1.0, ageGrowth), towerness) * (1.0 + 0.05 * overshoot);
    // Mammatus: a mature or dissipating anvil sags into pouches on its underside.
    float mammatus = 0.0;
    if (towerness > 0.5 && gMature > 0.62 && column.y > 0.02 && h > 0.55 && h < 0.8)
    {
        const float pouch = SampleNoise((position - up * altitude) / (kBaseVoxelMeters * 0.3) + 71.0).g;
        mammatus = (pouch - 0.5) * 0.08 * SmoothRange(0.62, 0.92, gMature);
    }
    const float hc = (h - 0.70 + mammatus) / 0.2;   // high layer: 0.70-0.90 of the shell
    // Cheap rejection before any noise fetch: above the cloud top (plus the dome
    // bump allowance) and outside the high layer there is nothing to sample.
    const bool mainPossible = column.x > 1.0e-4 && h <= topFrac * 1.15 + 0.3;
    const bool highPossible = column.y > 1.0e-4 && hc > 0.0 && hc < 1.0;
    if (!mainPossible && !highPossible)
    {
        return 0.0;
    }
    // Towers (congestus, cumulonimbus) get noise stretched vertically so their billows
    // read as tall turrets instead of round puffs.
    const float3 shapePosition = position - up * (altitude * (1.0 - lerp(1.0, 0.62, towerness)));
    // Two non-aligned scales (one rotated) so the repeating noise lattice never reads as a grid.
    const float3 rotated = RotateNoiseSpace(shapePosition);
    const float baseFeature = kBaseVoxelMeters * (32.0 / 12.0);
    // Each noise octave must be gone by the time a pixel is half a noise cell wide (the sampling
    // limit); fading from 0.5 cells left features right at the limit, which showed as a regular
    // lattice of pebbles (strongest from orbit, where one march pixel spans ~900 m).
    const float baseBlend = SmoothRange(0.2, 0.9, footprint / baseFeature);
    float baseMix =
        0.6 * SampleNoise(shapePosition / kBaseVoxelMeters).r +
        0.4 * SampleNoise(rotated / (kBaseVoxelMeters * 2.6) + 5.0).r;
    const float coarseMix = baseMix;   // without the billow octave (the anvil uses this)
    // Billow octave (cells of 0.4-1.7 km): lobes on the silhouette, so towers read as
    // cauliflower instead of smooth columns. Filtered out once a pixel covers them.
    const float billowBlend = SmoothRange(0.2, 0.9, footprint / (160.0 * (32.0 / 12.0)));
    if (billowBlend < 1.0)
    {
        const float billow = SampleNoise(shapePosition / 160.0 + 53.0).r;
        baseMix = lerp(0.5 * baseMix + 0.5 * billow, baseMix, billowBlend);
    }
    const float baseValue = saturate(0.5 + (baseMix - 0.5) * 1.6);
    const float coarseValue = saturate(0.5 + (coarseMix - 0.5) * 1.6);

    float sigma = 0.0;

    // ---- main cloud ----
    // Fine erosion noise (camera-near only).
    const float detailMeters = kDetailVoxelMeters * 14.0 / max(g.atmosphere.z, 1.0);
    // The finest cell of each baked octave set is 32/12 voxels wide.
    const float detailAmount = 1.0 - SmoothRange(0.4, 1.5, footprint / (detailMeters * (32.0 / 12.0)));
    // Each column tops out at its own height, and towers get a dome noise of 0.75-3 km
    // that lifts and lowers the top, so a tower is a cluster of turrets with a
    // cauliflower outline instead of one flat-topped column. Only towers pay for it.
    const float topVariation = lerp(0.9 + 0.1 * baseValue, 0.9 + 0.2 * baseValue, towerness);
    float topBumps = 0.0;
    if (column.x > 1.0e-4 && towerness > 0.02 && baseBlend < 0.95)
    {
        // Altitude-free coordinates: the dome height is a heightfield over the ground, so
        // each turret has one coherent top instead of a noisy one.
        const float3 groundPosition = position - up * altitude;
        const float4 domeNoise = SampleNoise(groundPosition / (kBaseVoxelMeters * 0.5) + 31.0);
        const float dome = 0.4 * domeNoise.g + 0.3 * domeNoise.b + 0.3 * domeNoise.a;
        // Turret heights vary by roughly +/-25 % of the tower, with the strongest updraft
        // cores (largest dome values) rising above their neighbours.
        topBumps = (0.5 - dome) * 1.1 * towerness * (1.0 - baseBlend);
    }
    const float hn = (h + topBumps) / max(topFrac * topVariation, 1.0e-3);
    if (column.x > 1.0e-4 && hn > 0.0 && hn < 1.0)
    {
        const float profile =
            SmoothRange(0.0, 0.08, hn) * (1.0 - SmoothRange(lerp(0.4, 0.8, towerness), 1.0, hn))
            / (lerp(0.69, 0.87, towerness) * topFrac * topVariation);
        // Towers and domes narrow with height: less of the noise passes the threshold.
        // Half cone, half hemisphere: near-vertical flanks and a rounded crown.
        const float crown = 1.0 - sqrt(saturate(1.0 - hn * hn));
        const float narrowing = lerp(SmoothRange(0.1, 0.95, hn), crown, 0.55);
        // Under an anvil the tower flares out into it instead of narrowing to a stem.
        const float flare = 1.0 + 0.7 * towerness * column.y * SmoothRange(0.5, 0.85, hn);
        const float coverage = saturate(saturate(column.x) * (1.0 - type.y * narrowing) * flare);
        // The erosion octaves also move the iso-surface of the base shape (not only
        // eat its interior), so the silhouette is lumpy at 200-800 m. Without this a
        // small cloud is a smooth blob wherever the base noise has a single peak.
        float erosionValue = 0.5;
        if (detailAmount > 0.0)
        {
            const float4 d0 = SampleNoise(rotated / detailMeters + 17.0);
            erosionValue = 0.5 * d0.g + 0.3 * d0.b + 0.2 * d0.a;
        }
        gRelief = lerp(1.0, 0.45 + 0.75 * saturate(0.5 * baseValue + 0.5 * erosionValue), detailAmount);
        const float shifted = baseValue - (1.0 - erosionValue) * lerp(0.15, 0.2, towerness) * detailAmount;
        // Towers keep a solid core but their skin is carved by the noise at every level:
        // threshold against a reduced coverage so the iso-surface sits inside the mass.
        const float skinCoverage = coverage * lerp(1.0, 0.88, towerness);
        float shape = saturate(type.w * (shifted - (1.0 - skinCoverage)) / max(skinCoverage, 0.05));
        shape = lerp(shape, coverage, baseBlend);
        if (detailAmount > 0.0 && shape > 0.0)
        {
            const float erosion = erosionValue;
            // Erosion bites harder towards the top, which makes the cauliflower edge.
            const float depth = type.z * detailAmount * (0.55 + 0.7 * hn) * lerp(1.0, 1.1, towerness);
            shape = saturate((shape - depth * (1.0 - erosion)) / max(1.0 - depth, 0.05));
        }
        // Glaciation: the top of an ageing tower turns to ice, which smooths the
        // cauliflower into a soft, fibrous, more translucent crown (capillatus).
        const float glaciate = SmoothRange(0.78, 0.97, hn) * SmoothRange(0.45, 0.8, gMature) * towerness;
        float density = 1.0;
        if (glaciate > 0.0)
        {
            const float soft = saturate(1.4 * (baseValue - (1.0 - 0.9 * coverage)));
            shape = lerp(shape, soft, glaciate);
            density = 1.0 - 0.45 * glaciate;
        }
        // Rain-bearing cloud is denser and heavier toward its base.
        const float rainBase = 1.0 + 1.2 * column.w * (1.0 - SmoothRange(0.0, 0.35, hn));
        // Real storm clouds are far denser than fair-weather cumulus (the reference
        // clouds use 0.006-0.012 per metre); towers shade themselves hard.
        sigma += g.lod.z * saturate(column.x) / thickness * profile * shape * 1.7 * g.optics.w * rainBase
            * lerp(1.0, 12.0, towerness) * lerp(gDensity, gDensityFar, SmoothRange(kFarFadeStart, kFarFadeEnd, footprint)) * density;
    }

    // ---- high cloud: cirrus fibres and cumulonimbus anvil ----
    // Over a tower the high layer is the anvil: dense, with a flat underside and a
    // billowy top, spreading from the tower summit (tower tops reach 0.76-0.95 of the
    // shell, inside this band, so the two are one cloud). Elsewhere it is thin cirrus.
    if (highPossible)
    {
        const float cirrusProfile =
            SmoothRange(0.0, 0.7, hc) * (1.0 - SmoothRange(0.5, 1.0, hc)) / (0.2 * 0.401);
        // The anvil thins towards its rim: the underside rises, so the edge is a
        // sloped wedge instead of a vertical wall.
        const float lift = (1.0 - saturate(column.y)) * 0.55;
        const float hcAnvil = saturate((hc - lift) / max(1.0 - lift, 0.05));
        const float anvilProfile =
            SmoothRange(0.0, 0.12, hcAnvil) * (1.0 - SmoothRange(0.75, 1.0, hcAnvil)) / (0.2 * 0.82 * (1.0 - lift));
        // Fibres dominate the thin edge of an anvil, billows its thick core.
        const float highTower = towerness * gAnvilGate;
        // A thin, spread-out (natural-weather) anvil is mostly fibrous cirrus; only a thick lab
        // storm anvil has a billowy core.
        const float anvilCore = highTower * SmoothRange(0.35, 0.85, column.y) * lerp(0.25, 1.0, saturate(gHighDensity));
        const float profile = lerp(cirrusProfile, anvilProfile, highTower);
        // Fibres: the noise is stretched along one axis, and sharp-edged.
        // Fibres run along lines of latitude (zonal, like jet-stream cirrus) in
        // longitude/latitude coordinates, bent by a slow warp. (Using the position
        // itself loses the tangential variation: on a sphere it is all in the radius.)
        const float lon = atan2(up.z, up.x) * g.shell.x;
        const float lat = asin(clamp(up.y, -1.0, 1.0)) * g.shell.x;
        const float warp = (SampleNoise(float3(lon, lat, 0.0) / (kBaseVoxelMeters * 9.0) + 3.0).g - 0.5) * kBaseVoxelMeters * 24.0;
        // An anvil is billowy, not fibrous: its noise cells are near isotropic (no tall
        // thin columns, which show as spikes on the top), thin cirrus stays streaky.
        // Wander the fibres sideways with a mid-frequency warp so they are streaks, not
        // parallel sheets (an anvil edge seen side-on shows every sheet as a rib).
        const float wander = (SampleNoise(float3(lon, lat, 5.0) / (kBaseVoxelMeters * 3.0) + 13.0).r - 0.5)
            * kBaseVoxelMeters * 6.0;
        const float latWarped = lat + wander * (1.0 - highTower);
        const float3 fibre = float3((lon + warp * (1.0 - highTower)) / (kBaseVoxelMeters * lerp(24.0, 1.4, anvilCore)),
                                    latWarped / (kBaseVoxelMeters * lerp(0.13, 1.0, anvilCore)),
                                    altitude / (kBaseVoxelMeters * lerp(1.5, 4.0, anvilCore))) + 9.0;
        const float4 f = SampleNoise(fibre);
        // Fibres come from the finer baked octaves; the coarse one only shapes the clumps.
        const float fibreValue = lerp(0.25 * f.r + 0.3 * f.b + 0.45 * f.a, 0.6 * f.r + 0.4 * f.g, anvilCore);
        // The anvil mixes in the billow noise so its top is lumpy, not fibrous.
        const float shapeValue = lerp(fibreValue, 0.5 * fibreValue + 0.5 * coarseValue, anvilCore);
        float shape = saturate(lerp(4.0, 1.5, anvilCore) * (shapeValue - lerp(0.86 - 0.5 * column.y, 1.0 - column.y, highTower)) / max(column.y, 0.05));
        // Filter by the narrow dimension of the fibres: once a pixel is as wide as a fibre,
        // fade to the mean density (otherwise the fibres alias into one-pixel rows).
        const float fibreWidthMeters = kBaseVoxelMeters * lerp(0.13, 1.0, anvilCore) * (32.0 / 12.0);
        const float fibreBlend = SmoothRange(0.25, 1.0, footprint / fibreWidthMeters);
        // Far away a pixel is wider than a fibre: fade to the MEAN density of the noise
        // (not to the coverage, which is 2-3x too dense and turns the whole planet into a
        // haze); filtering removes the moire rings the narrow fibres alias into.
        const float meanThin = 0.53 * pow(saturate(column.y), 1.6);
        shape = lerp(shape, lerp(meanThin, column.y * 0.75, highTower), max(baseBlend, fibreBlend));
        // Thin cirrus is translucent (peak optical depth about 0.7); an anvil is thick ice.
        const float peak = lerp(0.2, 9.0 * gHighDensity, highTower);
        sigma += peak * column.y / thickness * profile * shape * 1.7;
    }
    return sigma;
}

)";

constexpr const char* kCloudCommonF = R"(
float Hg(float c, float gAnisotropy)
{
    const float gg = gAnisotropy * gAnisotropy;
    return (1.0 - gg) / (4.0 * kPi * pow(max(1.0 + gg - 2.0 * gAnisotropy * c, 1.0e-4), 1.5));
}

float DualHg(float c, float anisotropy)
{
    const float g1 = anisotropy;
    const float g2 = -0.3 * anisotropy;
    const float a = (1.0 - g1 * g1) / (4.0 * kPi * pow(max(1.0 + g1 * g1 - 2.0 * g1 * c, 1.0e-4), 1.5));
    const float b = (1.0 - g2 * g2) / (4.0 * kPi * pow(max(1.0 + g2 * g2 - 2.0 * g2 * c, 1.0e-4), 1.5));
    return lerp(b, a, 0.7);
}

float2 LutUv(float radius, float sunMu)
{
    const float rb = g.atmosphere.x;
    const float rt = g.atmosphere.y;
    const float unit = saturate((radius * radius - rb * rb) / max(rt * rt - rb * rb, 1.0));
    return float2(saturate(sunMu * 0.5 + 0.5), unit);
}

// Optical depth towards the sun: four increasing steps over 1.2 km on the base
// shape, plus a coarse tail for the rest of the way out of the layer.
// 'tailTau' >= 0 is the optical depth beyond the explicit steps (from the light volume); negative
// falls back to extrapolating the last sample.
float SunOpticalDepth(float3 position, float3 sun, float outer, float footprint, float jit, float tailTau)
{
    float sunT0;
    float sunT1;
    if (!RaySphere(position, sun, outer, sunT0, sunT1) || sunT1 <= 0.0)
    {
        return 0.0;
    }
    const float marches[6] = { 120.0, 260.0, 520.0, 1000.0, 1800.0, 3000.0 }; // six
    float depth = 0.0;
    float travelled = 0.0;
    float lastSigma = 0.0;
    [unroll]
    for (uint i = 0u; i < 6u; ++i)
    {
        const float stepLength = min(marches[i], max(sunT1 - travelled, 0.0));
        const float3 samplePosition = position + sun * (travelled + (0.15 + 0.7 * jit) * stepLength);
        const float radius = length(samplePosition);
        lastSigma = Extinction(ColumnData(samplePosition / max(radius, 1.0)), samplePosition, radius, max(0.7 * stepLength, footprint));
        depth += lastSigma * stepLength;
        travelled += stepLength;
    }
    const float tail = clamp(sunT1 - travelled, 0.0, 6000.0);
    return depth + (tailTau >= 0.0 ? tailTau : lastSigma * tail * 0.5);
}

// Optical depth towards the sun from 'origin', sampled once per slab of equal altitude between the
// origin and the top of the shell: each slab is crossed by the sun ray at whatever slant the sun has,
// so a sun a few degrees up reaches a hundred km or more with the same few samples (the way the KSP
// cloud mods project each layer along the sun, instead of marching a fixed distance). The slab path
// is exact (difference of the ray's exits from two concentric spheres); the density comes from one
// sample at its middle, so structure smaller than the slab's slant span is averaged.
float AltitudeSunTau(float3 origin, float3 sun, uint slabs)
{
    const float startRadius = length(origin);
    const float topRadius = g.shell.x + g.shell.z;
    if (startRadius >= topRadius)
    {
        return 0.0;
    }
    float tau = 0.0;
    float previousExit = 0.0;
    [loop]
    for (uint i = 1u; i <= slabs; ++i)
    {
        const float radius = startRadius + (topRadius - startRadius) * float(i) / float(slabs);
        float s0;
        float s1;
        if (!RaySphere(origin, sun, radius, s0, s1) || s1 <= previousExit)
        {
            break;
        }
        const float pathLength = s1 - previousExit;
        const float3 p = origin + sun * (previousExit + 0.5 * pathLength);
        const float r = length(p);
        tau += Extinction(ColumnData(p / max(r, 1.0)), p, r, max(0.7 * pathLength, 120.0)) * pathLength;
        previousExit = s1;
    }
    return tau;
}

)";

constexpr const char* kCloudLife = R"(
float LabHash(float n)
{
    return frac(sin(n * 127.1 + g.labLife.w * 311.7) * 43758.5453);
}

// Cloud lab: a cell (or a cluster of cells of different ages) with a life cycle.
// Sets gMature / gDensity for the sample and returns the usual column data.
float4 LabColumn(float3 d)
{
    const float3 c = g.lab.xyz;
    const float3 east = normalize(cross(float3(0.0, 1.0, 0.0), c) + float3(1.0e-6, 0.0, 0.0));
    const float3 north = cross(c, east);
    const float3 tangent = d - c * dot(d, c);
    const float2 p = float2(dot(tangent, east), dot(tangent, north)) * g.shell.x;
    const float radiusMeters = g.lab.w;
    const float maturity = g.labLife.x;
    const float organisation = g.labLife.y;
    const int cells = organisation < 0.3 ? 1 : (organisation < 0.75 ? 3 : 5);

    float coverage = 0.0;
    float anvil = 0.0;
    float dominant = 0.0;
    float dominantAge = maturity;
    float anvilAge = 0.0;
    [loop]
    for (int k = 0; k < 5; ++k)
    {
        if (k >= cells)
        {
            break;
        }
        float2 offset = float2(0.0, 0.0);
        float radius = radiusMeters;
        float age = maturity;
        if (k > 0)
        {
            const float a = 6.2831853 * LabHash(float(k) * 3.0);
            offset = float2(cos(a), sin(a)) * radiusMeters * (1.1 + 1.0 * LabHash(float(k) * 3.0 + 1.0));
            radius = radiusMeters * (0.4 + 0.45 * LabHash(float(k) * 3.0 + 2.0));
            // Daughter cells are younger than the parent cell.
            age = saturate(maturity - (0.2 + 0.25 * LabHash(float(k) * 3.0 + 0.5)) * (1.0 - 0.4 * organisation));
        }
        const float dist = length(p - offset);
        // A dissipating cell loses its updraft: the tower collapses.
        const float collapse = 1.0 - 0.85 * SmoothRange(0.8, 1.0, age);
        const float core = (1.0 - SmoothRange(0.6 * radius, radius, dist)) * collapse;
        // The anvil spreads with age, much wider than the tower, wider still when organised.
        const float anvilRadius = radius * (1.0 + (1.6 + 1.6 * organisation) * SmoothRange(0.35, 0.8, age));
        const float wide = (1.0 - SmoothRange(0.5 * anvilRadius, anvilRadius, dist)) * SmoothRange(0.3, 0.6, age);
        if (core > dominant)
        {
            dominant = core;
            dominantAge = age;
        }
        coverage = max(coverage, core);
        anvil = max(anvil, wide);
        anvilAge = max(anvilAge, age * step(0.01, wide));
    }
    // Thin cirrus sheet on the anti-sun side of the cell: patchy, starts right behind the tower and
    // thins out over ~20 cell radii, so the storm's shadow has cirrus to fall on.
    float sheet = 0.0;
    const float sheetCover = g.temporal.z;
    if (sheetCover > 0.0)
    {
        const float3 sunTangent = g.sunFar.xyz - c * dot(g.sunFar.xyz, c);
        float2 sunDir = float2(dot(sunTangent, east), dot(sunTangent, north));
        const float sunLength = length(sunDir);
        if (sunLength > 1.0e-3)
        {
            sunDir /= sunLength;
            const float behind = -dot(p, sunDir);                 // metres on the shadow side
            const float side = dot(p, float2(-sunDir.y, sunDir.x));
            const float along = SmoothRange(0.7 * radiusMeters, 1.8 * radiusMeters, behind)
                * (1.0 - SmoothRange(14.0 * radiusMeters, 22.0 * radiusMeters, behind));
            const float lateral = 1.0 - SmoothRange(8.0 * radiusMeters, 16.0 * radiusMeters, abs(side));
            const float2 q = p / (1.7 * radiusMeters);
            const float patch = 0.5 + 0.25 * sin(q.x * 1.3 + 2.0 * sin(q.y * 0.7) + 1.1)
                + 0.25 * sin(q.y * 1.9 + 1.7 * sin(q.x * 0.9) + 3.4) + 0.12 * sin((q.x + q.y) * 3.7);
            sheet = sheetCover * along * lateral * SmoothRange(0.35, 0.75, patch);
        }
    }
    const float cellAnvil = g.labParams.z * anvil;
    const float high = max(cellAnvil, sheet);
    // Where the sheet dominates it is plain thin cirrus (type 0: no tower, no billowy core).
    const float sheetShare = sheet / max(high, 1.0e-4);
    gMature = dominant > 0.02 ? dominantAge : anvilAge;
    gDensity = g.labLife.z;
    gDensityFar = g.labLife.z;
    return float4(g.labParams.y * coverage, high, lerp(g.labParams.x, 0.0, sheetShare), g.labParams.w * coverage);
}
)";

constexpr const char* kDepthBindings = R"(
[[vk::binding(3, 0)]] [[vk::combinedImageSampler]] Texture2D g_depth;
[[vk::binding(3, 0)]] [[vk::combinedImageSampler]] SamplerState g_depthSampler;
)";

constexpr const char* kMarchBindings = R"([[vk::binding(4, 0)]] [[vk::combinedImageSampler]] Texture2D g_transmittance;
[[vk::binding(4, 0)]] [[vk::combinedImageSampler]] SamplerState g_transmittanceSampler;
[[vk::binding(5, 0)]] [[vk::combinedImageSampler]] Texture2D g_multi;
[[vk::binding(5, 0)]] [[vk::combinedImageSampler]] SamplerState g_multiSampler;
)";

// ---- Sun light volume --------------------------------------------------------------------
// A cache of the optical depth towards the sun (to the edge of the cloud shell) on a grid around
// the camera, the way EVE's light volume feeds its clouds and Scatterer's god rays. Three toroidal
// cascades (250 m, 2 km and 8 km cells: usable out to ~11, ~92 and ~368 km from the camera; 96 x 96
// columns, 32 layers over the shell) in a tangent frame
// anchored at one direction on the planet; a few voxels refresh every frame (see kVolumeCompute)
// and a voxel is trusted only while its tag matches the cell it should hold. Readers fall back to
// marching when a voxel is missing, so a cold or moving volume only costs speed, not correctness.
constexpr const char* kVolumeMath = R"(
static const uint kVolN = 96u;
static const uint kVolLayers = 32u;
static const float kVolCell0 = 250.0;
static const float kVolCell1 = 2000.0;
static const float kVolCell2 = 8000.0;
static const uint kVolCascades = 3u;
static const float kVolTagOffset = 100000.5;

bool VolActive() { return dot(g.volumeAnchor, g.volumeAnchor) > 0.5; }
float VolTop() { return max(g.shell.z, 1000.0); }
float VolCell(uint cascade) { return cascade == 0u ? kVolCell0 : (cascade == 1u ? kVolCell1 : kVolCell2); }
// Wraps a (possibly negative) cell index into [0, kVolN). Unsigned arithmetic only: a signed '%' of a
// negative value is implementation-defined enough across compilers and drivers to put the writer's
// and the reader's idea of a cell's address apart exactly where indices change sign. Cell indices stay
// far below the offset (about +-25000 even for the finest cascade over a whole planet).
uint VolWrap(int v) { return uint(v + int(kVolN) * 4096) % kVolN; }
uint VolAddress(uint cascade, uint layer, uint row, uint column)
{
    return (((cascade * kVolLayers + layer) * kVolN + row) * kVolN + column) * 16u;
}

void VolFrame(out float3 east, out float3 north)
{
    east = normalize(cross(float3(0.0, 1.0, 0.0), g.volumeAnchor) + float3(1.0e-6, 0.0, 0.0));
    north = cross(g.volumeAnchor, east);
}

// Tangent-plane coordinates (metres, orthographic about the anchor) and altitude of a position.
void VolCoords(float3 position, out float2 t, out float altitude)
{
    float3 east;
    float3 north;
    VolFrame(east, north);
    const float radius = length(position);
    const float3 d = position / max(radius, 1.0);
    t = float2(dot(d, east), dot(d, north)) * g.shell.x;
    altitude = radius - g.shell.x;
}

float3 VolPosition(float2 t, float altitude)
{
    float3 east;
    float3 north;
    VolFrame(east, north);
    const float2 s = t / g.shell.x;
    const float3 d = normalize(east * s.x + north * s.y + g.volumeAnchor * sqrt(max(1.0 - dot(s, s), 0.01)));
    return d * (g.shell.x + altitude);
}
)";

constexpr const char* kVolumeRead = R"(
[[vk::binding(2, 0)]] ByteAddressBuffer g_volume : register(t2);

bool VolSampleVoxel(uint cascade, uint layer, int ai, int aj, out float tau)
{
    const float4 v = asfloat(g_volume.Load4(VolAddress(cascade, layer, VolWrap(aj), VolWrap(ai))));
    tau = v.x;
    return v.y == float(ai) + kVolTagOffset && v.z == float(aj) + kVolTagOffset && v.w == g.temporal.w;
}

// Optical depth towards the sun from 'position' out of the cloud shell, trilinear from the volume.
// False when the volume is off, the position is outside both cascades or a voxel is not ready.
bool VolumeTau(float3 position, out float tau)
{
    tau = 0.0;
    if (!VolActive())
    {
        return false;
    }
    float2 t;
    float altitude;
    VolCoords(position, t, altitude);
    if (altitude >= VolTop())
    {
        return true;                       // above the shell: nothing between here and the sun
    }
    float2 cameraT;
    float cameraAltitude;
    VolCoords(g.cameraAspect.xyz, cameraT, cameraAltitude);
    const float2 offset = abs(t - cameraT);
    const float limit0 = (float(kVolN) * 0.5 - 2.0) * kVolCell0;
    const float limit1 = (float(kVolN) * 0.5 - 2.0) * kVolCell1;
    const float limit2 = (float(kVolN) * 0.5 - 2.0) * kVolCell2;
    uint cascade = 0u;
    if (offset.x >= limit0 || offset.y >= limit0)
    {
        cascade = 1u;
        if (offset.x >= limit1 || offset.y >= limit1)
        {
            if (offset.x >= limit2 || offset.y >= limit2)
            {
                return false;
            }
            cascade = 2u;
        }
    }
    const float2 f = t / VolCell(cascade) - 0.5;
    const int2 i0 = int2(floor(f));
    const float2 w = f - float2(i0);
    const float fk = clamp(altitude / VolTop() * float(kVolLayers) - 0.5, 0.0, float(kVolLayers - 1u));
    const uint k0 = uint(floor(fk));
    const uint k1 = min(k0 + 1u, kVolLayers - 1u);
    const float wk = fk - float(k0);
    float sum = 0.0;
    [unroll]
    for (uint dk = 0u; dk < 2u; ++dk)
    {
        [unroll]
        for (int dy = 0; dy < 2; ++dy)
        {
            [unroll]
            for (int dx = 0; dx < 2; ++dx)
            {
                float v;
                if (!VolSampleVoxel(cascade, dk == 0u ? k0 : k1, i0.x + dx, i0.y + dy, v))
                {
                    return false;
                }
                sum += (dk == 0u ? 1.0 - wk : wk) * (dx == 0 ? 1.0 - w.x : w.x) * (dy == 0 ? 1.0 - w.y : w.y) * v;
            }
        }
    }
    tau = sum;
    return true;
}
)";

// Debug read of the volume (nearest voxel, no filtering): 0 = outside every cascade, 1 = the voxel for
// this cell is not ready, 2 = ready. 'cascade' is the cascade that holds the position.
constexpr const char* kVolumeDebugRead = R"(
uint VolumeDebugSample(float3 position, out float tau, out uint cascade)
{
    tau = 0.0;
    cascade = 0u;
    if (!VolActive())
    {
        return 0u;
    }
    float2 t;
    float altitude;
    VolCoords(position, t, altitude);
    if (altitude >= VolTop() || altitude < 0.0)
    {
        return 0u;
    }
    float2 cameraT;
    float cameraAltitude;
    VolCoords(g.cameraAspect.xyz, cameraT, cameraAltitude);
    const float2 offset = abs(t - cameraT);
    const float limit0 = (float(kVolN) * 0.5 - 2.0) * kVolCell0;
    const float limit1 = (float(kVolN) * 0.5 - 2.0) * kVolCell1;
    const float limit2 = (float(kVolN) * 0.5 - 2.0) * kVolCell2;
    if (offset.x >= limit0 || offset.y >= limit0)
    {
        cascade = 1u;
        if (offset.x >= limit1 || offset.y >= limit1)
        {
            if (offset.x >= limit2 || offset.y >= limit2)
            {
                return 0u;
            }
            cascade = 2u;
        }
    }
    const int2 cellIndex = int2(floor(t / VolCell(cascade)));
    const uint layer = min(uint(altitude / VolTop() * float(kVolLayers)), kVolLayers - 1u);
    const float4 v = asfloat(g_volume.Load4(VolAddress(cascade, layer, VolWrap(cellIndex.y), VolWrap(cellIndex.x))));
    tau = v.x;
    const bool tagX = v.y == float(cellIndex.x) + kVolTagOffset;
    const bool tagY = v.z == float(cellIndex.y) + kVolTagOffset;
    const bool generation = v.w == g.temporal.w;
    if (tagX && tagY)
    {
        return generation ? 2u : 4u;               // 4: right cell, old generation
    }
    if (v.y == 0.0 && v.z == 0.0 && v.w == 0.0)
    {
        return 3u;                                 // never written
    }
    // 1: another cell. Tell which parts matched: bit 3 generation, bit 4 x tag, bit 5 y tag.
    tau = 0.0;
    return 1u | (generation ? 8u : 0u) | (tagX ? 16u : 0u) | (tagY ? 32u : 0u);
}
)";

constexpr const char* kVolumeCompute = R"(
[[vk::binding(2, 0)]] RWByteAddressBuffer g_volumeRw : register(u2);

// Optical depth from 'origin' along the sun to the edge of the shell (altitude-stratified).
float VolumeMarch(float3 origin, float3 sun)
{
    return AltitudeSunTau(origin, sun, 32u);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (!VolActive() || id.x >= kVolN || id.y >= kVolN)
    {
        return;
    }
    const uint cascade = id.z / kVolLayers;
    const uint layer = id.z % kVolLayers;
    if (cascade >= kVolCascades)
    {
        return;
    }
    const float cell = VolCell(cascade);
    float2 cameraT;
    float cameraAltitude;
    VolCoords(g.cameraAspect.xyz, cameraT, cameraAltitude);
    // Toroidal addressing: this buffer address always holds the one cell, among the kVolN around
    // the camera, that is congruent to it, so moving the camera only re-targets the edge rows.
    const int2 baseCell = int2(floor(cameraT / cell)) - int(kVolN / 2u);
    const int ai = baseCell.x + int((id.x + kVolN - VolWrap(baseCell.x)) % kVolN);
    const int aj = baseCell.y + int((id.y + kVolN - VolWrap(baseCell.y)) % kVolN);
    const uint address = VolAddress(cascade, layer, id.y, id.x);
    const float4 old = asfloat(g_volumeRw.Load4(address));
    const bool valid = old.y == float(ai) + kVolTagOffset && old.z == float(aj) + kVolTagOffset && old.w == g.temporal.w;
    const uint hash = (id.x * 73856093u) ^ (id.y * 19349663u) ^ (id.z * 83492791u);
    const uint phase = (hash >> 5u) + uint(g.temporal.x);
    // A valid voxel refreshes every 6th frame (12th for the far cascade, which changes slowest);
    // a missing one is filled every 3rd frame so a teleport does not stall a frame.
    const uint period = cascade == 2u ? 12u : 6u;
    if (valid ? (phase % period != 0u) : (phase % 3u != 0u))
    {
        return;
    }
    const float altitude = (float(layer) + 0.5) / float(kVolLayers) * VolTop();
    const float3 origin = VolPosition((float2(ai, aj) + 0.5) * cell, altitude);
    const float tau = VolumeMarch(origin, normalize(g.sunFar.xyz));
    g_volumeRw.Store4(address, asuint(float4(tau, float(ai) + kVolTagOffset, float(aj) + kVolTagOffset, g.temporal.w)));
}
)";

constexpr const char* kMarchMain = R"(float4 CloudMarch(VSOutput input)
{
    // Output: premultiplied in-scattered radiance (rgb) and transmittance (a),
    // at reduced resolution; the composite pass applies it over the scene.
    const float4 empty = float4(0.0, 0.0, 0.0, 1.0);
    gCloudDistance = 1.0e9;
    const int2 pixel = int2(input.position.xy);

    const float3 origin = g.cameraAspect.xyz;
    const float3 direction = ViewRay(input.uv);
    // The march covers the sky below the cloud base too (rain shafts fall to the ground).
    const float inner = g.shell.x + 1.0;
    const float outer = g.shell.x + g.shell.z;

    float o0, o1;
    if (!RaySphere(origin, direction, outer, o0, o1) || o1 <= 0.0)
    {
        return empty;
    }

    float tStart = max(o0, 0.0);
    float tEnd = o1;
    float i0, i1;
    if (RaySphere(origin, direction, inner, i0, i1) && i1 > 0.0)
    {
        if (i0 > 0.0)
        {
            tEnd = i0;                // camera outside the shell, ray enters the base
        }
        else
        {
            tStart = max(i1, tStart); // camera under the base looking up
        }
    }

    // Nearest (reverse-Z: largest) depth of the full-resolution pixels this output pixel covers.
    uint depthWidth, depthHeight;
    g_depth.GetDimensions(depthWidth, depthHeight);
    const int2 depthBase = int2(float2(pixel) * g.atmosphere.w);
    const int2 depthMax = int2(depthWidth - 1u, depthHeight - 1u);
    const float depth = max(
        max(g_depth.Load(int3(min(depthBase, depthMax), 0)).r,
            g_depth.Load(int3(min(depthBase + int2(1, 0), depthMax), 0)).r),
        max(g_depth.Load(int3(min(depthBase + int2(0, 1), depthMax), 0)).r,
            g_depth.Load(int3(min(depthBase + int2(1, 1), depthMax), 0)).r));
    if (depth > 0.0)
    {
        const float surfaceT =
            ReverseZViewDepth(depth) / max(dot(direction, normalize(g.forwardTanHalfFov.xyz)), 1.0e-5);
        tEnd = min(tEnd, surfaceT);
    }

    if (tEnd <= tStart)
    {
        return empty;
    }

    const float3 sun = normalize(g.sunFar.xyz);
    const float cosine = dot(direction, sun);
    const float anisotropy = g.optics.y;
    const float albedo = g.optics.x;
    const float irradiance = max(g.optics.z, 0.0);

    // Sun colour through the atmosphere once per pixel, at the segment midpoint.
    // Capped to a few km: a ray that runs along the shell to the horizon has its midpoint
    // hundreds of km away, which gave a different sun colour above and below the horizon
    // line (a hard band when the camera is inside cloud).
    const float3 midPoint = origin + direction * min(0.5 * (tStart + tEnd), tStart + 3000.0);
    const float midRadius = length(midPoint);
    const float3 atmosphereSun =
        g_transmittance.SampleLevel(
            g_transmittanceSampler, LutUv(midRadius, dot(midPoint / max(midRadius, 1.0), sun)), 0).rgb;

    // Phase functions after the reference clouds: single scattering is two strong
    // forward lobes (g 0.95 and 0.8, the silver lining), multiple scattering two broad
    // lobes (g 0.2 and -0.4) that brighten the cloud interior.
    const float singlePhase = 0.1 * Hg(cosine, 0.95) + 0.2 * Hg(cosine, 0.8);
    const float multiPhase = 3.0 * Hg(cosine, 0.2) + 0.3 * Hg(cosine, -0.4);

    // Steps grow geometrically from the base step; the growth rate is solved per
    // pixel so the whole segment is covered in kMaxSteps steps. A long grazing ray
    // therefore keeps fine steps near the camera and only coarsens with distance
    // (a uniform budget step would blur every near cloud on the horizon).
    const float segment = tEnd - tStart;
    float growthLow = kAdaptiveStep;
    float growthHigh = 0.25;
    [unroll]
    for (uint s = 0u; s < 10u; ++s)
    {
        const float mid = 0.5 * (growthLow + growthHigh);
        const float covered = kBaseStep / mid * (pow(1.0 + mid, float(kScheduleSteps)) - 1.0);
        if (covered > segment) { growthHigh = mid; } else { growthLow = mid; }
    }
    const float growth = growthHigh;

    // Stratified jitter: each sample sits at a random offset inside its own step.
    // Stratified over 4x4 pixels (ordered dither, plus white noise inside each of the 16
    // cells): the composite's 3x3 tent filter then averages four well-spread offsets
    // instead of four independent random ones, which removes most of the edge grain that
    // pure white noise leaves (white noise has low-frequency energy no small filter hides).
    const uint bayerIndex = (uint(pixel.y) & 3u) * 4u + (uint(pixel.x) & 3u);
    const float bayer[16] = { 0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0,
                              3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0 };
    // Each 4x4 block rotates the pattern by its own random offset (still stratified inside the
    // block), so the ordered dither leaves no visible period.
    const float blockShift = InterleavedGradient(float2(int2(pixel) >> 2) + 4096.0);
    // Golden-ratio step per frame: every frame samples a different offset, so the temporal
    // resolve averages them into a smooth result.
    const float frameShift = frac(g.temporal.x * 0.6180339887);
    const float jitter = frac((bayer[bayerIndex] + InterleavedGradient(float2(pixel))) * (1.0 / 16.0) + blockShift + frameShift);
    float cursor = tStart;
    float transmittance = 1.0;
    float3 radiance = 0.0;

    // Empty-space leaping with a safety net: after a run of empty samples the march
    // leaps ahead; if the leap lands inside cloud it backs up to where the leap began
    // and marches that stretch finely, so a leap can never skip a cloud edge.
    float leapOrigin = -1.0;
    uint fineSteps = 0u;
    float cachedSunDepth = 0.0;
    bool cachedValid = false;
    float weightSum = 0.0;
    float distanceSum = 0.0;

    [loop]
    for (uint i = 0u; i < kMaxSteps && cursor < tEnd && transmittance > kOpaqueCutoff; ++i)
    {
        const float stepLength = min(max(max(kBaseStep, cursor * g.lod.x * kFootprintStep), (cursor - tStart) * growth), kMaxStep);
        const float cursorBefore = cursor;
        // The jitter must stay inside the interval this sample actually represents. Inside
        // cloud that interval is the (capped) in-cloud step, not the coarse empty-space
        // step; jittering over the coarse step put samples kilometres ahead of the
        // cursor, re-counting the same cloud again and again as horizontal ribs.
        // The span must be the step the sample really represents: inside cloud that is the
        // in-cloud step below, which a coarse schedule step stretches (kInCloudScheduleFraction).
        // A narrower span puts the samples of neighbouring rays at nearly the same depths, which
        // shows as horizontal terraces on cloud flanks once the temporal resolve has converged.
        const float inCloudSpan = max(clamp(75.0 + 0.006 * cursor, 75.0, kMaxStep), kInCloudScheduleFraction * stepLength);
        const float jitterSpan = min(stepLength, inCloudSpan);
        const float t = cursor + jitter * jitterSpan;
        cursor += stepLength;
        if (fineSteps > 0u)
        {
            --fineSteps;
        }
        if (t >= tEnd)
        {
            break;
        }
        const float3 position = origin + direction * t;
        const float radius = length(position);

        const float4 column = ColumnData(position / max(radius, 1.0));
        // Thin high layer (cirrus / anvil, 0.70-0.90 of the shell): a coarse step can hop
        // straight across it, which bands it. Land the step on the layer edge and
        // take at least 8 steps across its thickness while inside.
        float stepCap = stepLength;
        if (column.y > 0.01)
        {
            const float layerThickness = max(g.shell.z - g.shell.y, 1.0);
            const float heightFraction = (radius - g.shell.x - g.shell.y) / layerThickness;
            const float slope = dot(direction, position / max(radius, 1.0)) / layerThickness;
            const float absSlope = max(abs(slope), 1.0e-7);
)" R"(            if (heightFraction >= 0.70 && heightFraction <= 0.90)
            {
                // Vertical advance of at most 1/8 of the layer, and never more than a few
                // hundred metres: a near-horizontal ray inside a thin dense slab (an anvil
                // seen edge-on) otherwise takes km-long steps and bands the slab.
                // The anvil thins towards its rim (down to ~45 % of the layer), so take the
                // 8 steps across the thinnest part, or the thin sheet is hit by 1-2 samples.
                const float thin = lerp(0.45, 1.0, saturate(column.y * 1.5)) * 0.5;
                stepCap = min(stepLength, min(0.2 * thin / absSlope / 8.0, 300.0 + 0.012 * t));
            }
            else if (heightFraction < 0.70 && slope > 0.0)
            {
                stepCap = min(stepLength, (0.70 - heightFraction) / slope + 20.0);
            }
            else if (heightFraction > 0.90 && slope < 0.0)
            {
                stepCap = min(stepLength, (0.90 - heightFraction) / slope + 20.0);
            }
        }
        cursor = cursorBefore + stepCap;
        gRelief = 1.0;
        const float sigma = Extinction(column, position, radius, t * g.lod.x);
        // Relief (crevices between billows) is fine, high-contrast noise. When the camera is
        // inside or against the cloud a pixel sees one random point of it, so the relief shows
        // as static grain: fade it in with distance, where a pixel averages over many billows.
        const float relief = lerp(1.0, gRelief, SmoothRange(150.0, 900.0, t));
        if (sigma <= 0.0)
        {
            if (column.x <= 1.0e-4 && column.y <= 1.0e-4 && fineSteps == 0u)
            {
                // Nothing here: leap ahead instead of marching empty space finely.
                // Only the most recent leap can hide a cloud edge: the sample it
                // started from was empty, so remember where this leap began.
                leapOrigin = cursorBefore;
                cursor += min(stepLength * (kEmptySkip - 1.0), 4000.0);
            }
            continue;
        }
        if (leapOrigin >= 0.0)
        {
            // The leap landed inside cloud: go back and march it finely.
            cursor = leapOrigin + min(max(max(kBaseStep, leapOrigin * g.lod.x * kFootprintStep), (leapOrigin - tStart) * growth), kMaxStep);
            leapOrigin = -1.0;
            fineSteps = 6u;
            continue;
        }

        // Inside cloud the step is capped (the reference clouds use 75 m + 0.006 x
        // distance, up to 1.5 km), so billow edges are sampled finely enough; the
        // coarse schedule above is only for crossing empty space.
        const float inCloudStep = clamp(75.0 + 0.006 * t, 75.0, kMaxStep);
        const float actualStep = min(stepCap, max(inCloudStep, kInCloudScheduleFraction * stepLength));
        cursor = cursorBefore + actualStep;

        const float h = saturate((radius - g.shell.x - g.shell.y) / max(g.shell.z - g.shell.y, 1.0));
        // Far away the exact sun march is wasted (the billows are filtered anyway):
        // blend to a column estimate of the optical depth above this point.
        const float farBlend = SmoothRange(45.0, 140.0, t * g.lod.x);
        float sunDepth = cachedSunDepth;
        // Thin samples (cirrus, cloud edges) contribute little: reuse the last full sun
        // march of this ray instead of marching again for each of them.
        const bool thinSample = sigma * actualStep < 0.02;
        if (farBlend < 1.0 && !(thinSample && cachedValid))
        {
            // Jittered over most of a step: without jitter the six sun-march steps show as terraces
            // on cloud flanks. The grain it adds is averaged out by the temporal resolve.
            // Beyond the explicit steps (6.7 km) the light volume knows the rest of the way to the sun.
            float volumeTail = -1.0;
            float volumeTau;
            if (VolumeTau(position + sun * 6700.0, volumeTau))
            {
                volumeTail = volumeTau;
            }
            sunDepth = SunOpticalDepth(position, sun, outer, t * g.lod.x, 0.1 + 0.8 * frac(jitter * 7.31 + 0.37), volumeTail);
            cachedSunDepth = sunDepth;
            cachedValid = true;
        }
        if (farBlend > 0.0)
        {
            const float4 estimateType = TypeParams(column.z);
            const float heightFraction = saturate(h / max(estimateType.x, 0.05));
            const float slant = 1.0 / clamp(dot(position / max(radius, 1.0), sun), 0.2, 1.0);
            const float estimate = g.lod.z * column.x * 0.55 * (1.0 - heightFraction) * slant;
            sunDepth = lerp(sunDepth, estimate, farBlend);
        }
        const float skyFacing = saturate(dot(position / max(radius, 1.0), sun) * 0.5 + 0.5);
        // Multiple scattering as octaves (energy, extinction and anisotropy each x0.5 per
        // octave): the highest octave decays slowly, so the inside of a thick tower keeps
        // a soft glow instead of going black.
        // Diffusion: light reaching deep inside a thick cloud falls off like sqrt(depth)
        // rather than exponentially, which is what keeps a storm tower white inside.
        const float multiDepth = 1.6 * sqrt(sunDepth);
        float multiScatter = 0.0;
        float octave = 1.0;
        [unroll]
        for (uint n = 0u; n < 4u; ++n)
        {
            multiScatter += octave *
                (3.0 * Hg(cosine, 0.2 * octave) + 0.3 * Hg(cosine, -0.4 * octave)) *
                exp(-multiDepth * 0.8 * octave);
            octave *= 0.5;
        }
        const float multi = singlePhase * 12.0 * exp(-sunDepth * 1.6) + 0.35 * multiScatter;
        // Sky light from above: tops of towers and billows catch it, the inside and the base
        // do not (the cloud's own optical depth above the sample shades it).
        const float ambient = (0.02 + 0.16 * h * h) * skyFacing * (0.35 + 0.65 * exp(-sunDepth * 0.12));
        // Precipitating cloud is greyer: more absorption, less in-scattered light.
        const float sampleAlbedo = albedo * (1.0 - 0.4 * column.w);
        // Soft-limit the forward-scattering peak so a limb or terminator view does
        // not blow out to white.
        const float lit = multi + ambient;
        const float limited = lit / (1.0 + 0.25 * lit);
        // Thick cores are brighter, thin edges and low-lying cloud darker, which
        // keeps the weather readable from far away where the billows are filtered.
        const float thickness01 = saturate(max(column.x, column.y * smoothstep(0.69, 0.72, h)) / 0.8);
        // A rain-bearing storm has a dark, flat base: the rain-cooled air under the updraft
        // is thick, grey and shaded by the whole cloud above it.
        const float baseDark = lerp(1.0, 0.3, (1.0 - smoothstep(0.0, 0.2, h)) * saturate(0.5 + column.w));
        const float shading = (0.62 + 0.38 * thickness01) * baseDark;
        const float3 source =
            irradiance * sampleAlbedo * atmosphereSun * limited * 4.6 * shading * relief;

        const float stepTransmittance = exp(-sigma * actualStep);
        const float contribution = transmittance * (1.0 - stepTransmittance);
        radiance += contribution * source;
        weightSum += contribution;
        distanceSum += contribution * t;
        transmittance *= stepTransmittance;
    }

    // The march stops once the cloud is nearly opaque. Whatever transmittance is left at that
    // point is the unmarched remainder, not real see-through: drop it, or the (very bright,
    // HDR) atmosphere behind a close opaque cloud still shows through at that fraction.
    transmittance = saturate((transmittance - kOpaqueCutoff) / (1.0 - kOpaqueCutoff));

    // Soft shoulder: a wide sheet seen at a grazing angle accumulates many steps of
    // in-scattered light and would clip to flat white. Compress towards a ceiling
    // that tracks the sun brightness, keeping structure in the bright range.
    const float ceiling = max(irradiance * 0.5, 1.0e-4);
    radiance = radiance / (1.0 + radiance / ceiling);

    // Aerial perspective over the camera-to-cloud distance (the cloud is composited after
    // the atmosphere pass, which only knows the terrain/sky behind it): attenuate the cloud
    // by the air in front of it and add the single-scattered in-scatter of that stretch.
    if (weightSum > 1.0e-4 && g.atmR.w > 0.0)
    {
        // Same single + multiple scattering integral as the atmosphere pass (sun transmittance
        // and multi-scattering LUTs sampled per step), so a cloud fades into exactly the haze
        // the terrain/sky behind it received. Geometric steps keep the near air finely sampled
        // while a cloud 100+ km away is still covered.
        // Distance the cloud's light actually comes from: the opacity-weighted mean along the ray.
        // (An analytic distance to the middle of the shell looked smoother, but from inside the
        // layer it is ~200 km for any ray near the horizon, which hazed close clouds away.)
        // The per-frame march jitter varies it a little; the temporal resolve averages that out.
        const float meanDistance = clamp(distanceSum / weightSum, tStart, max(tEnd, tStart));
        gCloudDistance = meanDistance;
        const float rayleighPhase = 3.0 / (16.0 * kPi) * (1.0 + cosine * cosine);
        const float miePhase = (1.0 - g.atmG.w * g.atmG.w) /
            (4.0 * kPi * pow(max(1.0 + g.atmG.w * g.atmG.w - 2.0 * g.atmG.w * cosine, 1.0e-4), 1.5));
        float3 aerial = 1.0;
        float3 inscatter = 0.0;
        float segStart = 0.0;
        [loop]
        for (uint a = 0u; a < 16u; ++a)
        {
            const float segEnd = meanDistance * pow(float(a + 1u) / 16.0, 2.0);
            const float aerialStep = segEnd - segStart;
            const float3 ap = origin + direction * (0.5 * (segStart + segEnd));
            const float apRadius = length(ap);
            const float alt = max(apRadius - g.shell.x, 0.0);
            const float3 rayleigh = g.atmR.rgb * exp(-alt / g.atmR.w);
            const float3 mie = g.atmG.rgb * exp(-alt / g.atmM.w);
            const float3 extinction = rayleigh + g.atmM.rgb * exp(-alt / g.atmM.w);
            const float2 lut = LutUv(apRadius, dot(ap / max(apRadius, 1.0), sun));
            const float3 sunT = g_transmittance.SampleLevel(g_transmittanceSampler, lut, 0).rgb;
            const float3 multi = g_multi.SampleLevel(g_multiSampler, lut, 0).rgb;
            const float3 source = irradiance *
                (sunT * (rayleigh * rayleighPhase + mie * miePhase) +
                 multi * (rayleigh + mie) / (4.0 * kPi));
            const float3 stepT = exp(-extinction * aerialStep);
            inscatter += aerial * source * (1.0 - stepT) / max(extinction, 1.0e-12);
            aerial *= stepT;
            segStart = segEnd;
        }
        radiance = radiance * aerial + (1.0 - transmittance) * inscatter;
    }
    return float4(radiance, transmittance);
}
)";

// Crepuscular rays. The atmosphere pass lights every view-ray sample as if the sun were
// unobstructed; here the direct in-scatter of the air that sits in cloud shadow is
// subtracted again (as negative radiance), so shafts appear in the air under and between
// clouds, over the terrain and the sky alike.
constexpr const char* kMarchGodrays = R"(
static const uint kGodrayStepCount = 40u;

float SunVisibility(float3 position, float3 sun)
{
    float volumeTau;
    if (VolumeTau(position, volumeTau))
    {
        return exp(-volumeTau);
    }
    const float radius = length(position);
    const float inner = g.shell.x + g.shell.y;
    const float outer = g.shell.x + g.shell.z;
    if (radius >= outer)
    {
        return 1.0;
    }
    float o0, o1;
    if (!RaySphere(position, sun, outer, o0, o1) || o1 <= 0.0)
    {
        return 1.0;
    }
    float segmentStart = 0.0;
    if (radius < inner)
    {
        float i0, i1;
        if (RaySphere(position, sun, inner, i0, i1) && i1 > 0.0)
        {
            segmentStart = i1;
        }
    }
    const float segmentEnd = min(o1, segmentStart + 60000.0);
    if (segmentEnd <= segmentStart)
    {
        return 1.0;
    }
    const float stepLength = (segmentEnd - segmentStart) / 6.0;
    float opticalDepth = 0.0;
    [loop]
    for (uint i = 0u; i < 6u; ++i)
    {
        const float3 samplePosition = position + sun * (segmentStart + (float(i) + 0.5) * stepLength);
        const float sampleRadius = length(samplePosition);
        opticalDepth +=
            Extinction(ColumnData(samplePosition / max(sampleRadius, 1.0)), samplePosition, sampleRadius, stepLength) *
            stepLength;
    }
    return exp(-opticalDepth);
}

float4 CloudWithGodrays(VSOutput input)
{
    const float4 cloud = CloudMarch(input);
    if (g.atmR.w <= 0.0 || g.temporal.y <= 0.0)
    {
        return cloud;
    }
    const int2 pixel = int2(input.position.xy);
    const float3 origin = g.cameraAspect.xyz;
    const float3 direction = ViewRay(input.uv);
    const float3 sun = normalize(g.sunFar.xyz);
    const float cosine = dot(direction, sun);

    // View-ray extent: the surface (depth buffer), else the atmosphere exit, capped.
    float tTop0, tTop1;
    if (!RaySphere(origin, direction, g.atmosphere.y, tTop0, tTop1) || tTop1 <= 0.0)
    {
        return cloud;
    }
    float rayEnd = min(tTop1, 60000.0);
    uint depthWidth, depthHeight;
    g_depth.GetDimensions(depthWidth, depthHeight);
    const int2 depthBase = int2(float2(pixel) * g.atmosphere.w);
    const int2 depthMax = int2(depthWidth - 1u, depthHeight - 1u);
    const float depth = max(
        max(g_depth.Load(int3(min(depthBase, depthMax), 0)).r,
            g_depth.Load(int3(min(depthBase + int2(1, 0), depthMax), 0)).r),
        max(g_depth.Load(int3(min(depthBase + int2(0, 1), depthMax), 0)).r,
            g_depth.Load(int3(min(depthBase + int2(1, 1), depthMax), 0)).r));
    if (depth > 0.0)
    {
        rayEnd = min(rayEnd, ReverseZViewDepth(depth) / max(dot(direction, normalize(g.forwardTanHalfFov.xyz)), 1.0e-5));
    }
    float g0, g1;
    if (RaySphere(origin, direction, g.shell.x, g0, g1) && g1 > 0.0 && g0 > 0.0)
    {
        rayEnd = min(rayEnd, g0);
    }
    if (rayEnd <= 0.0)
    {
        return cloud;
    }

    // Distance of the cloud on this ray (from the march): in-scatter behind it is hidden by it.
    const float cloudDistance = gCloudDistance;

    const float rayleighPhase = 3.0 / (16.0 * kPi) * (1.0 + cosine * cosine);
    const float miePhase = (1.0 - g.atmG.w * g.atmG.w) /
        (4.0 * kPi * pow(max(1.0 + g.atmG.w * g.atmG.w - 2.0 * g.atmG.w * cosine, 1.0e-4), 1.5));
    const float irradiance = max(g.optics.z, 0.0);
    // Blue-noise offset (tile appended to the noise buffer after the 32^3 voxels), shifted by a
    // golden-ratio step per frame so the temporal resolve sees a new well-spread offset each time.
    const uint blueTexel = (uint(pixel.y) & 63u) * 64u + (uint(pixel.x) & 63u);
    const float blue = asfloat(g_noise.Load(32u * 32u * 32u * 16u + blueTexel * 4u));
    const float jitter = frac(blue + frac(g.temporal.x * 0.6180339887));

    float3 aerial = 1.0;
    float3 loss = 0.0;
    [loop]
    for (uint a = 0u; a < kGodrayStepCount; ++a)
    {
        const float u0 = float(a) / float(kGodrayStepCount);
        const float u1 = float(a + 1u) / float(kGodrayStepCount);
        const float segStart = rayEnd * u0 * u0;
        const float segEnd = rayEnd * u1 * u1;
        const float stepLength = segEnd - segStart;
        const float tm = lerp(segStart, segEnd, jitter);
        const float3 ap = origin + direction * tm;
        const float apRadius = length(ap);
        const float alt = max(apRadius - g.shell.x, 0.0);
        const float3 rayleigh = g.atmR.rgb * exp(-alt / g.atmR.w);
        const float3 mie = g.atmG.rgb * exp(-alt / g.atmM.w);
        const float3 extinction = rayleigh + g.atmM.rgb * exp(-alt / g.atmM.w);
        const float3 stepT = exp(-extinction * stepLength);
        if (alt < g.shell.z)
        {
            const float visibility = SunVisibility(ap, sun);
            if (visibility < 0.999)
            {
                const float2 lut = LutUv(apRadius, dot(ap / max(apRadius, 1.0), sun));
                const float3 sunT = g_transmittance.SampleLevel(g_transmittanceSampler, lut, 0).rgb;
                const float3 direct = irradiance * sunT * (rayleigh * rayleighPhase + mie * miePhase) *
                    (1.0 - stepT) / max(extinction, 1.0e-12);
                const float hidden = tm > cloudDistance ? cloud.a : 1.0;
                loss += aerial * direct * (1.0 - visibility) * hidden;
            }
        }
        aerial *= stepT;
    }
    return float4(cloud.rgb - loss * g.temporal.y, cloud.a);
}

// Heat ramp for the optical depth towards the sun: clear -> blue -> cyan -> yellow -> red -> white.
float3 VolumeHeat(float tau)
{
    const float x = saturate(log2(1.0 + tau) / log2(1.0 + 24.0));
    const float3 c0 = float3(0.02, 0.04, 0.25);
    const float3 c1 = float3(0.0, 0.7, 0.9);
    const float3 c2 = float3(1.0, 0.9, 0.1);
    const float3 c3 = float3(0.9, 0.1, 0.05);
    const float3 c4 = float3(1.0, 1.0, 1.0);
    if (x < 0.25) { return lerp(c0, c1, x / 0.25); }
    if (x < 0.5) { return lerp(c1, c2, (x - 0.25) / 0.25); }
    if (x < 0.75) { return lerp(c2, c3, (x - 0.5) / 0.25); }
    return lerp(c3, c4, (x - 0.75) / 0.25);
}

float4 main(VSOutput input) : SV_Target0
{
    const float4 result = CloudWithGodrays(input);
    if (g.volumeDebug < 0.0)
    {
        // Depth view (volume debug altitude < 0): the scene depth buffer as view-space distance,
        // log scaled with the heat ramp (1 m blue ... 1000 km white); magenta where the depth
        // buffer holds no geometry (never written / cleared). Shows what the terrain pass left behind.
        uint depthWidth, depthHeight;
        g_depth.GetDimensions(depthWidth, depthHeight);
        const int2 depthPixel = int2(input.uv * float2(depthWidth, depthHeight));
        const float rawDepth = g_depth.Load(int3(min(depthPixel, int2(depthWidth - 1u, depthHeight - 1u)), 0)).r;
        if (rawDepth <= 0.0)
        {
            return float4(1.0, 0.0, 1.0, 0.0);
        }
        const float distanceMeters = ReverseZViewDepth(rawDepth);
        const float scaled = saturate(log2(max(distanceMeters, 1.0)) / log2(1.0e6));
        return float4(VolumeHeat(scaled * 24.0), 0.0);
    }
    if (g.volumeDebug <= 0.0)
    {
        return result;
    }
    // Light volume debug: a horizontal slice of the volume at altitude g.volumeDebug, drawn as a
    // sheet in the world. Heat = optical depth towards the sun (blue clear, white opaque), magenta =
    // inside a cascade but the voxel is not ready (black = never written, lime = old generation), nothing = outside every cascade.
    const float3 origin = g.cameraAspect.xyz;
    const float3 direction = ViewRay(input.uv);
    const float sliceRadius = g.shell.x + g.volumeDebug;
    float s0;
    float s1;
    if (!RaySphere(origin, direction, sliceRadius, s0, s1))
    {
        return result;
    }
    const float t = length(origin) > sliceRadius ? s0 : s1;
    if (t <= 0.0)
    {
        return result;
    }
    // Hidden by the terrain?
    const int2 pixel = int2(input.position.xy);
    uint depthWidth, depthHeight;
    g_depth.GetDimensions(depthWidth, depthHeight);
    const int2 depthBase = int2(float2(pixel) * g.atmosphere.w);
    const float depth = g_depth.Load(int3(min(depthBase, int2(depthWidth - 1u, depthHeight - 1u)), 0)).r;
    if (depth > 0.0)
    {
        const float surfaceT = ReverseZViewDepth(depth) / max(dot(direction, normalize(g.forwardTanHalfFov.xyz)), 1.0e-5);
        if (surfaceT < t)
        {
            return result;
        }
    }
    float tau;
    uint cascade;
    const uint status = VolumeDebugSample(origin + direction * t, tau, cascade);
    if (status == 0u)
    {
        return result;
    }
    float3 color = VolumeHeat(tau);
    if ((status & 7u) == 1u)
    {
        // another cell: R = generation matches, G = x tag matches, B = y tag matches (dark when none)
        color = float3((status & 8u) != 0u ? 1.0 : 0.15, (status & 16u) != 0u ? 1.0 : 0.15, (status & 32u) != 0u ? 1.0 : 0.15);
    }
    else if (status == 3u) { color = float3(0.0, 0.0, 0.0); }     // black: never written
    else if (status == 4u) { color = float3(0.8, 1.0, 0.0); }     // lime: right cell, old generation
    color *= (1.0 - 0.18 * float(cascade));
    const float alpha = 0.78;
    return float4(result.rgb * (1.0 - alpha) + color * alpha, result.a * (1.0 - alpha));
}
)";

constexpr const char* kShadowMain = R"(float4 main(VSOutput input) : SV_Target0
{
    // Sun transmittance through the cloud shell at the surface seen by this
    // pixel, from the same density field the camera march draws.
    const float4 lit = float4(1.0, 1.0, 1.0, 1.0);
    const int2 pixel = int2(input.position.xy);
    uint depthWidth, depthHeight;
    g_depth.GetDimensions(depthWidth, depthHeight);
    const int2 depthBase = int2(float2(pixel) * g.atmosphere.w);
    const int2 depthMax = int2(depthWidth - 1u, depthHeight - 1u);
    const float depth = max(
        max(g_depth.Load(int3(min(depthBase, depthMax), 0)).r,
            g_depth.Load(int3(min(depthBase + int2(1, 0), depthMax), 0)).r),
        max(g_depth.Load(int3(min(depthBase + int2(0, 1), depthMax), 0)).r,
            g_depth.Load(int3(min(depthBase + int2(1, 1), depthMax), 0)).r));
    if (depth <= 0.0)
    {
        return lit;
    }

    const float3 origin = g.cameraAspect.xyz;
    const float3 direction = ViewRay(input.uv);
    const float surfaceT =
        ReverseZViewDepth(depth) / max(dot(direction, normalize(g.forwardTanHalfFov.xyz)), 1.0e-5);
    const float3 surface = origin + direction * surfaceT;
    const float3 sun = normalize(g.sunFar.xyz);
    const float radius = length(surface);
    const float inner = g.shell.x + g.shell.y;
    const float outer = g.shell.x + g.shell.z;
    if (radius >= outer || dot(surface / max(radius, 1.0), sun) <= 0.0)
    {
        return lit;
    }

    // Altitude-stratified march (see AltitudeSunTau): the shadow of a tower or an anvil lands where the
    // sun ray crosses it, however far that is at a low sun. No per-pixel jitter: exp(-depth)
    // amplifies it into peppered shadow edges.
    return float4(exp(-AltitudeSunTau(surface, sun, 24u)), 0.0, 0.0, 1.0);
}
)";

[[nodiscard]] std::string MarchSource()
{
    return std::string(kCloudCommon) + kCloudCommonB + kCloudCommonC + kCloudCommonD + kCloudCommonE + kCloudCommonF + kCloudLife + kVolumeMath + kDepthBindings + kMarchBindings + kVolumeRead + kVolumeDebugRead + kMarchMain + kMarchGodrays;
}

[[nodiscard]] std::string ShadowSource()
{
    return std::string(kCloudCommon) + kCloudCommonB + kCloudCommonC + kCloudCommonD + kCloudCommonE + kCloudCommonF + kCloudLife + kDepthBindings + kShadowMain;
}

[[nodiscard]] std::string VolumeSource()
{
    return std::string(kCloudCommon) + kCloudCommonB + kCloudCommonC + kCloudCommonD + kCloudCommonE + kCloudCommonF + kCloudLife + kVolumeMath + kVolumeCompute;
}


constexpr const char* kCompositePixelShader = R"(
struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[[vk::binding(0, 0)]] [[vk::combinedImageSampler]] Texture2D g_clouds;
[[vk::binding(0, 0)]] [[vk::combinedImageSampler]] SamplerState g_cloudsSampler;

// Alpha blend turns (L / (1 - T), 1 - T) into L + T * scene.
float4 main(VSOutput input) : SV_Target0
{
    // Four bilinear taps a half texel off-centre: a 3x3 tent filter that hides
    // the march's per-pixel grain at the cost of a single pass.
    uint cloudWidth, cloudHeight;
    g_clouds.GetDimensions(cloudWidth, cloudHeight);
    const float2 o = 0.5 / float2(cloudWidth, cloudHeight);
    const float4 tent = 0.25 * (
        g_clouds.SampleLevel(g_cloudsSampler, input.uv + float2(-o.x, -o.y), 0) +
        g_clouds.SampleLevel(g_cloudsSampler, input.uv + float2( o.x, -o.y), 0) +
        g_clouds.SampleLevel(g_cloudsSampler, input.uv + float2(-o.x,  o.y), 0) +
        g_clouds.SampleLevel(g_cloudsSampler, input.uv + float2( o.x,  o.y), 0));
    // Mostly tent: with the march's stratified jitter it averages the grain away; the
    // march target is already reduced resolution, so edges stay as sharp as they were.
    const float4 c = lerp(g_clouds.SampleLevel(g_cloudsSampler, input.uv, 0), tent, 0.8);
    // c.rgb can be negative: the in-scatter the atmosphere pass added for air that sits in
    // cloud shadow is taken back out (god rays). Alpha blending can only reach it through a
    // non-zero alpha, so clear air gets a tiny floor (0.1% of the scene) to carry it.
    const float coverage = saturate(1.0 - c.a);
    if (coverage <= 1.0e-4 && all(abs(c.rgb) < 1.0e-7))
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    const float alpha = max(coverage, 1.0e-3);
    return float4(c.rgb / alpha, alpha);
}
)";

// Temporal resolve of the cloud march. The march jitters its sample positions differently
// every frame; this pass reprojects the previous result onto the current camera and blends
// it in, so the per-frame sampling noise averages out (a half-resolution march then reads
// as a much finer image). Reprojection goes through the cloud shell, so camera translation
// is compensated as well as rotation; the history is clamped to the current 3x3
// neighbourhood so changing weather and disocclusion never leave ghosts.
constexpr const char* kResolvePixelShader = R"(
struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

struct ResolveConstants
{
    float4 curPosShell;        // current camera position (planet-centred, m), cloud shell radius
    float4 curForwardTan;      // current forward, tan(vfov/2)
    float4 curUpAspect;        // current up, aspect
    float4 prevPosValid;       // previous camera position, 1 when the history is usable
    float4 prevForwardTan;
    float4 prevUpAspect;
    float4 params;             // x: weight of the current frame
};

[[vk::push_constant]] ResolveConstants r;

[[vk::binding(0, 0)]] [[vk::combinedImageSampler]] Texture2D g_current;
[[vk::binding(0, 0)]] [[vk::combinedImageSampler]] SamplerState g_currentSampler;
[[vk::binding(1, 0)]] [[vk::combinedImageSampler]] Texture2D g_history;
[[vk::binding(1, 0)]] [[vk::combinedImageSampler]] SamplerState g_historySampler;

float3 RayDirection(float2 uv, float3 forwardIn, float3 upIn, float tanHalf, float aspect)
{
    const float3 forward = normalize(forwardIn);
    const float3 right = normalize(cross(forward, normalize(upIn)));
    const float3 up = cross(right, forward);
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    return normalize(forward + right * (ndc.x * aspect * tanHalf) + up * (ndc.y * tanHalf));
}

float4 main(VSOutput input) : SV_Target0
{
    uint width, height;
    g_current.GetDimensions(width, height);
    const int2 pixel = int2(input.position.xy);
    const int2 maxPixel = int2(width - 1u, height - 1u);
    const float4 current = g_current.Load(int3(pixel, 0));
    if (r.prevPosValid.w < 0.5)
    {
        return current;
    }

    float4 lo = current;
    float4 hi = current;
    [unroll]
    for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx)
        {
            const float4 s = g_current.Load(int3(clamp(pixel + int2(dx, dy), int2(0, 0), maxPixel), 0));
            lo = min(lo, s);
            hi = max(hi, s);
        }
    }

    const float3 origin = r.curPosShell.xyz;
    const float3 direction = RayDirection(input.uv, r.curForwardTan.xyz, r.curUpAspect.xyz, r.curForwardTan.w, r.curUpAspect.w);

    // Where this ray meets the cloud shell (far away when it misses, i.e. sky above the horizon).
    float t = 3.0e5;
    const float b = dot(origin, direction);
    const float3 perpendicular = origin - b * direction;
    const float h2 = r.curPosShell.w * r.curPosShell.w - dot(perpendicular, perpendicular);
    if (h2 > 0.0)
    {
        const float h = sqrt(h2);
        const float t0 = -b - h;
        const float t1 = -b + h;
        t = t0 > 0.0 ? t0 : (t1 > 0.0 ? t1 : t);
    }

    const float3 toPoint = origin + direction * t - r.prevPosValid.xyz;
    const float3 prevForward = normalize(r.prevForwardTan.xyz);
    const float3 prevRight = normalize(cross(prevForward, normalize(r.prevUpAspect.xyz)));
    const float3 prevUp = cross(prevRight, prevForward);
    const float depth = dot(toPoint, prevForward);
    if (depth <= 1.0e-3)
    {
        return current;
    }
    const float2 ndc = float2(
        dot(toPoint, prevRight) / (depth * r.prevForwardTan.w * r.prevUpAspect.w),
        dot(toPoint, prevUp) / (depth * r.prevForwardTan.w));
    const float2 previousUv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (any(previousUv < 0.0) || any(previousUv > 1.0))
    {
        return current;
    }

    float4 history = g_history.SampleLevel(g_historySampler, previousUv, 0);
    if (any(isnan(history)) || any(isinf(history)))
    {
        return current;
    }
    history = clamp(history, lo, hi);
    return lerp(history, current, r.params.x);
}
)";

constexpr u32 kNoiseGrid = 32U;
constexpr u32 kBlueNoiseSize = 64U;

// The layer's base..top describes low and mid level cloud. Cumulonimbus towers and
// cirrus need to reach higher, so the march shell is stretched to 2.2x the layer
// thickness above the base (about 12.5 km for the default 1.5-6.5 km layer).
[[nodiscard]] f64 ExtendedTopAltitude(
    const CloudLayerParameters& layer,
    const f64 heightScale = 1.0) noexcept
{
    return layer.baseAltitudeMeters +
        2.2 * (layer.topAltitudeMeters - layer.baseAltitudeMeters) * heightScale;
}

[[nodiscard]] std::array<u32, 12> LabConstants(const CloudLab& lab) noexcept
{
    const auto bits = [](const f64 value) { return std::bit_cast<u32>(static_cast<f32>(value)); };
    if (!lab.enabled)
    {
        return {0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U};
    }
    return {
        bits(lab.centerDirection.x), bits(lab.centerDirection.y), bits(lab.centerDirection.z),
        bits(std::max(lab.radiusMeters, 100.0F)),
        bits(lab.type), bits(lab.coverage), bits(lab.cirrus), bits(lab.precipitation),
        bits(lab.maturity), bits(lab.organisation), bits(lab.density),
        bits(static_cast<f64>(lab.seed % 9973U))};
}

[[nodiscard]] u32 HashCell(u32 x, u32 y, u32 z, const u32 salt) noexcept
{
    u32 h = x * 0x8DA6B343U ^ y * 0xD8163841U ^ z * 0xCB1AB31FU ^ salt * 0x165667B1U;
    h ^= h >> 15U;
    h *= 0x2C1B3C6DU;
    h ^= h >> 12U;
    h *= 0x297A2D39U;
    h ^= h >> 15U;
    return h;
}

// Tileable cellular (worley) noise: distance to the nearest feature point of a
// jittered grid with 'cells' cells per axis, wrapped so the volume repeats.
[[nodiscard]] f32 Worley(
    const u32 x,
    const u32 y,
    const u32 z,
    const u32 cells,
    const u32 salt) noexcept
{
    const f32 scale = static_cast<f32>(cells) / static_cast<f32>(kNoiseGrid);
    const f32 px = (static_cast<f32>(x) + 0.5F) * scale;
    const f32 py = (static_cast<f32>(y) + 0.5F) * scale;
    const f32 pz = (static_cast<f32>(z) + 0.5F) * scale;
    const i32 cx = static_cast<i32>(std::floor(px));
    const i32 cy = static_cast<i32>(std::floor(py));
    const i32 cz = static_cast<i32>(std::floor(pz));
    const i32 n = static_cast<i32>(cells);

    f32 best = 4.0F;
    for (i32 dz = -1; dz <= 1; ++dz)
    {
        for (i32 dy = -1; dy <= 1; ++dy)
        {
            for (i32 dx = -1; dx <= 1; ++dx)
            {
                const i32 gx = cx + dx;
                const i32 gy = cy + dy;
                const i32 gz = cz + dz;
                const u32 h = HashCell(
                    static_cast<u32>(((gx % n) + n) % n),
                    static_cast<u32>(((gy % n) + n) % n),
                    static_cast<u32>(((gz % n) + n) % n),
                    salt);
                const f32 fx = static_cast<f32>(gx) + static_cast<f32>(h & 0xFFU) / 255.0F;
                const f32 fy = static_cast<f32>(gy) + static_cast<f32>((h >> 8U) & 0xFFU) / 255.0F;
                const f32 fz = static_cast<f32>(gz) + static_cast<f32>((h >> 16U) & 0xFFU) / 255.0F;
                const f32 ex = fx - px;
                const f32 ey = fy - py;
                const f32 ez = fz - pz;
                best = std::min(best, ex * ex + ey * ey + ez * ez);
            }
        }
    }
    return std::sqrt(best);
}

// Tileable blue-noise rank tile (void and cluster, simplified: the initial pattern is relaxed
// to an even spread, then ranks are handed out by filling the largest void each time).
// Values are ranks in [0, 1), one per texel, row-major kBlueNoiseSize x kBlueNoiseSize.
[[nodiscard]] std::vector<f32> BakeBlueNoise()
{
    constexpr u32 n = kBlueNoiseSize;
    constexpr std::size_t count = static_cast<std::size_t>(n) * n;
    constexpr f32 sigma = 1.9F;
    constexpr i32 radius = 6;

    std::vector<f32> kernel(static_cast<std::size_t>(2 * radius + 1) * (2 * radius + 1));
    for (i32 dy = -radius; dy <= radius; ++dy)
    {
        for (i32 dx = -radius; dx <= radius; ++dx)
        {
            kernel[static_cast<std::size_t>(dy + radius) * (2 * radius + 1) + (dx + radius)] =
                std::exp(-static_cast<f32>(dx * dx + dy * dy) / (2.0F * sigma * sigma));
        }
    }

    std::vector<f32> energy(count, 0.0F);
    std::vector<u8> filled(count, 0U);
    const auto splat = [&](const std::size_t index, const f32 sign)
    {
        const i32 px = static_cast<i32>(index % n);
        const i32 py = static_cast<i32>(index / n);
        for (i32 dy = -radius; dy <= radius; ++dy)
        {
            for (i32 dx = -radius; dx <= radius; ++dx)
            {
                const std::size_t x = static_cast<std::size_t>((px + dx + static_cast<i32>(n)) % static_cast<i32>(n));
                const std::size_t y = static_cast<std::size_t>((py + dy + static_cast<i32>(n)) % static_cast<i32>(n));
                energy[y * n + x] += sign *
                    kernel[static_cast<std::size_t>(dy + radius) * (2 * radius + 1) + (dx + radius)];
            }
        }
    };
    const auto tightest = [&]
    {
        std::size_t best = 0U;
        f32 bestEnergy = -1.0e30F;
        for (std::size_t i = 0U; i < count; ++i)
        {
            if (filled[i] != 0U && energy[i] > bestEnergy) { bestEnergy = energy[i]; best = i; }
        }
        return best;
    };
    const auto largestVoid = [&]
    {
        std::size_t best = 0U;
        f32 bestEnergy = 1.0e30F;
        for (std::size_t i = 0U; i < count; ++i)
        {
            if (filled[i] == 0U && energy[i] < bestEnergy) { bestEnergy = energy[i]; best = i; }
        }
        return best;
    };

    // Deterministic initial pattern (~10% of the texels), then relax it.
    u32 state = 0x9E3779B9U;
    const auto next = [&state]
    {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        return state;
    };
    const std::size_t initial = count / 10U;
    for (std::size_t placed = 0U; placed < initial;)
    {
        const std::size_t index = next() % count;
        if (filled[index] == 0U)
        {
            filled[index] = 1U;
            splat(index, 1.0F);
            ++placed;
        }
    }
    for (u32 pass = 0U; pass < 2000U; ++pass)
    {
        const std::size_t cluster = tightest();
        filled[cluster] = 0U;
        splat(cluster, -1.0F);
        const std::size_t hole = largestVoid();
        filled[hole] = 1U;
        splat(hole, 1.0F);
        if (hole == cluster)
        {
            break;
        }
    }

    std::vector<f32> rank(count, 0.0F);
    // Phase 1: peel the pattern, tightest cluster first, ranks initial-1 down to 0.
    {
        auto energyBackup = energy;
        auto filledBackup = filled;
        for (std::size_t r = initial; r-- > 0U;)
        {
            const std::size_t cluster = tightest();
            filled[cluster] = 0U;
            splat(cluster, -1.0F);
            rank[cluster] = static_cast<f32>(r);
        }
        energy = std::move(energyBackup);
        filled = std::move(filledBackup);
    }
    // Phase 2: fill the largest void each time, ranks initial..count-1.
    for (std::size_t r = initial; r < count; ++r)
    {
        const std::size_t hole = largestVoid();
        filled[hole] = 1U;
        splat(hole, 1.0F);
        rank[hole] = static_cast<f32>(r);
    }
    for (auto& value : rank)
    {
        value = (value + 0.5F) / static_cast<f32>(count);
    }
    return rank;
}

// R: base shape (fbm of inverted worley), G/B/A: erosion octaves, coarse to fine.
[[nodiscard]] std::vector<f32> BakeCloudNoise()
{
    const std::size_t voxels =
        static_cast<std::size_t>(kNoiseGrid) * kNoiseGrid * kNoiseGrid;
    std::vector<f32> data(voxels * 4U);
    std::array<f32, 4> low{1.0e9F, 1.0e9F, 1.0e9F, 1.0e9F};
    std::array<f32, 4> high{-1.0e9F, -1.0e9F, -1.0e9F, -1.0e9F};

    for (u32 z = 0U; z < kNoiseGrid; ++z)
    {
        for (u32 y = 0U; y < kNoiseGrid; ++y)
        {
            for (u32 x = 0U; x < kNoiseGrid; ++x)
            {
                const f32 w3 = 1.0F - std::min(Worley(x, y, z, 3U, 1U), 1.0F);
                const f32 w6 = 1.0F - std::min(Worley(x, y, z, 6U, 2U), 1.0F);
                const f32 w12 = 1.0F - std::min(Worley(x, y, z, 12U, 3U), 1.0F);
                const std::size_t index =
                    ((static_cast<std::size_t>(z) * kNoiseGrid + y) * kNoiseGrid + x) * 4U;
                data[index + 0U] = 0.6F * w3 + 0.3F * w6 + 0.1F * w12;
                data[index + 1U] = w3;
                data[index + 2U] = w6;
                data[index + 3U] = w12;
                for (std::size_t c = 0U; c < 4U; ++c)
                {
                    low[c] = std::min(low[c], data[index + c]);
                    high[c] = std::max(high[c], data[index + c]);
                }
            }
        }
    }

    // Stretch each channel to [0, 1] so the shader's thresholds are meaningful.
    for (std::size_t v = 0U; v < voxels; ++v)
    {
        for (std::size_t c = 0U; c < 4U; ++c)
        {
            const f32 range = std::max(high[c] - low[c], 1.0e-6F);
            data[v * 4U + c] = (data[v * 4U + c] - low[c]) / range;
        }
    }
    // The god-ray march jitters its samples with a blue-noise tile appended after the voxels
    // (it cannot be dithered away as cheaply as white noise: shafts amplify low frequencies).
    const auto blue = BakeBlueNoise();
    data.insert(data.end(), blue.begin(), blue.end());
    return data;
}

[[nodiscard]] u32 Bits(const f32 value) noexcept
{
    return std::bit_cast<u32>(value);
}
} // namespace

CloudRenderer::CloudRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler)
    : device_(&device)
{
    const auto vertex = compiler.Compile({
        .source = kVertexShader,
        .entryPoint = "main",
        .stage = shader::Stage::Vertex,
        .debug = false});
    const std::string marchSource = MarchSource();
    const auto pixel = compiler.Compile({
        .source = marchSource,
        .entryPoint = "main",
        .stage = shader::Stage::Pixel,
        .debug = false});

    pipeline_ = device.CreateGraphicsPipeline({
        .vertexShader = {.data = vertex.bytecode.data(), .size = vertex.bytecode.size()},
        .pixelShader = {.data = pixel.bytecode.data(), .size = pixel.bytecode.size()},
        .vertexAttributes = {},
        .vertexStrideBytes = 0U,
        .pushConstantDwords = 64U,
        .shaderResourceBuffers = 3U,
        .sampledTextures = 3U,
        .topology = rhi::PrimitiveTopology::TriangleList,
        .fillMode = rhi::FillMode::Solid,
        .cullMode = rhi::CullMode::None,
        .blendMode = rhi::BlendMode::Opaque,
        .depthTest = false,
        .depthWrite = false,
        .colorAttachmentFormats = {rhi::TextureFormat::RGBA16_Float},
        .colorAttachmentCount = 1U});

    const auto compositePixel = compiler.Compile({
        .source = kCompositePixelShader,
        .entryPoint = "main",
        .stage = shader::Stage::Pixel,
        .debug = false});
    compositePipeline_ = device.CreateGraphicsPipeline({
        .vertexShader = {.data = vertex.bytecode.data(), .size = vertex.bytecode.size()},
        .pixelShader = {.data = compositePixel.bytecode.data(), .size = compositePixel.bytecode.size()},
        .vertexAttributes = {},
        .vertexStrideBytes = 0U,
        .pushConstantDwords = 0U,
        .shaderResourceBuffers = 0U,
        .sampledTextures = 1U,
        .topology = rhi::PrimitiveTopology::TriangleList,
        .fillMode = rhi::FillMode::Solid,
        .cullMode = rhi::CullMode::None,
        .blendMode = rhi::BlendMode::Alpha,
        .depthTest = false,
        .depthWrite = false,
        .colorAttachmentFormats = {rhi::TextureFormat::RGBA16_Float},
        .colorAttachmentCount = 1U});

    const auto resolvePixel = compiler.Compile({
        .source = kResolvePixelShader,
        .entryPoint = "main",
        .stage = shader::Stage::Pixel,
        .debug = false});
    resolvePipeline_ = device.CreateGraphicsPipeline({
        .vertexShader = {.data = vertex.bytecode.data(), .size = vertex.bytecode.size()},
        .pixelShader = {.data = resolvePixel.bytecode.data(), .size = resolvePixel.bytecode.size()},
        .vertexAttributes = {},
        .vertexStrideBytes = 0U,
        .pushConstantDwords = 28U,
        .shaderResourceBuffers = 0U,
        .sampledTextures = 2U,
        .topology = rhi::PrimitiveTopology::TriangleList,
        .fillMode = rhi::FillMode::Solid,
        .cullMode = rhi::CullMode::None,
        .blendMode = rhi::BlendMode::Opaque,
        .depthTest = false,
        .depthWrite = false,
        .colorAttachmentFormats = {rhi::TextureFormat::RGBA16_Float},
        .colorAttachmentCount = 1U});

    const std::string shadowSource = ShadowSource();
    const auto shadowPixel = compiler.Compile({
        .source = shadowSource,
        .entryPoint = "main",
        .stage = shader::Stage::Pixel,
        .debug = false});
    shadowPipeline_ = device.CreateGraphicsPipeline({
        .vertexShader = {.data = vertex.bytecode.data(), .size = vertex.bytecode.size()},
        .pixelShader = {.data = shadowPixel.bytecode.data(), .size = shadowPixel.bytecode.size()},
        .vertexAttributes = {},
        .vertexStrideBytes = 0U,
        .pushConstantDwords = 64U,
        .shaderResourceBuffers = 3U,
        .sampledTextures = 1U,
        .topology = rhi::PrimitiveTopology::TriangleList,
        .fillMode = rhi::FillMode::Solid,
        .cullMode = rhi::CullMode::None,
        .blendMode = rhi::BlendMode::Opaque,
        .depthTest = false,
        .depthWrite = false,
        .colorAttachmentFormats = {rhi::TextureFormat::RGBA16_Float},
        .colorAttachmentCount = 1U});

    const std::string volumeSource = VolumeSource();
    const auto volumeShader = compiler.Compile({
        .source = volumeSource,
        .entryPoint = "main",
        .stage = shader::Stage::Compute,
        .debug = false});
    volumePipeline_ = device.CreateComputePipeline({
        .computeShader = {.data = volumeShader.bytecode.data(), .size = volumeShader.bytecode.size()},
        .pushConstantDwords = 64U,
        .shaderResourceBuffers = 3U,
        .storageTextures = 0U,
        .sampledTextures = 0U});
    const auto noise = BakeCloudNoise();
    noise_ = device.CreateBuffer({
        .sizeBytes = static_cast<u64>(noise.size() * sizeof(f32)),
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::ShaderResource});
    if (!noise_)
    {
        throw std::runtime_error("Failed to allocate cloud noise volume.");
    }
    std::memcpy(noise_->Map(), noise.data(), noise.size() * sizeof(f32));
    noise_->Unmap();
}

CloudRenderer::~CloudRenderer() = default;

std::unique_ptr<CloudRenderer::LightVolume> CloudRenderer::CreateLightVolume() const
{
    auto volume = std::make_unique<LightVolume>();
    // 3 cascades x 32 layers x 96 x 96 voxels of 16 bytes (tau, tag, tag, generation).
    volume->buffer = device_->CreateBuffer({
        .sizeBytes = 3ULL * 32ULL * 96ULL * 96ULL * 16ULL,
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::GpuOnly,
        .initialState = rhi::ResourceState::ShaderResource});
    if (!volume->buffer)
    {
        throw std::runtime_error("Failed to allocate the cloud light volume.");
    }

    // ORBIT_CLOUD_VOLUME_ALWAYS_REFRESH=1 turns the settled-input skip off, restoring the
    // original refresh-every-frame schedule (for A/B timing and bug isolation).
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, "ORBIT_CLOUD_VOLUME_ALWAYS_REFRESH") == 0 && value != nullptr)
    {
        volume->allowRefreshSkip = value[0] != '1';
        std::free(value);
    }
    return volume;
}

namespace
{
// Push constants shared by the march and the light-volume update (one layout, see kCloudCommon).
[[nodiscard]] std::array<u32, 64> BuildMarchConstants(
    const u32 depthWidth,
    const u32 width,
    const u32 height,
    const u32 faceResolution,
    const f64 referenceRadiusMeters,
    const CloudLayerParameters& layer,
    const celestial_atmosphere::AtmosphereParameters& atmosphere,
    const celestial_atmosphere::AtmosphereRenderView& view,
    const CloudLab& lab,
    const u32 frameIndex,
    const f32 godrayStrength,
    const bool volumeActive,
    const u32 volumeGeneration,
    const math::Double3& volumeAnchor,
    const f32 volumeDebugAltitude)
{
    const auto f = [](const f64 value) { return Bits(static_cast<f32>(value)); };

    const std::array<u32, 32> baseConstants{
        f(view.cameraPositionMeters.x),
        f(view.cameraPositionMeters.y),
        f(view.cameraPositionMeters.z),
        Bits(static_cast<f32>(width) / static_cast<f32>(height)),

        Bits(view.forward.x), Bits(view.forward.y), Bits(view.forward.z),
        Bits(std::tan(view.verticalFovRadians * 0.5F)),

        Bits(view.up.x), Bits(view.up.y), Bits(view.up.z),
        Bits(std::max(view.nearPlaneMeters, 1.0e-5F)),

        Bits(view.sunDirection.x), Bits(view.sunDirection.y), Bits(view.sunDirection.z),
        Bits(std::max(view.farPlaneMeters, view.nearPlaneMeters + 1.0e-4F)),

        f(referenceRadiusMeters),
        f(layer.baseAltitudeMeters),
        f(ExtendedTopAltitude(layer, lab.enabled ? static_cast<f64>(lab.heightScale) : 1.0)),
        Bits(static_cast<f32>(faceResolution)),

        f(std::clamp(layer.singleScatteringAlbedo, 0.0, 1.0)),
        f(std::clamp(layer.anisotropy, -0.95, 0.95)),
        Bits(std::max(view.irradianceScale, 0.0F)),
        f(0.6),

        f(atmosphere.bottomRadiusMeters),
        f(atmosphere.topRadiusMeters),
        f(layer.detailScale),
        Bits(static_cast<f32>(depthWidth) / static_cast<f32>(width)),

        Bits(2.0F * std::tan(view.verticalFovRadians * 0.5F) / static_cast<f32>(height)),
        f(std::clamp(0.52 - layer.coverageBias * 0.35, 0.05, 0.95)),
        f(layer.peakOpticalDepth * (lab.enabled ? static_cast<f64>(lab.heightScale) : 1.0)),
        f(layer.densityExponent)};

    std::array<u32, 64> constants{};
    std::copy(baseConstants.begin(), baseConstants.end(), constants.begin());
    const auto labBits = LabConstants(lab);
    std::copy(labBits.begin(), labBits.end(), constants.begin() + 32);
    constants[58] = std::bit_cast<u32>(lab.enabled ? lab.cirrusSheet : 0.0F);
    {
        const auto fb = [](const f64 value) { return Bits(static_cast<f32>(value)); };
        constants[44] = fb(atmosphere.rayleighScatteringPerMeter.x);
        constants[45] = fb(atmosphere.rayleighScatteringPerMeter.y);
        constants[46] = fb(atmosphere.rayleighScatteringPerMeter.z);
        constants[47] = fb(atmosphere.rayleighScaleHeightMeters);
        constants[48] = fb(atmosphere.mieExtinctionPerMeter.x);
        constants[49] = fb(atmosphere.mieExtinctionPerMeter.y);
        constants[50] = fb(atmosphere.mieExtinctionPerMeter.z);
        constants[51] = fb(atmosphere.mieScaleHeightMeters);
        constants[52] = fb(atmosphere.mieScatteringPerMeter.x);
        constants[53] = fb(atmosphere.mieScatteringPerMeter.y);
        constants[54] = fb(atmosphere.mieScatteringPerMeter.z);
        constants[55] = fb(std::clamp(atmosphere.mieAnisotropy, -0.99, 0.99));
        // Wrapped so the float stays exact; only the golden-ratio step matters.
        constants[56] = fb(static_cast<f64>(frameIndex % 4096U));
        constants[57] = fb(static_cast<f64>(std::clamp(godrayStrength, 0.0F, 4.0F)));
    }
    // Light volume: generation (so a re-anchored volume never trusts old voxels) and anchor.
    constants[59] = f(static_cast<f64>(volumeGeneration));
    if (volumeActive)
    {
        constants[60] = f(volumeAnchor.x);
        constants[61] = f(volumeAnchor.y);
        constants[62] = f(volumeAnchor.z);
    }
    constants[63] = f(volumeDebugAltitude);
    return constants;
}

// Every voxel refreshes at least once in any 12 consecutive frames: a valid voxel every 6th
// (12th for the far cascade), a missing one every 3rd (see kVolumeCompute). The schedule uses
// frameIndex % 4096, and 4096 is not a multiple of 12, so a window that straddles that wrap sees
// the residues in two runs. Two full cycles guarantee one run is at least 12 frames long, which
// covers every residue wherever the wrap falls.
constexpr u32 kLightVolumeSettleFrames = 24U;

// Hash of everything the light-volume compute shader reads, so a frame whose inputs are exactly
// the previous frame's can be recognised. The dwords are the Constants members reachable from
// the shader's main(): camera position, sun direction, shell, optics.w, atmosphere.z,
// lod.yzw, the cloud lab, temporal.z (lab cirrus), temporal.w (voxel generation) and the
// anchor. temporal.x (the frame index) only schedules which voxels refresh, so it is left out.
// View orientation, field of view, the atmosphere scattering terms and the debug altitude are
// not read by this shader and are left out too. The voxels also depend on the cloud field and
// on the volume buffer itself, so their identities are mixed in.
[[nodiscard]] u64 LightVolumeInputFingerprint(
    const std::array<u32, 64>& constants,
    const GpuCloudFieldProduct& field,
    const rhi::Buffer& volumeBuffer)
{
    static constexpr std::array<u32, 29> kReadDwords{
        0U, 1U, 2U,                                           // cameraAspect.xyz
        12U, 13U, 14U,                                        // sunFar.xyz
        16U, 17U, 18U, 19U,                                   // shell
        23U,                                                  // optics.w
        26U,                                                  // atmosphere.z
        29U, 30U, 31U,                                        // lod.yzw
        32U, 33U, 34U, 35U, 36U, 37U, 38U, 39U, 40U, 41U, 42U, 43U, // lab, labParams, labLife
        58U, 59U,                                             // temporal.z, temporal.w
    };

    u64 hash = 14695981039346656037ULL;
    const auto mix = [&hash](const u64 value)
    {
        hash ^= value;
        hash *= 1099511628211ULL;
    };
    for (const u32 index : kReadDwords)
    {
        mix(constants[index]);
    }
    mix(constants[60]);
    mix(constants[61]);
    mix(constants[62]);
    mix(field.Fingerprint());
    mix(static_cast<u64>(reinterpret_cast<std::uintptr_t>(&volumeBuffer)));
    return hash;
}
} // namespace

void CloudRenderer::Draw(
    rhi::CommandList& commands,
    const LightVolume& volume,
    rhi::Texture& depth,
    celestial_atmosphere::GpuAtmosphereLuts& luts,
    GpuCloudFieldProduct& field,
    rhi::Texture& target,
    const u32 width,  // size of the (reduced resolution) cloud target
    const u32 height,
    const f64 referenceRadiusMeters,
    const CloudLayerParameters& layer,
    const celestial_atmosphere::AtmosphereParameters& atmosphere,
    const celestial_atmosphere::AtmosphereRenderView& view,
    const CloudLab& lab,
    const u32 frameIndex,
    const f32 godrayStrength,
    const f32 volumeDebugAltitude)
{
    if (width == 0U || height == 0U || field.LayerCount() == 0U ||
        field.FaceResolution() < 2U)
    {
        return;
    }

    const auto constants = BuildMarchConstants(
        depth.Width(),
        width,
        height,
        field.FaceResolution(),
        referenceRadiusMeters,
        layer,
        atmosphere,
        view,
        lab,
        frameIndex,
        godrayStrength,
        volume.active,
        volume.generation,
        volume.anchor,
        volumeDebugAltitude);


    commands.SetRenderTarget(target);
    commands.SetViewport({
        .x = 0.0F, .y = 0.0F,
        .width = static_cast<f32>(width), .height = static_cast<f32>(height),
        .minDepth = 0.0F, .maxDepth = 1.0F});
    commands.SetScissor({
        .left = 0, .top = 0,
        .right = static_cast<i32>(width), .bottom = static_cast<i32>(height)});

    commands.SetGraphicsPipeline(*pipeline_);
    commands.SetGraphicsConstants(constants);
    commands.SetGraphicsBuffer(0, field.Buffer());
    commands.SetGraphicsBuffer(1, *noise_);
    commands.SetGraphicsBuffer(2, *volume.buffer);
    commands.SetGraphicsTexture(0, depth);
    commands.SetGraphicsTexture(1, luts.Transmittance());
    commands.SetGraphicsTexture(2, luts.MultiScattering());
    commands.Draw(6);
}

void CloudRenderer::UpdateLightVolume(
    rhi::CommandList& commands,
    LightVolume& volume,
    GpuCloudFieldProduct& field,
    const f64 referenceRadiusMeters,
    const CloudLayerParameters& layer,
    const celestial_atmosphere::AtmosphereParameters& atmosphere,
    const celestial_atmosphere::AtmosphereRenderView& view,
    const CloudLab& lab,
    const u32 frameIndex,
    const bool enabled)
{
    volume.active = false;
    volume.refreshSkipped = false;
    if (!enabled || field.LayerCount() == 0U || field.FaceResolution() < 2U ||
        !volumePipeline_ || !volume.buffer)
    {
        // Frames without a dispatch break the "every voxel refreshed within the last N frames"
        // guarantee the skip below relies on.
        volume.inputFingerprint = 0U;
        volume.settledFrames = 0U;
        return;
    }

    const math::Double3 camera = view.cameraPositionMeters;
    const f64 cameraLength = std::sqrt(camera.x * camera.x + camera.y * camera.y + camera.z * camera.z);
    if (cameraLength < 1.0)
    {
        volume.inputFingerprint = 0U;
        volume.settledFrames = 0U;
        return;
    }
    const math::Double3 direction{camera.x / cameraLength, camera.y / cameraLength, camera.z / cameraLength};

    // The volume's tangent frame stays put while the camera moves within ~250 km of it (cells
    // are addressed toroidally, so moving costs only the new edge rows); farther than that the
    // frame is re-anchored and a new generation invalidates every voxel.
    bool reanchor = !volume.anchorSet;
    if (!reanchor)
    {
        const f64 cosine = std::clamp(
            volume.anchor.x * direction.x + volume.anchor.y * direction.y + volume.anchor.z * direction.z,
            -1.0,
            1.0);
        reanchor = std::acos(cosine) * referenceRadiusMeters > 250000.0;
    }
    if (reanchor)
    {
        volume.anchor = direction;
        volume.anchorSet = true;
        volume.generation = (volume.generation + 1U) % 1000000U;
    }
    volume.active = true;

    const auto constants = BuildMarchConstants(
        1U,
        1U,
        1U,
        field.FaceResolution(),
        referenceRadiusMeters,
        layer,
        atmosphere,
        view,
        lab,
        frameIndex,
        1.0F,
        true,
        volume.generation,
        volume.anchor,
        0.0F);

    // The voxels are a pure function of the inputs hashed here, and the shader refreshes every
    // voxel at least once in kLightVolumeSettleFrames consecutive dispatches. So once the inputs
    // have been identical for that many dispatched frames, every voxel already holds the result
    // for them and another refresh would recompute bit-identical values: skip it. Any change
    // (sun, camera, weather, parameters, a re-anchor) restarts the count, and the original
    // refresh schedule runs unchanged until it settles again.
    const u64 fingerprint = LightVolumeInputFingerprint(constants, field, *volume.buffer);
    if (fingerprint != volume.inputFingerprint)
    {
        volume.inputFingerprint = fingerprint;
        volume.settledFrames = 0U;
    }
    else if (volume.settledFrames < kLightVolumeSettleFrames)
    {
        ++volume.settledFrames;
    }

    if (volume.allowRefreshSkip && volume.settledFrames >= kLightVolumeSettleFrames)
    {
        volume.refreshSkipped = true;
        return;
    }

    commands.Transition(*volume.buffer, rhi::ResourceState::ShaderResource, rhi::ResourceState::UnorderedAccess);
    commands.SetComputePipeline(*volumePipeline_);
    commands.SetComputeConstants(constants);
    commands.SetComputeBuffer(0U, field.Buffer());
    commands.SetComputeBuffer(1U, *noise_);
    commands.SetComputeBuffer(2U, *volume.buffer);
    commands.Dispatch(96U / 8U, 96U / 8U, 3U * 32U);
    commands.UavBarrier(*volume.buffer);
    commands.Transition(*volume.buffer, rhi::ResourceState::UnorderedAccess, rhi::ResourceState::ShaderResource);
}

void CloudRenderer::DrawShadow(
    rhi::CommandList& commands,
    rhi::Texture& depth,
    GpuCloudFieldProduct& field,
    rhi::Texture& target,
    const u32 width,
    const u32 height,
    const f64 referenceRadiusMeters,
    const CloudLayerParameters& layer,
    const celestial_atmosphere::AtmosphereRenderView& view,
    const CloudLab& lab)
{
    if (width == 0U || height == 0U || field.LayerCount() == 0U ||
        field.FaceResolution() < 2U)
    {
        return;
    }

    const auto f = [](const f64 value) { return Bits(static_cast<f32>(value)); };

    const std::array<u32, 32> baseConstants{
        f(view.cameraPositionMeters.x),
        f(view.cameraPositionMeters.y),
        f(view.cameraPositionMeters.z),
        Bits(static_cast<f32>(width) / static_cast<f32>(height)),

        Bits(view.forward.x), Bits(view.forward.y), Bits(view.forward.z),
        Bits(std::tan(view.verticalFovRadians * 0.5F)),

        Bits(view.up.x), Bits(view.up.y), Bits(view.up.z),
        Bits(std::max(view.nearPlaneMeters, 1.0e-5F)),

        Bits(view.sunDirection.x), Bits(view.sunDirection.y), Bits(view.sunDirection.z),
        Bits(std::max(view.farPlaneMeters, view.nearPlaneMeters + 1.0e-4F)),

        f(referenceRadiusMeters),
        f(layer.baseAltitudeMeters),
        f(ExtendedTopAltitude(layer, lab.enabled ? static_cast<f64>(lab.heightScale) : 1.0)),
        Bits(static_cast<f32>(field.FaceResolution())),

        0U, 0U, 0U, f(0.6),

        0U, 0U,
        f(layer.detailScale),
        Bits(static_cast<f32>(depth.Width()) / static_cast<f32>(width)),

        0U,
        f(std::clamp(0.52 - layer.coverageBias * 0.35, 0.05, 0.95)),
        f(layer.peakOpticalDepth * (lab.enabled ? static_cast<f64>(lab.heightScale) : 1.0)),
        f(layer.densityExponent)};

    std::array<u32, 64> constants{};
    std::copy(baseConstants.begin(), baseConstants.end(), constants.begin());
    const auto labBits = LabConstants(lab);
    std::copy(labBits.begin(), labBits.end(), constants.begin() + 32);
    constants[58] = std::bit_cast<u32>(lab.enabled ? lab.cirrusSheet : 0.0F);
    commands.SetRenderTarget(target);
    commands.SetViewport({
        .x = 0.0F, .y = 0.0F,
        .width = static_cast<f32>(width), .height = static_cast<f32>(height),
        .minDepth = 0.0F, .maxDepth = 1.0F});
    commands.SetScissor({
        .left = 0, .top = 0,
        .right = static_cast<i32>(width), .bottom = static_cast<i32>(height)});

    commands.SetGraphicsPipeline(*shadowPipeline_);
    commands.SetGraphicsConstants(constants);
    commands.SetGraphicsBuffer(0, field.Buffer());
    commands.SetGraphicsBuffer(1, *noise_);
    commands.SetGraphicsBuffer(2, *noise_);   // slot 2 is unused by the shadow shader
    commands.SetGraphicsTexture(0, depth);
    commands.Draw(6);
}

void CloudRenderer::Resolve(
    rhi::CommandList& commands,
    rhi::Texture& current,
    rhi::Texture& history,
    rhi::Texture& target,
    const u32 width,
    const u32 height,
    const f64 shellRadiusMeters,
    const CloudResolveView& now,
    const CloudResolveView& previous,
    const bool historyValid,
    const f32 currentWeight)
{
    if (width == 0U || height == 0U)
    {
        return;
    }

    const auto f = [](const f64 value) { return Bits(static_cast<f32>(value)); };
    const auto fill = [&f](const CloudResolveView& v, const f64 w0, std::array<u32, 4>& position,
                           std::array<u32, 4>& forward, std::array<u32, 4>& up)
    {
        position = {f(v.cameraPositionMeters.x), f(v.cameraPositionMeters.y), f(v.cameraPositionMeters.z), f(w0)};
        forward = {Bits(v.forward.x), Bits(v.forward.y), Bits(v.forward.z),
                   Bits(std::tan(v.verticalFovRadians * 0.5F))};
        up = {Bits(v.up.x), Bits(v.up.y), Bits(v.up.z), Bits(v.aspect)};
    };

    std::array<u32, 4> curPos{}, curForward{}, curUp{}, prevPos{}, prevForward{}, prevUp{};
    fill(now, shellRadiusMeters, curPos, curForward, curUp);
    fill(previous, historyValid ? 1.0 : 0.0, prevPos, prevForward, prevUp);

    std::array<u32, 28> constants{};
    const auto put = [&constants](const std::size_t slot, const std::array<u32, 4>& values)
    {
        std::copy(values.begin(), values.end(), constants.begin() + static_cast<std::ptrdiff_t>(slot * 4U));
    };
    put(0U, curPos);
    put(1U, curForward);
    put(2U, curUp);
    put(3U, prevPos);
    put(4U, prevForward);
    put(5U, prevUp);
    put(6U, {Bits(std::clamp(currentWeight, 0.0F, 1.0F)), 0U, 0U, 0U});

    commands.SetRenderTarget(target);
    commands.SetViewport({
        .x = 0.0F, .y = 0.0F,
        .width = static_cast<f32>(width), .height = static_cast<f32>(height),
        .minDepth = 0.0F, .maxDepth = 1.0F});
    commands.SetScissor({
        .left = 0, .top = 0,
        .right = static_cast<i32>(width), .bottom = static_cast<i32>(height)});
    commands.SetGraphicsPipeline(*resolvePipeline_);
    commands.SetGraphicsConstants(constants);
    commands.SetGraphicsTexture(0, current);
    commands.SetGraphicsTexture(1, history);
    commands.Draw(6);
}

void CloudRenderer::Composite(
    rhi::CommandList& commands,
    rhi::Texture& clouds,
    rhi::Texture& target,
    const u32 width,
    const u32 height)
{
    if (width == 0U || height == 0U)
    {
        return;
    }

    commands.SetRenderTarget(target);
    commands.SetViewport({
        .x = 0.0F, .y = 0.0F,
        .width = static_cast<f32>(width), .height = static_cast<f32>(height),
        .minDepth = 0.0F, .maxDepth = 1.0F});
    commands.SetScissor({
        .left = 0, .top = 0,
        .right = static_cast<i32>(width), .bottom = static_cast<i32>(height)});
    commands.SetGraphicsPipeline(*compositePipeline_);
    commands.SetGraphicsTexture(0, clouds);
    commands.Draw(6);
}
} // namespace orbit::celestial_clouds
