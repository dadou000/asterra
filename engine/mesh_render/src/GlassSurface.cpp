#include <orbit/mesh_render/GlassSurface.hpp>

#include <orbit/lighting/SdfTraceShader.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <string>

namespace orbit::mesh_render
{
namespace
{
constexpr u64 kRecordRetireTicks = 24U;
constexpr u32 kGlassPushDwords = 28U;
constexpr u32 kMinPhotonGrid = 96U;
constexpr u32 kMaxPhotonGrid = 224U;

// Constants and record layout shared by every stage. Push block (float4s):
//   c0 = camera right.xyz, tan(half fov) * aspect
//   c1 = camera up.xyz, tan(half fov)
//   c2 = camera forward.xyz, near plane
//   c3 = far plane, instance index, width, height
//   c4 = unit vector toward the sun, photon grid size
//   c5 = sun irradiance rgb (already scaled by sun visibility), instance count
//   c6 = environment radiance rgb, -
// Record (6 float4 per instance): three rows of the local-to-camera-relative
// transform, tint + IOR, half extents + shape id, spare.
constexpr const char* kCommon = R"(
struct Constants
{
    float4 c0;
    float4 c1;
    float4 c2;
    float4 c3;
    float4 c4;
    float4 c5;
    float4 c6;
};
[[vk::push_constant]] Constants g;

[[vk::binding(0, 0)]] StructuredBuffer<float4> g_records : register(t0);

struct Inst
{
    float4 r0;
    float4 r1;
    float4 r2;
    float3 tint;
    float ior;
    float3 halfSize;
    uint shape;
};

Inst LoadInst(uint index)
{
    Inst inst;
    const uint b = index * 6u;
    inst.r0 = g_records[b];
    inst.r1 = g_records[b + 1u];
    inst.r2 = g_records[b + 2u];
    const float4 v3 = g_records[b + 3u];
    const float4 v4 = g_records[b + 4u];
    inst.tint = v3.xyz;
    inst.ior = v3.w;
    inst.halfSize = v4.xyz;
    inst.shape = (uint)(v4.w + 0.5);
    return inst;
}

float3 InstOrigin(Inst inst)
{
    return float3(inst.r0.w, inst.r1.w, inst.r2.w);
}

float3 ToLocalDir(Inst inst, float3 v)
{
    return float3(
        dot(v, float3(inst.r0.x, inst.r1.x, inst.r2.x)),
        dot(v, float3(inst.r0.y, inst.r1.y, inst.r2.y)),
        dot(v, float3(inst.r0.z, inst.r1.z, inst.r2.z)));
}

float3 ToWorldDir(Inst inst, float3 v)
{
    return float3(dot(inst.r0.xyz, v), dot(inst.r1.xyz, v), dot(inst.r2.xyz, v));
}

float3 ToLocal(Inst inst, float3 p)
{
    return ToLocalDir(inst, p - InstOrigin(inst));
}

// Entry/exit of a ray against a convex body: ray = o + t d, t may be negative
// when the origin is inside. Normals are outward, in local space.
struct Span
{
    bool hit;
    float tIn;
    float tOut;
    float3 nIn;
    float3 nOut;
};

Span MissSpan()
{
    Span s;
    s.hit = false;
    s.tIn = 1.0e30;
    s.tOut = -1.0e30;
    s.nIn = float3(0.0, 1.0, 0.0);
    s.nOut = float3(0.0, 1.0, 0.0);
    return s;
}

Span IntersectSpans(Span a, Span b)
{
    if (!a.hit || !b.hit)
    {
        return MissSpan();
    }
    Span s;
    s.tIn = max(a.tIn, b.tIn);
    s.nIn = a.tIn > b.tIn ? a.nIn : b.nIn;
    s.tOut = min(a.tOut, b.tOut);
    s.nOut = a.tOut < b.tOut ? a.nOut : b.nOut;
    s.hit = s.tIn < s.tOut;
    return s;
}

Span UnionSpans(Span a, Span b)
{
    if (!a.hit)
    {
        return b;
    }
    if (!b.hit)
    {
        return a;
    }
    Span s;
    s.hit = true;
    s.tIn = min(a.tIn, b.tIn);
    s.nIn = a.tIn < b.tIn ? a.nIn : b.nIn;
    s.tOut = max(a.tOut, b.tOut);
    s.nOut = a.tOut > b.tOut ? a.nOut : b.nOut;
    return s;
}

Span SlabSpan(float oc, float dc, float hc, float3 axis)
{
    const float safe = abs(dc) < 1.0e-9 ? (dc < 0.0 ? -1.0e-9 : 1.0e-9) : dc;
    const float inv = 1.0 / safe;
    const float t0 = (-hc - oc) * inv;
    const float t1 = (hc - oc) * inv;
    Span s;
    s.tIn = min(t0, t1);
    s.tOut = max(t0, t1);
    s.nIn = dc > 0.0 ? -axis : axis;
    s.nOut = dc > 0.0 ? axis : -axis;
    s.hit = s.tIn < s.tOut;
    return s;
}

Span EllipsoidSpan(float3 o, float3 d, float3 h, float3 c)
{
    Span s = MissSpan();
    const float3 oo = (o - c) / h;
    const float3 dd = d / h;
    const float a = dot(dd, dd);
    const float b = dot(oo, dd);
    const float cc = dot(oo, oo) - 1.0;
    const float disc = b * b - a * cc;
    if (disc < 0.0 || a < 1.0e-20)
    {
        return s;
    }
    const float q = sqrt(disc);
    s.hit = true;
    s.tIn = (-b - q) / a;
    s.tOut = (-b + q) / a;
    const float3 pIn = o + d * s.tIn - c;
    const float3 pOut = o + d * s.tOut - c;
    s.nIn = normalize(pIn / (h * h));
    s.nOut = normalize(pOut / (h * h));
    return s;
}

// Infinite elliptic cylinder around the Y axis.
Span CylinderSideSpan(float3 o, float3 d, float hx, float hz)
{
    Span s = MissSpan();
    const float2 oo = o.xz / float2(hx, hz);
    const float2 dd = d.xz / float2(hx, hz);
    const float a = dot(dd, dd);
    const float b = dot(oo, dd);
    const float cc = dot(oo, oo) - 1.0;
    if (a < 1.0e-12)
    {
        if (cc < 0.0)
        {
            s.hit = true;
            s.tIn = -1.0e30;
            s.tOut = 1.0e30;
        }
        return s;
    }
    const float disc = b * b - a * cc;
    if (disc < 0.0)
    {
        return s;
    }
    const float q = sqrt(disc);
    s.hit = true;
    s.tIn = (-b - q) / a;
    s.tOut = (-b + q) / a;
    const float3 pIn = o + d * s.tIn;
    const float3 pOut = o + d * s.tOut;
    s.nIn = normalize(float3(pIn.x / (hx * hx), 0.0, pIn.z / (hz * hz)));
    s.nOut = normalize(float3(pOut.x / (hx * hx), 0.0, pOut.z / (hz * hz)));
    return s;
}

Span ShapeSpan(Inst inst, float3 o, float3 d)
{
    const float3 h = inst.halfSize;
    if (inst.shape == 1u)
    {
        return EllipsoidSpan(o, d, h, float3(0.0, 0.0, 0.0));
    }
    if (inst.shape == 2u)
    {
        return IntersectSpans(
            CylinderSideSpan(o, d, h.x, h.z),
            SlabSpan(o.y, d.y, h.y, float3(0.0, 1.0, 0.0)));
    }
    if (inst.shape == 3u)
    {
        const float r = min(h.x, h.z);
        const float shaft = max(h.y - r, 0.0);
        const Span body = IntersectSpans(
            CylinderSideSpan(o, d, r, r),
            SlabSpan(o.y, d.y, shaft, float3(0.0, 1.0, 0.0)));
        const Span top = EllipsoidSpan(o, d, float3(r, r, r), float3(0.0, shaft, 0.0));
        const Span bottom = EllipsoidSpan(o, d, float3(r, r, r), float3(0.0, -shaft, 0.0));
        return UnionSpans(UnionSpans(body, top), bottom);
    }
    // Box and plane (a thin box).
    return IntersectSpans(
        IntersectSpans(
            SlabSpan(o.x, d.x, h.x, float3(1.0, 0.0, 0.0)),
            SlabSpan(o.y, d.y, h.y, float3(0.0, 1.0, 0.0))),
        SlabSpan(o.z, d.z, h.z, float3(0.0, 0.0, 1.0)));
}

// Unpolarised dielectric Fresnel; eta = n_transmitted / n_incident.
float Fresnel(float cosi, float eta)
{
    const float sin2t = (1.0 - cosi * cosi) / (eta * eta);
    if (sin2t >= 1.0)
    {
        return 1.0;
    }
    const float cost = sqrt(1.0 - sin2t);
    const float rs = (cosi - eta * cost) / (cosi + eta * cost);
    const float rp = (eta * cosi - cost) / (eta * cosi + cost);
    return 0.5 * (rs * rs + rp * rp);
}
)";

// The path of one ray through a glass body: entry refraction, up to four
// segments inside (total internal reflection bounces), Beer-Lambert
// absorption, exit refraction.
constexpr const char* kTrace = R"(
struct GlassPath
{
    bool hit;     // the ray meets the body
    bool exits;   // a refracted ray leaves it (weight, pos, dir valid)
    bool inside;  // the origin was already inside
    float tIn;
    float3 nIn;   // outward world normal at the entry point
    float fresnelIn;
    float3 pos;   // where the ray leaves the glass
    float3 dir;   // its direction
    float3 weight; // transmitted fraction (absorption and both Fresnel terms)
};

GlassPath TraceGlass(Inst inst, float3 o, float3 d)
{
    GlassPath e;
    e.hit = false;
    e.exits = false;
    e.inside = false;
    e.tIn = 0.0;
    e.nIn = float3(0.0, 1.0, 0.0);
    e.fresnelIn = 0.0;
    e.pos = o;
    e.dir = d;
    e.weight = float3(0.0, 0.0, 0.0);

    const float3 lo = ToLocal(inst, o);
    const float3 ld = ToLocalDir(inst, d);
    const Span s = ShapeSpan(inst, lo, ld);
    if (!s.hit || s.tOut <= 1.0e-4)
    {
        return e;
    }

    e.hit = true;
    e.inside = s.tIn < 0.0;
    e.tIn = max(s.tIn, 0.0);
    e.nIn = normalize(ToWorldDir(inst, s.nIn));

    float3 pos = o + d * e.tIn;
    float3 dir = d;
    float3 weight = float3(1.0, 1.0, 1.0);
    if (!e.inside)
    {
        const float cosi = saturate(dot(-d, e.nIn));
        e.fresnelIn = Fresnel(cosi, inst.ior);
        dir = normalize(refract(d, e.nIn, 1.0 / inst.ior));
        weight *= 1.0 - e.fresnelIn;
    }

    const float3 tint = max(inst.tint, 0.001);
    [loop]
    for (uint bounce = 0u; bounce < 4u; ++bounce)
    {
        const Span si = ShapeSpan(inst, ToLocal(inst, pos), ToLocalDir(inst, dir));
        if (!si.hit || si.tOut <= 1.0e-5)
        {
            return e;
        }
        weight *= pow(tint, si.tOut);
        pos += dir * si.tOut;
        const float3 nOut = normalize(ToWorldDir(inst, si.nOut));
        const float cosi = saturate(dot(dir, nOut));
        const float3 refracted = refract(dir, -nOut, inst.ior);
        if (dot(refracted, refracted) < 1.0e-8)
        {
            dir = reflect(dir, nOut);
            pos -= nOut * 1.0e-3;
            continue;
        }
        weight *= 1.0 - Fresnel(cosi, 1.0 / inst.ior);
        e.pos = pos + nOut * 1.0e-3;
        e.dir = normalize(refracted);
        e.weight = weight;
        e.exits = true;
        return e;
    }
    return e;
}
)";

// Screen-space march against the depth buffer; shared by the glass pass and
// the caustic photons. Needs g_depth at binding 1.
constexpr const char* kScene = R"(
[[vk::binding(DEPTH_BINDING, 0)]] [[vk::combinedImageSampler]] Texture2D g_depth;
[[vk::binding(DEPTH_BINDING, 0)]] [[vk::combinedImageSampler]] SamplerState g_depthSampler;

float ViewZFromDepth(float depth)
{
    if (depth <= 0.0)
    {
        return 1.0e30;
    }
    const float n = g.c2.w;
    const float f = g.c3.x;
    return n * f / (depth * (f - n) + n);
}

float SceneViewZ(int2 pixel)
{
    return ViewZFromDepth(g_depth.Load(int3(pixel, 0)).r);
}

float3 PixelRay(float2 pixel)
{
    const float2 ndc = float2(
        pixel.x / g.c3.z * 2.0 - 1.0,
        1.0 - pixel.y / g.c3.w * 2.0);
    return g.c0.xyz * (ndc.x * g.c0.w) +
           g.c1.xyz * (ndc.y * g.c1.w) +
           g.c2.xyz;
}

// uv of a camera-relative point; z = view depth (<= near when behind).
float3 ProjectPoint(float3 p)
{
    const float z = dot(p, g.c2.xyz);
    const float zs = max(z, 1.0e-4);
    return float3(
        dot(p, g.c0.xyz) / (zs * g.c0.w) * 0.5 + 0.5,
        0.5 - dot(p, g.c1.xyz) / (zs * g.c1.w) * 0.5,
        z);
}

bool MarchScene(float3 p, float3 dir, out float tHit)
{
    tHit = 0.0;
    float t = 0.02;
    [loop]
    for (uint i = 0u; i < 56u; ++i)
    {
        const float stepLength = max(0.03, t * 0.10);
        const float tNext = t + stepLength;
        if (tNext > 80.0)
        {
            break;
        }
        const float3 q = ProjectPoint(p + dir * tNext);
        if (q.z <= g.c2.w || q.x < 0.0 || q.x >= 1.0 || q.y < 0.0 || q.y >= 1.0)
        {
            break;
        }
        const float sz = SceneViewZ(int2(q.xy * g.c3.zw));
        const float thickness = max(0.20, q.z * 0.05);
        if (q.z > sz && q.z < sz + thickness)
        {
            float lo = t;
            float hi = tNext;
            [unroll]
            for (uint k = 0u; k < 5u; ++k)
            {
                const float mid = 0.5 * (lo + hi);
                const float3 qm = ProjectPoint(p + dir * mid);
                const float szm = SceneViewZ(int2(clamp(qm.xy, 0.0, 0.9999) * g.c3.zw));
                if (qm.z > szm)
                {
                    hi = mid;
                }
                else
                {
                    lo = mid;
                }
            }
            tHit = hi;
            return true;
        }
        t = tNext;
    }
    return false;
}
)";

constexpr const char* kFullscreenVertex = R"(
float4 main(uint vertexId : SV_VertexID) : SV_Position
{
    const float2 p[3] = {float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)};
    return float4(p[vertexId], 0.0, 1.0);
}
)";

constexpr const char* kCopyPixel = R"(
[[vk::binding(0, 0)]] [[vk::combinedImageSampler]] Texture2D g_color;
[[vk::binding(0, 0)]] [[vk::combinedImageSampler]] SamplerState g_colorSampler;

float4 main(float4 position : SV_Position) : SV_Target0
{
    return g_color.Load(int3(int2(position.xy), 0));
}
)";

constexpr const char* kGlassPixel = R"(
[[vk::binding(BACKDROP_BINDING, 0)]] [[vk::combinedImageSampler]] Texture2D g_backdrop;
[[vk::binding(BACKDROP_BINDING, 0)]] [[vk::combinedImageSampler]] SamplerState g_backdropSampler;

// The merged mesh distance field (dummies when absent): what a ray finds once
// it leaves the screen. Read-only here; the parameters ride behind the
// instance records (origin + voxel size, dimensions + enable, local up).
#define SDF_DIST_CORNERS 1
[[vk::binding(1, 0)]] StructuredBuffer<uint4> g_sdfDist : register(t20);
[[vk::binding(2, 0)]] StructuredBuffer<uint> g_sdfAlbedo : register(t21);
[[vk::binding(3, 0)]] StructuredBuffer<uint> g_sdfNormal : register(t22);
[[vk::binding(4, 0)]] StructuredBuffer<float4> g_sdfRadiance : register(t23);
float4 SdfParams0() { return g_records[(uint)g.c5.w * 6u]; }
float4 SdfParams1() { return g_records[(uint)g.c5.w * 6u + 1u]; }
float4 SdfParams2() { return g_records[(uint)g.c5.w * 6u + 2u]; }
#define SDF_ORIGIN SdfParams0().xyz
#define SDF_VOXEL SdfParams0().w
#define SDF_DIMS int3(SdfParams1().xyz)
//SDF_TRACE_INCLUDE

// What a ray starting at p sees: the lit scene where the march meets the depth
// buffer, the sky pixel it points at, the lit surface the distance field finds
// beyond the screen, else the sky (upward) or a dim ground.
float3 EnvironmentTrace(float3 p, float3 dir)
{
    float t;
    if (MarchScene(p, dir, t))
    {
        const float3 q = ProjectPoint(p + dir * t);
        return g_backdrop.SampleLevel(g_backdropSampler, clamp(q.xy, 0.0, 0.9999), 0.0).rgb;
    }
    const float3 farPoint = ProjectPoint(p + dir * 4000.0);
    if (farPoint.z > g.c2.w && farPoint.x >= 0.0 && farPoint.x < 1.0 &&
        farPoint.y >= 0.0 && farPoint.y < 1.0 &&
        SceneViewZ(int2(farPoint.xy * g.c3.zw)) > 1.0e20)
    {
        return g_backdrop.SampleLevel(g_backdropSampler, farPoint.xy, 0.0).rgb;
    }
    if (SdfParams1().w > 0.5)
    {
        float3 sdfHit;
        float sdfT;
        if (SdfTrace(p, dir, 60.0, sdfHit, sdfT))
        {
            return SdfFetchRadiance(sdfHit, -dir);
        }
    }
    return dot(dir, SdfParams2().xyz) > -0.15 ? g.c6.rgb : g.c6.rgb * 0.05;
}

float4 main(float4 position : SV_Position) : SV_Target0
{
    const Inst inst = LoadInst((uint)g.c3.y);
    const float3 ray = PixelRay(position.xy);
    const float rayLength = length(ray);
    const float3 d = ray / rayLength;

    const GlassPath path = TraceGlass(inst, float3(0.0, 0.0, 0.0), d);
    if (!path.hit)
    {
        discard;
    }
    const float sceneDistance = SceneViewZ(int2(position.xy)) * rayLength;
    if (sceneDistance < path.tIn)
    {
        discard;
    }

    float3 color = float3(0.0, 0.0, 0.0);
    if (path.exits)
    {
        color += EnvironmentTrace(path.pos, path.dir) * path.weight;
    }
    if (!path.inside)
    {
        const float3 entry = d * path.tIn + path.nIn * 2.0e-3;
        color += EnvironmentTrace(entry, reflect(d, path.nIn)) * path.fresnelIn;
    }
    return float4(color, 1.0);
}
)";

constexpr const char* kCausticVertex = R"(
[[vk::binding(2, 0)]] [[vk::combinedImageSampler]] Texture2D g_baseRoughness;
[[vk::binding(2, 0)]] [[vk::combinedImageSampler]] SamplerState g_baseRoughnessSampler;
[[vk::binding(3, 0)]] [[vk::combinedImageSampler]] Texture2D g_normalMetallic;
[[vk::binding(3, 0)]] [[vk::combinedImageSampler]] SamplerState g_normalMetallicSampler;
[[vk::binding(4, 0)]] [[vk::combinedImageSampler]] Texture2D g_emission;
[[vk::binding(4, 0)]] [[vk::combinedImageSampler]] SamplerState g_emissionSampler;
[[vk::binding(5, 0)]] [[vk::combinedImageSampler]] Texture2D g_sunShadow;
[[vk::binding(5, 0)]] [[vk::combinedImageSampler]] SamplerState g_sunShadowSampler;

struct VSOutput
{
    float4 position : SV_Position;
    nointerpolation float3 hit : TEXCOORD0;
    nointerpolation float3 dir : TEXCOORD1;
    nointerpolation float3 power : TEXCOORD2;
    nointerpolation float sigma : TEXCOORD3;
};

VSOutput Degenerate()
{
    VSOutput o;
    o.position = float4(2.0, 2.0, 2.0, 1.0);
    o.hit = float3(0.0, 0.0, 0.0);
    o.dir = float3(0.0, 1.0, 0.0);
    o.power = float3(0.0, 0.0, 0.0);
    o.sigma = 1.0;
    return o;
}

float3 SunBasisU(float3 toSun)
{
    return normalize(cross(toSun, abs(toSun.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0)));
}

float3 PhotonOrigin(float3 toSun, float3 centre, float radius, float2 cell, float grid)
{
    const float3 u = SunBasisU(toSun);
    const float3 v = cross(toSun, u);
    const float2 ab = ((cell + 0.5) / grid * 2.0 - 1.0) * radius;
    return centre + u * ab.x + v * ab.y + toSun * (radius * 1.5);
}

GlassPath TracePhoton(Inst inst, float3 toSun, float3 centre, float radius, float2 cell, float grid)
{
    return TraceGlass(inst, PhotonOrigin(toSun, centre, radius, cell, grid), -toSun);
}

// 1 when the sun reaches `p` according to the mesh sun shadow map (or when no
// map is available / p lies outside its window), else 0.
float SunReaches(float3 p, float3 toSun)
{
    const uint base = (uint)g.c5.w * 6u;
    const float4 a = g_records[base];
    const float4 b = g_records[base + 1u];
    const float4 c = g_records[base + 2u];
    if (c.w < 0.5)
    {
        return 1.0;
    }
    const float3 q = p - a.xyz;
    const float2 window = float2(dot(q, b.xyz), dot(q, c.xyz)) / a.w;
    if (abs(window.x) >= 1.0 || abs(window.y) >= 1.0)
    {
        return 1.0;
    }
    const float mapSize = b.w;
    const float receiverDepth = (a.w - dot(q, toSun)) / (2.0 * a.w);
    const float2 t = float2(window.x * 0.5 + 0.5, 0.5 - window.y * 0.5) * mapSize;
    const float stored = g_sunShadow.Load(int3(int2(floor(t)), 0)).r;
    return stored >= receiverDepth - 2.0 / mapSize ? 1.0 : 0.0;
}

VSOutput main(uint vertexId : SV_VertexID)
{
    const Inst inst = LoadInst((uint)g.c3.y);
    const uint grid = (uint)g.c4.w;
    const uint photon = vertexId / 6u;
    const uint corner = vertexId % 6u;
    const float2 cell = float2((float)(photon % grid), (float)(photon / grid));

    const float3 toSun = g.c4.xyz;
    const float3 centre = InstOrigin(inst);
    const float radius = length(inst.halfSize) * 1.02;
    const float spacing = 2.0 * radius / (float)grid;

    const GlassPath center = TracePhoton(inst, toSun, centre, radius, cell, (float)grid);
    if (!center.exits)
    {
        return Degenerate();
    }
    const float reaches = SunReaches(PhotonOrigin(toSun, centre, radius, cell, (float)grid), toSun);
    if (reaches < 0.5)
    {
        return Degenerate();
    }

    float hitT;
    if (!MarchScene(center.pos, center.dir, hitT))
    {
        return Degenerate();
    }
    const float3 hit = center.pos + center.dir * hitT;
    const float3 hitUv = ProjectPoint(hit);
    const int2 pixel = int2(clamp(hitUv.xy, 0.0, 0.9999) * g.c3.zw);
    if (g_emission.Load(int3(pixel, 0)).a <= 0.0)
    {
        return Degenerate();
    }
    const float3 normal = normalize(g_normalMetallic.Load(int3(pixel, 0)).xyz);

    // The spread of neighbouring photons on the receiver plane sets how wide
    // this photon's footprint must be to blend into them.
    float spread = spacing * 2.0;
    {
        const GlassPath nx = TracePhoton(inst, toSun, centre, radius, cell + float2(1.0, 0.0), (float)grid);
        const GlassPath ny = TracePhoton(inst, toSun, centre, radius, cell + float2(0.0, 1.0), (float)grid);
        float measured = 0.0;
        if (nx.exits)
        {
            const float denom = dot(nx.dir, normal);
            if (abs(denom) > 0.05)
            {
                measured = max(measured, length(nx.pos + nx.dir * (dot(hit - nx.pos, normal) / denom) - hit));
            }
        }
        if (ny.exits)
        {
            const float denom = dot(ny.dir, normal);
            if (abs(denom) > 0.05)
            {
                measured = max(measured, length(ny.pos + ny.dir * (dot(hit - ny.pos, normal) / denom) - hit));
            }
        }
        if (measured > 0.0)
        {
            spread = measured;
        }
    }
    const float sigma = clamp(spread * 0.8, spacing * 0.7, 0.6);

    VSOutput o;
    o.hit = hit;
    o.dir = center.dir;
    o.sigma = sigma;
    o.power = g.c5.rgb * (spacing * spacing) * center.weight;

    const float2 offsets[6] = {
        float2(-1.0, -1.0), float2(1.0, -1.0), float2(-1.0, 1.0),
        float2(1.0, -1.0), float2(1.0, 1.0), float2(-1.0, 1.0)};
    const float extent = min(3.0 * sigma, 2.0);
    const float3 q = hit + (g.c0.xyz * offsets[corner].x + g.c1.xyz * offsets[corner].y) * extent;
    const float3 view = float3(dot(q, g.c0.xyz), dot(q, g.c1.xyz), dot(q, g.c2.xyz));
    if (view.z <= g.c2.w)
    {
        return Degenerate();
    }
    o.position = float4(view.x / g.c0.w, view.y / g.c1.w, 0.5 * view.z, view.z);
    return o;
}
)";

constexpr const char* kCausticPixel = R"(
[[vk::binding(2, 0)]] [[vk::combinedImageSampler]] Texture2D g_baseRoughness;
[[vk::binding(2, 0)]] [[vk::combinedImageSampler]] SamplerState g_baseRoughnessSampler;
[[vk::binding(3, 0)]] [[vk::combinedImageSampler]] Texture2D g_normalMetallic;
[[vk::binding(3, 0)]] [[vk::combinedImageSampler]] SamplerState g_normalMetallicSampler;
[[vk::binding(4, 0)]] [[vk::combinedImageSampler]] Texture2D g_emission;
[[vk::binding(4, 0)]] [[vk::combinedImageSampler]] SamplerState g_emissionSampler;

struct PSInput
{
    float4 position : SV_Position;
    nointerpolation float3 hit : TEXCOORD0;
    nointerpolation float3 dir : TEXCOORD1;
    nointerpolation float3 power : TEXCOORD2;
    nointerpolation float sigma : TEXCOORD3;
};

float4 main(PSInput input) : SV_Target0
{
    const int2 pixel = int2(input.position.xy);
    if (g_emission.Load(int3(pixel, 0)).a <= 0.0)
    {
        discard;
    }
    const float depth = g_depth.Load(int3(pixel, 0)).r;
    if (depth <= 0.0)
    {
        discard;
    }
    const float3 position = PixelRay(input.position.xy) * ViewZFromDepth(depth);
    const float3 delta = position - input.hit;
    const float distance2 = dot(delta, delta);
    const float sigma = input.sigma;
    if (distance2 > 9.0 * sigma * sigma)
    {
        discard;
    }

    const float4 baseRoughness = g_baseRoughness.Load(int3(pixel, 0));
    const float4 normalMetallic = g_normalMetallic.Load(int3(pixel, 0));
    const float cosine = saturate(dot(normalize(normalMetallic.xyz), -input.dir));
    const float kernel = exp(-0.5 * distance2 / (sigma * sigma)) / (6.2831853 * sigma * sigma);
    const float3 albedo = max(baseRoughness.rgb, 0.0) * (1.0 - saturate(normalMetallic.w));
    return float4(albedo * (1.0 / 3.14159265) * input.power * (cosine * kernel), 0.0);
}
)";

[[nodiscard]] std::string Compose(
    const std::initializer_list<const char*> parts,
    const std::string& defines = "#define DEPTH_BINDING 1\n#define BACKDROP_BINDING 2\n")
{
    std::string source = defines;
    for (const char* part : parts)
    {
        source += part;
    }
    const std::string marker = "//SDF_TRACE_INCLUDE";
    if (const auto at = source.find(marker); at != std::string::npos)
    {
        source.replace(at, marker.size(), lighting::kSdfTraceHlsl);
    }
    return source;
}

[[nodiscard]] std::array<u32, kGlassPushDwords> PackPush(
    const GlassCamera& camera,
    const u32 instanceIndex,
    const u32 width,
    const u32 height,
    const f32 photonGrid,
    const GlassLighting& lighting,
    const u32 instanceCount)
{
    const auto bits = [](const f32 value) { return std::bit_cast<u32>(value); };
    const f32 visibility = std::clamp(lighting.sunVisibility, 0.0F, 1.0F);
    return {
        bits(camera.right[0]), bits(camera.right[1]), bits(camera.right[2]),
        bits(camera.tanHalfX),
        bits(camera.up[0]), bits(camera.up[1]), bits(camera.up[2]),
        bits(camera.tanHalfY),
        bits(camera.forward[0]), bits(camera.forward[1]),
        bits(camera.forward[2]), bits(camera.nearPlane),
        bits(camera.farPlane), bits(static_cast<f32>(instanceIndex)),
        bits(static_cast<f32>(width)), bits(static_cast<f32>(height)),
        bits(lighting.toSun[0]), bits(lighting.toSun[1]),
        bits(lighting.toSun[2]), bits(photonGrid),
        bits(lighting.sunIrradiance[0] * visibility),
        bits(lighting.sunIrradiance[1] * visibility),
        bits(lighting.sunIrradiance[2] * visibility),
        bits(static_cast<f32>(instanceCount)),
        bits(lighting.environment[0]), bits(lighting.environment[1]),
        bits(lighting.environment[2]), 0U};
}

void SetFullTarget(
    rhi::CommandList& commands,
    const u32 width,
    const u32 height)
{
    commands.SetViewport({
        .x = 0.0F,
        .y = 0.0F,
        .width = static_cast<f32>(width),
        .height = static_cast<f32>(height),
        .minDepth = 0.0F,
        .maxDepth = 1.0F});
    commands.SetScissor({
        .left = 0,
        .top = 0,
        .right = static_cast<i32>(width),
        .bottom = static_cast<i32>(height)});
}
} // namespace

GlassCamera MakeGlassCamera(
    const lighting::LightingView& view,
    const u32 width,
    const u32 height) noexcept
{
    // The same basis PackProxySurfaceConstants and the lighting passes use.
    const auto forward = math::Normalize(view.forward);
    const auto right = math::Normalize(math::Cross(forward, math::Normalize(view.up)));
    const auto up = math::Cross(right, forward);

    const f32 aspect = height > 0U
        ? static_cast<f32>(width) / static_cast<f32>(height)
        : 1.0F;
    const f32 tanHalf =
        std::tan(std::max(view.verticalFovRadians, 1.0e-4F) * 0.5F);

    GlassCamera camera;
    camera.right = {right.x, right.y, right.z};
    camera.up = {up.x, up.y, up.z};
    camera.forward = {forward.x, forward.y, forward.z};
    camera.tanHalfY = tanHalf;
    camera.tanHalfX = tanHalf * std::max(aspect, 1.0e-4F);
    camera.nearPlane = std::max(view.nearPlaneMeters, 1.0e-5F);
    camera.farPlane =
        std::max(view.farPlaneMeters, camera.nearPlane + 1.0e-4F);
    return camera;
}

std::optional<GlassScreenRect> GlassScreenBounds(
    const GlassInstance& instance,
    const GlassCamera& camera,
    const u32 width,
    const u32 height) noexcept
{
    const GlassScreenRect full{
        0, 0, static_cast<i32>(width), static_cast<i32>(height)};
    const auto dot = [](const std::array<f32, 3>& a, const std::array<f32, 3>& b)
    {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };

    f32 minX = 1.0e30F;
    f32 minY = 1.0e30F;
    f32 maxX = -1.0e30F;
    f32 maxY = -1.0e30F;
    bool anyInFront = false;
    for (u32 corner = 0U; corner < 8U; ++corner)
    {
        // A bounding sphere-safe box: capsule/sphere/cylinder fit their
        // half extents, a plane has a thin y.
        const std::array<f32, 3> local{
            ((corner & 1U) != 0U ? 1.0F : -1.0F) * instance.halfExtents[0],
            ((corner & 2U) != 0U ? 1.0F : -1.0F) * instance.halfExtents[1],
            ((corner & 4U) != 0U ? 1.0F : -1.0F) * instance.halfExtents[2]};
        const std::array<f32, 3> world{
            instance.rows[0] * local[0] + instance.rows[1] * local[1] +
                instance.rows[2] * local[2] + instance.rows[3],
            instance.rows[4] * local[0] + instance.rows[5] * local[1] +
                instance.rows[6] * local[2] + instance.rows[7],
            instance.rows[8] * local[0] + instance.rows[9] * local[1] +
                instance.rows[10] * local[2] + instance.rows[11]};

        const f32 z = dot(world, camera.forward);
        if (z <= camera.nearPlane)
        {
            // Straddles the near plane: the footprint is unbounded.
            return full;
        }
        anyInFront = true;
        const f32 ndcX = dot(world, camera.right) / (z * camera.tanHalfX);
        const f32 ndcY = dot(world, camera.up) / (z * camera.tanHalfY);
        const f32 px = (ndcX * 0.5F + 0.5F) * static_cast<f32>(width);
        const f32 py = (0.5F - ndcY * 0.5F) * static_cast<f32>(height);
        minX = std::min(minX, px);
        maxX = std::max(maxX, px);
        minY = std::min(minY, py);
        maxY = std::max(maxY, py);
    }
    if (!anyInFront)
    {
        return std::nullopt;
    }

    GlassScreenRect rect{
        static_cast<i32>(std::floor(minX)) - 2,
        static_cast<i32>(std::floor(minY)) - 2,
        static_cast<i32>(std::ceil(maxX)) + 2,
        static_cast<i32>(std::ceil(maxY)) + 2};
    rect.left = std::clamp(rect.left, 0, full.right);
    rect.right = std::clamp(rect.right, 0, full.right);
    rect.top = std::clamp(rect.top, 0, full.bottom);
    rect.bottom = std::clamp(rect.bottom, 0, full.bottom);
    if (rect.right <= rect.left || rect.bottom <= rect.top)
    {
        return std::nullopt;
    }
    return rect;
}

std::array<f32, kGlassRecordFloats> PackGlassRecord(
    const GlassInstance& instance) noexcept
{
    std::array<f32, kGlassRecordFloats> record{};
    std::copy(instance.rows.begin(), instance.rows.end(), record.begin());
    record[12] = instance.tint[0];
    record[13] = instance.tint[1];
    record[14] = instance.tint[2];
    record[15] = instance.indexOfRefraction;
    record[16] = instance.halfExtents[0];
    record[17] = instance.halfExtents[1];
    record[18] = instance.halfExtents[2];
    record[19] = static_cast<f32>(static_cast<u32>(instance.shape));
    return record;
}

u32 GlassPhotonGrid(const f32 boundingRadiusMeters) noexcept
{
    if (!(boundingRadiusMeters > 0.0F))
    {
        return kMinPhotonGrid;
    }
    // About one photon per centimetre across a body up to a couple of metres.
    const f32 wanted = boundingRadiusMeters * 2.0F * 100.0F;
    return static_cast<u32>(std::clamp(
        wanted,
        static_cast<f32>(kMinPhotonGrid),
        static_cast<f32>(kMaxPhotonGrid)));
}

GlassSurfaceRenderer::GlassSurfaceRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler)
    : device_(device)
{
    const auto compile =
        [&compiler](const std::string& source, const shader::Stage stage)
    {
        return compiler.Compile({
            .source = source,
            .entryPoint = "main",
            .stage = stage,
            .debug = false});
    };
    const auto view = [](const auto& result)
    {
        return rhi::ShaderBytecodeView{
            .data = result.bytecode.data(), .size = result.bytecode.size()};
    };

    {
        const auto vs = compile(kFullscreenVertex, shader::Stage::Vertex);
        const auto ps = compile(kCopyPixel, shader::Stage::Pixel);
        copyPipeline_ = device.CreateGraphicsPipeline({
            .vertexShader = view(vs),
            .pixelShader = view(ps),
            .shaderResourceBuffers = 0U,
            .sampledTextures = 1U,
            .topology = rhi::PrimitiveTopology::TriangleList,
            .fillMode = rhi::FillMode::Solid,
            .cullMode = rhi::CullMode::None,
            .blendMode = rhi::BlendMode::Opaque,
            .depthTest = false,
            .depthWrite = false,
            .colorAttachmentFormats = {rhi::TextureFormat::RGBA16_Float},
            .colorAttachmentCount = 1U});
    }
    {
        const auto vs = compile(
            Compose({kCommon, kTrace, kScene, kCausticVertex}),
            shader::Stage::Vertex);
        const auto ps = compile(
            Compose({kCommon, kScene, kCausticPixel}),
            shader::Stage::Pixel);
        causticPipeline_ = device.CreateGraphicsPipeline({
            .vertexShader = view(vs),
            .pixelShader = view(ps),
            .pushConstantDwords = kGlassPushDwords,
            .shaderResourceBuffers = 1U,
            .sampledTextures = 5U,
            .topology = rhi::PrimitiveTopology::TriangleList,
            .fillMode = rhi::FillMode::Solid,
            .cullMode = rhi::CullMode::None,
            .blendMode = rhi::BlendMode::Additive,
            .depthTest = false,
            .depthWrite = false,
            .colorAttachmentFormats = {rhi::TextureFormat::RGBA16_Float},
            .colorAttachmentCount = 1U});
    }
    {
        const auto vs = compile(kFullscreenVertex, shader::Stage::Vertex);
        const auto ps = compile(
            Compose(
                {kCommon, kTrace, kScene, kGlassPixel},
                "#define DEPTH_BINDING 5\n#define BACKDROP_BINDING 6\n"),
            shader::Stage::Pixel);
        glassPipeline_ = device.CreateGraphicsPipeline({
            .vertexShader = view(vs),
            .pixelShader = view(ps),
            .pushConstantDwords = kGlassPushDwords,
            .shaderResourceBuffers = 5U,
            .sampledTextures = 2U,
            .topology = rhi::PrimitiveTopology::TriangleList,
            .fillMode = rhi::FillMode::Solid,
            .cullMode = rhi::CullMode::None,
            .blendMode = rhi::BlendMode::Opaque,
            .depthTest = false,
            .depthWrite = false,
            .colorAttachmentFormats = {rhi::TextureFormat::RGBA16_Float},
            .colorAttachmentCount = 1U});
    }
}

rhi::Buffer& GlassSurfaceRenderer::UploadRecords(
    const std::span<const GlassInstance> instances,
    const std::span<const f32> tail)
{
    std::vector<f32> floats;
    floats.reserve(instances.size() * kGlassRecordFloats);
    for (const GlassInstance& instance : instances)
    {
        const auto record = PackGlassRecord(instance);
        floats.insert(floats.end(), record.begin(), record.end());
    }
    floats.insert(floats.end(), tail.begin(), tail.end());
    for (std::size_t i = tail.size(); i < kGlassTailFloats; ++i)
    {
        floats.push_back(0.0F);
    }

    auto buffer = device_.CreateBuffer({
        .sizeBytes = std::max<std::size_t>(floats.size() * sizeof(f32), 16U),
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::ShaderResource});
    std::memcpy(buffer->Map(), floats.data(), floats.size() * sizeof(f32));
    buffer->Unmap();

    records_.push_back({std::move(buffer), tick_ + kRecordRetireTicks});
    return *records_.back().buffer;
}

void GlassSurfaceRenderer::CopyBackdrop(
    rhi::CommandList& commands,
    rhi::Texture& sceneColor,
    rhi::Texture& backdrop,
    const u32 width,
    const u32 height)
{
    if (copyPipeline_ == nullptr || width == 0U || height == 0U)
    {
        return;
    }
    commands.SetRenderTarget(backdrop);
    SetFullTarget(commands, width, height);
    commands.SetGraphicsPipeline(*copyPipeline_);
    commands.SetGraphicsTexture(0U, sceneColor);
    commands.Draw(3U);
}

void GlassSurfaceRenderer::DrawCaustics(
    rhi::CommandList& commands,
    const std::span<const GlassInstance> instances,
    rhi::Texture& sceneColor,
    rhi::Texture& depth,
    rhi::Texture& surfaceBaseRoughness,
    rhi::Texture& surfaceNormalMetallic,
    rhi::Texture& surfaceEmissionClass,
    const u32 width,
    const u32 height,
    const lighting::LightingView& view,
    const GlassLighting& lighting,
    rhi::Texture* const sunShadowMap,
    const MeshShadowFrame* const shadowFrame)
{
    ++tick_;
    std::erase_if(
        records_,
        [this](const RetiredRecords& records)
        {
            return tick_ >= records.retireAtTick;
        });

    const f32 sunPower = lighting.sunIrradiance[0] + lighting.sunIrradiance[1] +
                         lighting.sunIrradiance[2];
    if (causticPipeline_ == nullptr || width == 0U || height == 0U ||
        !(sunPower * lighting.sunVisibility > 0.0F))
    {
        return;
    }

    std::vector<GlassInstance> casters;
    for (const GlassInstance& instance : instances)
    {
        if (instance.caustics)
        {
            casters.push_back(instance);
        }
    }
    if (casters.empty())
    {
        return;
    }

    const bool haveShadow = sunShadowMap != nullptr && shadowFrame != nullptr;
    std::array<f32, kGlassTailFloats> shadowTail{};
    if (haveShadow)
    {
        shadowTail = {
            shadowFrame->center[0], shadowFrame->center[1],
            shadowFrame->center[2], shadowFrame->radius,
            shadowFrame->right[0], shadowFrame->right[1],
            shadowFrame->right[2], static_cast<f32>(shadowFrame->mapSize),
            shadowFrame->up[0], shadowFrame->up[1], shadowFrame->up[2],
            1.0F};
    }
    rhi::Buffer& records = UploadRecords(casters, shadowTail);
    const GlassCamera camera = MakeGlassCamera(view, width, height);

    commands.SetRenderTarget(sceneColor);
    SetFullTarget(commands, width, height);
    commands.SetGraphicsPipeline(*causticPipeline_);
    commands.SetGraphicsBuffer(0U, records);
    commands.SetGraphicsTexture(0U, depth);
    commands.SetGraphicsTexture(1U, surfaceBaseRoughness);
    commands.SetGraphicsTexture(2U, surfaceNormalMetallic);
    commands.SetGraphicsTexture(3U, surfaceEmissionClass);
    // Never read when no shadow frame was uploaded (enabled flag 0).
    commands.SetGraphicsTexture(4U, haveShadow ? *sunShadowMap : depth);

    for (u32 index = 0U; index < casters.size(); ++index)
    {
        const GlassInstance& instance = casters[index];
        const f32 radius = std::sqrt(
            instance.halfExtents[0] * instance.halfExtents[0] +
            instance.halfExtents[1] * instance.halfExtents[1] +
            instance.halfExtents[2] * instance.halfExtents[2]);
        const u32 grid = GlassPhotonGrid(radius);
        const auto constants = PackPush(
            camera, index, width, height, static_cast<f32>(grid), lighting,
            static_cast<u32>(casters.size()));
        commands.SetGraphicsConstants(constants);
        commands.Draw(grid * grid * 6U);
    }
}

void GlassSurfaceRenderer::DrawGlass(
    rhi::CommandList& commands,
    const std::span<const GlassInstance> instances,
    rhi::Texture& sceneColor,
    rhi::Texture& backdrop,
    rhi::Texture& depth,
    const u32 width,
    const u32 height,
    const lighting::LightingView& view,
    const GlassLighting& lighting,
    const lighting::SdfGatherInput* const sdf)
{
    if (glassPipeline_ == nullptr || width == 0U || height == 0U ||
        instances.empty())
    {
        return;
    }

    const bool sdfAvailable =
        sdf != nullptr && sdf->distance != nullptr && sdf->albedo != nullptr &&
        sdf->normal != nullptr && sdf->radiance != nullptr;
    const math::Double3 sdfOrigin = sdfAvailable
        ? sdf->originInFrameMeters - view.cameraPositionInFrameMeters
        : math::Double3{};
    const std::array<f32, kGlassTailFloats> sdfTail{
        static_cast<f32>(sdfOrigin.x), static_cast<f32>(sdfOrigin.y),
        static_cast<f32>(sdfOrigin.z), sdfAvailable ? sdf->voxelSize : 0.25F,
        sdfAvailable ? static_cast<f32>(sdf->dimensions[0]) : 1.0F,
        sdfAvailable ? static_cast<f32>(sdf->dimensions[1]) : 1.0F,
        sdfAvailable ? static_cast<f32>(sdf->dimensions[2]) : 1.0F,
        sdfAvailable ? 1.0F : 0.0F,
        lighting.localUp[0], lighting.localUp[1], lighting.localUp[2], 0.0F};

    if (dummySdf_ == nullptr)
    {
        dummySdf_ = device_.CreateBuffer({
            .sizeBytes = 64U,
            .usage = rhi::BufferUsage::Structured,
            .memory = rhi::MemoryUsage::HostVisible,
            .initialState = rhi::ResourceState::UnorderedAccess});
        std::memset(
            dummySdf_->Map(), 0, static_cast<std::size_t>(dummySdf_->SizeBytes()));
        dummySdf_->Unmap();
    }

    rhi::Buffer& records = UploadRecords(instances, sdfTail);
    const GlassCamera camera = MakeGlassCamera(view, width, height);

    commands.SetRenderTarget(sceneColor);
    commands.SetViewport({
        .x = 0.0F,
        .y = 0.0F,
        .width = static_cast<f32>(width),
        .height = static_cast<f32>(height),
        .minDepth = 0.0F,
        .maxDepth = 1.0F});
    commands.SetGraphicsPipeline(*glassPipeline_);
    commands.SetGraphicsBuffer(0U, records);
    commands.SetGraphicsBuffer(1U, sdfAvailable ? *sdf->distance : *dummySdf_);
    commands.SetGraphicsBuffer(2U, sdfAvailable ? *sdf->albedo : *dummySdf_);
    commands.SetGraphicsBuffer(3U, sdfAvailable ? *sdf->normal : *dummySdf_);
    commands.SetGraphicsBuffer(4U, sdfAvailable ? *sdf->radiance : *dummySdf_);
    commands.SetGraphicsTexture(0U, depth);
    commands.SetGraphicsTexture(1U, backdrop);

    for (u32 index = 0U; index < instances.size(); ++index)
    {
        const auto rect =
            GlassScreenBounds(instances[index], camera, width, height);
        if (!rect.has_value())
        {
            continue;
        }
        commands.SetScissor({
            .left = rect->left,
            .top = rect->top,
            .right = rect->right,
            .bottom = rect->bottom});
        commands.SetGraphicsConstants(
            PackPush(
                camera, index, width, height, 0.0F, lighting,
                static_cast<u32>(instances.size())));
        commands.Draw(3U);
    }
}
} // namespace orbit::mesh_render
