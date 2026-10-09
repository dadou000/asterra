#include <orbit/mesh_render/MeshSdfScene.hpp>

#include <orbit/lighting/SdfTraceShader.hpp>

#include <algorithm>
#include <cstdint>
#include <bit>
#include <cmath>
#include <cstring>

namespace orbit::mesh_render
{
namespace
{
constexpr u64 kRetireTicks = 24U;
constexpr f32 kMinimumVoxelMeters = 0.25F;
constexpr u32 kMaximumDimension = 256U;
constexpr f32 kVolumeMarginMeters = 2.0F;
constexpr f32 kSurroundMarginMeters = 16.0F;

constexpr const char* kOctahedral = R"(
float3 UnpackOct(uint packed)
{
    const int xi = (int)(packed << 16) >> 16;
    const int yi = (int)packed >> 16;
    float2 e = float2(float(xi), float(yi)) / 32767.0;
    float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0)
    {
        n.xy = (1.0 - abs(n.yx)) *
            float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    }
    return normalize(n);
}

uint PackOct(float3 n)
{
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1.0e-20);
    float2 e = n.xy;
    if (n.z < 0.0)
    {
        e = (1.0 - abs(e.yx)) *
            float2(e.x >= 0.0 ? 1.0 : -1.0, e.y >= 0.0 ? 1.0 : -1.0);
    }
    const int2 q = int2(round(clamp(e, -1.0, 1.0) * 32767.0));
    return (uint(q.x) & 0xFFFFu) | ((uint(q.y) & 0xFFFFu) << 16);
}
)";

// Clears the global volume to "far, nothing".
constexpr const char* kClearShader = R"(
[[vk::binding(0, 0)]] StructuredBuffer<float4> g_params : register(t0);
[[vk::binding(1, 0)]] RWStructuredBuffer<float> g_dist : register(u1);
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> g_albedo : register(u2);
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> g_normal : register(u3);
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> g_emissive : register(u4);
[[vk::binding(5, 0)]] RWStructuredBuffer<float4> g_radiance : register(u5);

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint count = (uint)g_params[0].x;
    if (id.x >= count)
    {
        return;
    }
    g_radiance[id.x] = 0.0;
    g_dist[id.x] = 1.0e4;
    g_albedo[id.x] = 0u;
    g_normal[id.x] = 0u;
    g_emissive[id.x] = 0u;
}
)";

// Repacks the merged distance field as one uint4 of eight f16 corners per
// cell (see SdfTraceShader.hpp), so the gather's trace step is one load.
constexpr const char* kCornerShader = R"(
[[vk::binding(0, 0)]] StructuredBuffer<float4> g_params : register(t0);
[[vk::binding(1, 0)]] RWStructuredBuffer<float> g_dist : register(u1);
[[vk::binding(2, 0)]] RWStructuredBuffer<uint4> g_corners : register(u2);

uint PackPair(float a, float b)
{
    return f32tof16(min(a, 60000.0)) | (f32tof16(min(b, 60000.0)) << 16);
}

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    const int3 dims = int3(g_params[0].xyz);
    const int3 i0 = int3(id);
    if (any(i0 >= dims))
    {
        return;
    }
    const int3 i1 = min(i0 + 1, dims - 1);
    const int row = dims.x;
    const int slab = dims.x * dims.y;
    const int b000 = i0.z * slab + i0.y * row;
    const int b010 = i0.z * slab + i1.y * row;
    const int b001 = i1.z * slab + i0.y * row;
    const int b011 = i1.z * slab + i1.y * row;
    g_corners[i0.z * slab + i0.y * row + i0.x] = uint4(
        PackPair(g_dist[b000 + i0.x], g_dist[b000 + i1.x]),
        PackPair(g_dist[b010 + i0.x], g_dist[b010 + i1.x]),
        PackPair(g_dist[b001 + i0.x], g_dist[b001 + i1.x]),
        PackPair(g_dist[b011 + i0.x], g_dist[b011 + i1.x]));
}
)";

// Merges one mesh instance's distance field into the global volume: each
// global voxel is mapped into the mesh's local space, the mesh field is
// sampled trilinearly (plus the distance to the mesh's grid box when outside
// it) and scaled to world units; the closest instance keeps its surface
// attributes.
constexpr const char* kMergeShader = R"(
[[vk::binding(0, 0)]] StructuredBuffer<float4> g_params : register(t0);
[[vk::binding(1, 0)]] RWStructuredBuffer<float> g_dist : register(u1);
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> g_albedo : register(u2);
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> g_normal : register(u3);
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> g_emissive : register(u4);
[[vk::binding(5, 0)]] RWStructuredBuffer<float> m_dist : register(u5);
[[vk::binding(6, 0)]] RWStructuredBuffer<uint> m_albedo : register(u6);
[[vk::binding(7, 0)]] RWStructuredBuffer<uint> m_normal : register(u7);
[[vk::binding(8, 0)]] RWStructuredBuffer<uint> m_emissive : register(u8);
)" R"(
// params: 0..2 volume->mesh rows, 3..5 mesh->volume linear rows,
// 6 = (scale, global voxel, emissive scale, 0), 7 = (mesh origin, mesh voxel),
// 8 = (mesh dims, 0), 9 = (global dims, 0)
uint MeshIndex(int3 c, int3 dims)
{
    return (uint(c.z) * uint(dims.y) + uint(c.y)) * uint(dims.x) + uint(c.x);
}

float MeshDistance(float3 gc, int3 dims)
{
    const float3 clamped = clamp(gc, 0.0, float3(dims - 1));
    const int3 i0 = int3(floor(clamped));
    const int3 i1 = min(i0 + 1, dims - 1);
    const float3 f = clamped - float3(i0);

    const float c000 = m_dist[MeshIndex(int3(i0.x, i0.y, i0.z), dims)];
    const float c100 = m_dist[MeshIndex(int3(i1.x, i0.y, i0.z), dims)];
    const float c010 = m_dist[MeshIndex(int3(i0.x, i1.y, i0.z), dims)];
    const float c110 = m_dist[MeshIndex(int3(i1.x, i1.y, i0.z), dims)];
    const float c001 = m_dist[MeshIndex(int3(i0.x, i0.y, i1.z), dims)];
    const float c101 = m_dist[MeshIndex(int3(i1.x, i0.y, i1.z), dims)];
    const float c011 = m_dist[MeshIndex(int3(i0.x, i1.y, i1.z), dims)];
    const float c111 = m_dist[MeshIndex(int3(i1.x, i1.y, i1.z), dims)];

    return lerp(
        lerp(lerp(c000, c100, f.x), lerp(c010, c110, f.x), f.y),
        lerp(lerp(c001, c101, f.x), lerp(c011, c111, f.x), f.y),
        f.z);
}

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    const int3 gdims = int3(g_params[9].xyz);
    if (any(int3(id) >= gdims))
    {
        return;
    }

    const float gvox = g_params[6].y;
    const float3 p = float3(id) * gvox;

    const float4 r0 = g_params[0];
    const float4 r1 = g_params[1];
    const float4 r2 = g_params[2];
    const float3 q = float3(
        dot(r0.xyz, p) + r0.w,
        dot(r1.xyz, p) + r1.w,
        dot(r2.xyz, p) + r2.w);

    const float mvox = g_params[7].w;
    const int3 mdims = int3(g_params[8].xyz);
    const float3 gc = (q - g_params[7].xyz) / mvox;
    const float3 clamped = clamp(gc, 0.0, float3(mdims - 1));
    const float outside = length((gc - clamped) * mvox);

    const float meshDistance = MeshDistance(clamped, mdims) + outside;
    const float world = meshDistance * g_params[6].x;

    const uint at = (id.z * uint(gdims.y) + id.y) * uint(gdims.x) + id.x;
    if (world >= g_dist[at])
    {
        return;
    }
    g_dist[at] = world;

    // Surface attributes: the nearest surface voxel around the sample point.
    if (world <= 0.87 * gvox + 0.5 * g_params[6].x * mvox)
    {
        const int3 base = int3(floor(clamped));
        float best = 1.0e9;
        int3 chosen = int3(-1, -1, -1);
        [unroll]
        for (int k = 0; k < 8; ++k)
        {
            const int3 c = min(base + int3(k & 1, (k >> 1) & 1, (k >> 2) & 1), mdims - 1);
            const uint index = MeshIndex(c, mdims);
            if ((m_albedo[index] >> 24) != 0u)
            {
                const float d = m_dist[index] + length((float3(c) - clamped) * mvox);
                if (d < best)
                {
                    best = d;
                    chosen = c;
                }
            }
        }
        if (chosen.x >= 0)
        {
            const uint index = MeshIndex(chosen, mdims);
            g_albedo[at] = m_albedo[index];
            g_emissive[at] = m_emissive[index];
            const float3 nm = UnpackOct(m_normal[index]);
            const float3 nw = float3(
                dot(g_params[3].xyz, nm),
                dot(g_params[4].xyz, nm),
                dot(g_params[5].xyz, nm));
            g_normal[at] = PackOct(normalize(nw));
        }
        else
        {
            g_albedo[at] = 0u;
            g_normal[at] = 0u;
            g_emissive[at] = 0u;
        }
    }
    else
    {
        g_albedo[at] = 0u;
        g_normal[at] = 0u;
        g_emissive[at] = 0u;
    }
}
)";


// Proxy primitives (spheres / oriented boxes) stamped into the field.
// params: 0 = (global dims, primitive count), 1 = (global voxel, 0, 0, 0),
// 2 = albedo rgb. data: 4 float4 per primitive:
//   (centre rel. volume origin, type 0 sphere / 1 box), (axisX, ext.x),
//   (axisY, ext.y), (axisZ, ext.z).
constexpr const char* kPrimitiveShader = R"(
[[vk::binding(0, 0)]] StructuredBuffer<float4> g_params : register(t0);
[[vk::binding(1, 0)]] RWStructuredBuffer<float> g_dist : register(u1);
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> g_albedo : register(u2);
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> g_normal : register(u3);
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> g_emissive : register(u4);
[[vk::binding(5, 0)]] StructuredBuffer<float4> g_data : register(t5);

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    const int3 gdims = int3(g_params[0].xyz);
    if (any(int3(id) >= gdims))
    {
        return;
    }
    const uint count = (uint)g_params[0].w;
    const float gvox = g_params[1].x;
    const float3 p = float3(id) * gvox;

    float best = 1.0e9;
    float3 bestNormal = float3(0.0, 1.0, 0.0);
    for (uint i = 0u; i < count; ++i)
    {
        const float4 a = g_data[i * 4u + 0u];
        const float4 bx = g_data[i * 4u + 1u];
        const float4 by = g_data[i * 4u + 2u];
        const float4 bz = g_data[i * 4u + 3u];
        const float3 rel = p - a.xyz;
        float d;
        float3 n;
        if (a.w < 0.5)
        {
            const float len = max(length(rel), 1.0e-5);
            d = abs(len - bx.w);
            n = (len >= bx.w ? 1.0 : -1.0) * rel / len;
        }
        else
        {
            const float3 q = float3(dot(rel, bx.xyz), dot(rel, by.xyz), dot(rel, bz.xyz));
            const float3 ext = float3(bx.w, by.w, bz.w);
            const float3 o = abs(q) - ext;
            const float3 sgn = float3(q.x >= 0.0 ? 1.0 : -1.0, q.y >= 0.0 ? 1.0 : -1.0, q.z >= 0.0 ? 1.0 : -1.0);
            float3 local;
            if (any(o > 0.0))
            {
                const float3 outside = max(o, 0.0);
                d = length(outside);
                local = sgn * outside / max(d, 1.0e-5);
            }
            else
            {
                d = -max(o.x, max(o.y, o.z));
                local = float3(0.0, 0.0, 0.0);
                if (o.x >= o.y && o.x >= o.z) local.x = sgn.x;
                else if (o.y >= o.z) local.y = sgn.y;
                else local.z = sgn.z;
            }
            n = local.x * bx.xyz + local.y * by.xyz + local.z * bz.xyz;
        }
        if (d < best)
        {
            best = d;
            bestNormal = n;
        }
    }

    const uint at = (id.z * uint(gdims.y) + id.y) * uint(gdims.x) + id.x;
    if (best >= g_dist[at])
    {
        return;
    }
    const bool wasSurface = (g_albedo[at] >> 24) != 0u;
    g_dist[at] = best;
    if (wasSurface)
    {
    }
    else if (best <= 0.87 * gvox)
    {
        const float3 c = saturate(g_params[2].xyz);
        g_albedo[at] = uint(c.x * 255.0 + 0.5) | (uint(c.y * 255.0 + 0.5) << 8) |
            (uint(c.z * 255.0 + 0.5) << 16) | (255u << 24);
        g_normal[at] = PackOct(normalize(bestNormal));
        g_emissive[at] = 0u;
    }
    else
    {
        g_albedo[at] = 0u;
        g_normal[at] = 0u;
        g_emissive[at] = 0u;
    }
}
)";

// Terrain heightfield patch stamped into the field.
// params: 0 = (global dims, 0), 1 = (global voxel, cell, count, 0),
// 2 = (patch centre rel. volume origin, 0), 3 = east, 4 = north, 5 = up,
// 6 = albedo rgb. data: heights (count * count).
constexpr const char* kTerrainShader = R"(
[[vk::binding(0, 0)]] StructuredBuffer<float4> g_params : register(t0);
[[vk::binding(1, 0)]] RWStructuredBuffer<float> g_dist : register(u1);
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> g_albedo : register(u2);
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> g_normal : register(u3);
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> g_emissive : register(u4);
[[vk::binding(5, 0)]] StructuredBuffer<float> g_heights : register(t5);

float HeightAt(float2 g, int count)
{
    const float2 c = clamp(g, 0.0, float(count - 1));
    const int2 i0 = int2(floor(c));
    const int2 i1 = min(i0 + 1, count - 1);
    const float2 f = c - float2(i0);
    const float h00 = g_heights[i0.y * count + i0.x];
    const float h10 = g_heights[i0.y * count + i1.x];
    const float h01 = g_heights[i1.y * count + i0.x];
    const float h11 = g_heights[i1.y * count + i1.x];
    return lerp(lerp(h00, h10, f.x), lerp(h01, h11, f.x), f.y);
}

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    const int3 gdims = int3(g_params[0].xyz);
    if (any(int3(id) >= gdims))
    {
        return;
    }
    const float gvox = g_params[1].x;
    const float cell = g_params[1].y;
    const int count = (int)g_params[1].z;
    const float3 east = g_params[3].xyz;
    const float3 north = g_params[4].xyz;
    const float3 up = g_params[5].xyz;

    const float3 rel = float3(id) * gvox - g_params[2].xyz;
    const float u = dot(rel, east);
    const float v = dot(rel, north);
    const float hp = dot(rel, up);
    const float half = 0.5 * float(count - 1);
    const float2 g = float2(u, v) / cell + half;
    // Outside the sampled footprint the patch says nothing.
    if (any(g < 0.0) || any(g > float(count - 1)))
    {
        return;
    }

    const float h = HeightAt(g, count);
    const float gx = (HeightAt(g + float2(1.0, 0.0), count) - HeightAt(g - float2(1.0, 0.0), count)) / (2.0 * cell);
    const float gy = (HeightAt(g + float2(0.0, 1.0), count) - HeightAt(g - float2(0.0, 1.0), count)) / (2.0 * cell);
    const float d = abs(hp - h) * rsqrt(1.0 + gx * gx + gy * gy);

    const uint at = (id.z * uint(gdims.y) + id.y) * uint(gdims.x) + id.x;
    if (d >= g_dist[at])
    {
        return;
    }
    const bool wasSurface = (g_albedo[at] >> 24) != 0u;
    g_dist[at] = d;
    if (wasSurface)
    {
        // A mesh surface voxel keeps its own material: meshes win over ground.
    }
    else if (d <= 0.87 * gvox && hp >= h - 0.5 * gvox)
    {
        const float3 c = saturate(g_params[6].xyz);
        g_albedo[at] = uint(c.x * 255.0 + 0.5) | (uint(c.y * 255.0 + 0.5) << 8) |
            (uint(c.z * 255.0 + 0.5) << 16) | (255u << 24);
        g_normal[at] = PackOct(normalize(up - gx * east - gy * north));
        g_emissive[at] = 0u;
    }
    else
    {
        g_albedo[at] = 0u;
        g_normal[at] = 0u;
        g_emissive[at] = 0u;
    }
}
)";


// Surface voxel lighting. Every frame a quarter of the surface voxels
// recompute their outgoing diffuse radiance:
//   emissive + albedo / pi * (sun * soft shadow + irradiance gathered by
//   cosine-weighted rays through the field, each hit read from the previous
//   radiance, each escape counted as sky).
// Temporal blending converges the multi-bounce term over a few frames.
const std::string kLightShader = std::string(R"(
[[vk::binding(0, 0)]] StructuredBuffer<float4> g_params : register(t0);
[[vk::binding(1, 0)]] RWStructuredBuffer<float> g_sdfDist : register(u1);
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> g_sdfAlbedo : register(u2);
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> g_sdfNormal : register(u3);
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> g_emissive : register(u4);
[[vk::binding(5, 0)]] RWStructuredBuffer<float4> g_sdfRadiance : register(u5);

#define SDF_ORIGIN float3(0.0, 0.0, 0.0)
#define SDF_VOXEL g_params[0].w
#define SDF_DIMS int3(g_params[0].xyz)
)") + lighting::kSdfTraceHlsl + R"(
uint Hash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float3 CosineDirection(float3 normal, float u, float v)
{
    const float3 helper = abs(normal.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    const float3 tangent = normalize(cross(helper, normal));
    const float3 bitangent = cross(normal, tangent);
    const float radius = sqrt(u);
    const float phi = 6.28318530718 * v;
    return normalize(
        tangent * (radius * cos(phi)) +
        bitangent * (radius * sin(phi)) +
        normal * sqrt(max(1.0 - u, 0.0)));
}

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    const int3 dims = SDF_DIMS;
    if (any(int3(id) >= dims))
    {
        return;
    }

    const uint index = SdfIndex(int3(id));
    const uint albedoPacked = g_sdfAlbedo[index];
    if ((albedoPacked >> 24) == 0u)
    {
        return;
    }

    const uint frame = uint(g_params[2].w);
    if (((Hash(index) + frame) & 3u) != 0u)
    {
        return;
    }

    const float voxel = SDF_VOXEL;
    const float3 position = float3(id) * voxel;
    const float3 normal = SdfUnpackOct(g_sdfNormal[index]);
    const float3 origin = position + normal * (1.1 * voxel);

    const float3 albedo = float3(
        float(albedoPacked & 0xFFu),
        float((albedoPacked >> 8) & 0xFFu),
        float((albedoPacked >> 16) & 0xFFu)) / 255.0;

    // Sun, softened by the field.
    const float3 toSun = normalize(g_params[1].xyz);
    float sun = 0.0;
    const float facing = saturate(dot(normal, toSun));
    if (facing > 0.0 && g_params[1].w > 0.0)
    {
        sun = g_params[1].w * facing * SdfSoftShadow(origin, toSun, 60.0, 12.0);
    }

    // Gathered irradiance: rays that hit read the previous radiance, rays
    // that escape upward see the sky.
    const float3 up = normalize(g_params[3].xyz);
    const float3 skyRadiance = g_params[2].xyz / 3.14159265;
    const uint seed = Hash(index * 9781u + frame * 6271u);
    const float2 rotation = float2(float(seed & 0xFFFFu), float(seed >> 16)) / 65536.0;

    float3 gathered = 0.0;
    const uint rays = 8u;
    for (uint r = 0u; r < rays; ++r)
    {
        const float u = frac(rotation.x + float(r) * 0.7548776662);
        const float v = frac(rotation.y + float(r) * 0.5698402910);
        const float3 direction = CosineDirection(normal, u, v);

        float3 hit;
        float hitT;
        if (SdfTrace(origin, direction, 24.0, hit, hitT))
        {
            gathered += SdfFetchRadiance(hit, -direction);
        }
        else if (dot(direction, up) > 0.0)
        {
            gathered += skyRadiance;
        }
    }
    const float3 indirect = 3.14159265 * gathered / float(rays);

    const uint emissivePacked = g_emissive[index];
    const float3 emissive = float3(
        float(emissivePacked & 0xFFu),
        float((emissivePacked >> 8) & 0xFFu),
        float((emissivePacked >> 16) & 0xFFu)) / 255.0 * g_params[3].w;

    const float3 outgoing = emissive + albedo / 3.14159265 * (sun + indirect);
    const float4 previous = g_sdfRadiance[index];
    g_sdfRadiance[index] = float4(lerp(previous.rgb, outgoing, previous.a > 0.5 ? 0.3 : 1.0), 1.0);
}
)";

constexpr const char* kDebugShader = R"(
[[vk::binding(0, 0)]] RWStructuredBuffer<float> g_dist : register(u0);
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> g_albedo : register(u1);
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> g_normal : register(u2);
[[vk::binding(3, 0)]] RWStructuredBuffer<float4> g_radiance : register(u3);
[[vk::binding(4, 0)]] RWTexture2D<float4> g_destination : register(u4);
[[vk::binding(5, 0)]] [[vk::combinedImageSampler]] Texture2D g_color;
[[vk::binding(5, 0)]] [[vk::combinedImageSampler]] SamplerState g_colorSampler;

struct Constants
{
    uint width;
    uint height;
    uint mode;
    uint pad;
    float4 forwardAspect;
    float4 upTanHalfFov;
    float4 originVoxel;   // volume voxel (0,0,0) centre relative to the camera, voxel size
    float4 dimensions;    // volume dimensions
    float4 sunMaxDistance;
};
[[vk::push_constant]] Constants g;
)" R"(
uint VolumeIndex(int3 c)
{
    const int3 dims = int3(g.dimensions.xyz);
    return (uint(c.z) * uint(dims.y) + uint(c.y)) * uint(dims.x) + uint(c.x);
}

float VolumeDistance(float3 gc)
{
    const int3 dims = int3(g.dimensions.xyz);
    const float3 clamped = clamp(gc, 0.0, float3(dims - 1));
    const float outside = length((gc - clamped) * g.originVoxel.w);
    const int3 i0 = int3(floor(clamped));
    const int3 i1 = min(i0 + 1, dims - 1);
    const float3 f = clamped - float3(i0);

    const float c000 = g_dist[VolumeIndex(int3(i0.x, i0.y, i0.z))];
    const float c100 = g_dist[VolumeIndex(int3(i1.x, i0.y, i0.z))];
    const float c010 = g_dist[VolumeIndex(int3(i0.x, i1.y, i0.z))];
    const float c110 = g_dist[VolumeIndex(int3(i1.x, i1.y, i0.z))];
    const float c001 = g_dist[VolumeIndex(int3(i0.x, i0.y, i1.z))];
    const float c101 = g_dist[VolumeIndex(int3(i1.x, i0.y, i1.z))];
    const float c011 = g_dist[VolumeIndex(int3(i0.x, i1.y, i1.z))];
    const float c111 = g_dist[VolumeIndex(int3(i1.x, i1.y, i1.z))];

    return outside + lerp(
        lerp(lerp(c000, c100, f.x), lerp(c010, c110, f.x), f.y),
        lerp(lerp(c001, c101, f.x), lerp(c011, c111, f.x), f.y),
        f.z);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g.width || id.y >= g.height)
    {
        return;
    }

    const float2 uv = (float2(id.xy) + 0.5) / float2(g.width, g.height);
    float4 result = g_color.SampleLevel(g_colorSampler, uv, 0);

    if (g.mode == 4u && id.x >= g.width / 2u)
    {
        g_destination[id.xy] = result;
        return;
    }

    const float3 forward = normalize(g.forwardAspect.xyz);
    const float3 requestedUp = normalize(g.upTanHalfFov.xyz);
    const float3 right = normalize(cross(forward, requestedUp));
    const float3 up = cross(right, forward);
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    const float3 direction = normalize(
        forward +
        right * (ndc.x * g.forwardAspect.w * g.upTanHalfFov.w) +
        up * (ndc.y * g.upTanHalfFov.w));

    const float voxel = g.originVoxel.w;
    const float3 boxMin = g.originVoxel.xyz - 0.5 * voxel;
    const float3 boxMax = g.originVoxel.xyz + (g.dimensions.xyz - 0.5) * voxel;

    // Ray / box entry and exit (camera at the origin).
    const float3 inverse = 1.0 / (direction + 1.0e-12 * sign(direction + 1.0e-20));
    const float3 t0 = boxMin * inverse;
    const float3 t1 = boxMax * inverse;
    const float tNear = max(max(min(t0.x, t1.x), min(t0.y, t1.y)), max(min(t0.z, t1.z), 0.0));
    const float tFar = min(min(max(t0.x, t1.x), max(t0.y, t1.y)), max(t0.z, t1.z));

    if (tNear >= tFar)
    {
        g_destination[id.xy] = result;
        return;
    }

    float t = tNear;
    bool hit = false;
    uint steps = 0u;
    for (uint i = 0u; i < 200u; ++i)
    {
        const float3 position = direction * t;
        const float d = VolumeDistance((position - g.originVoxel.xyz) / voxel);
        steps = i;
        if (d < 0.35 * voxel)
        {
            hit = true;
            break;
        }
        t += max(d * 0.85, 0.15 * voxel);
        if (t > tFar)
        {
            break;
        }
    }

    if (!hit)
    {
        g_destination[id.xy] = g.mode == 2u ? float4(0.0, 0.0, 0.0, 1.0) : result;
        return;
    }

    if (g.mode == 2u)
    {
        const float heat = saturate(float(steps) / 120.0);
        g_destination[id.xy] = float4(heat, 1.0 - abs(heat - 0.5) * 2.0, 1.0 - heat, 1.0);
        return;
    }

    if (g.mode == 3u)
    {
        const float v = saturate(t / g.sunMaxDistance.w);
        g_destination[id.xy] = float4(v, v, v, 1.0);
        return;
    }

    // Nearest surface voxel with attributes around the hit.
    const float3 hitCell = (direction * t - g.originVoxel.xyz) / voxel;
    const int3 centre = int3(round(hitCell));
    const int3 dims = int3(g.dimensions.xyz);
    float best = 1.0e9;
    uint albedo = 0u;
    uint normalPacked = 0u;
    float3 radiance = 0.0;
    for (int z = -1; z <= 1; ++z)
    {
        for (int y = -1; y <= 1; ++y)
        {
            for (int x = -1; x <= 1; ++x)
            {
                const int3 c = clamp(centre + int3(x, y, z), 0, dims - 1);
                const uint index = VolumeIndex(c);
                const uint a = g_albedo[index];
                if ((a >> 24) != 0u)
                {
                    const float d = length(float3(c) - hitCell);
                    if (d < best)
                    {
                        best = d;
                        albedo = a;
                        normalPacked = g_normal[index];
                        radiance = g_radiance[index].rgb;
                    }
                }
            }
        }
    }

    float3 colour = float3(0.5, 0.5, 0.5);
    float3 normal = float3(0.0, 1.0, 0.0);
    if (albedo != 0u)
    {
        colour = float3(
            float(albedo & 0xFFu),
            float((albedo >> 8) & 0xFFu),
            float((albedo >> 16) & 0xFFu)) / 255.0;
        normal = UnpackOct(normalPacked);
    }
    if (g.mode == 5u)
    {
        // Surface radiance (sun + sky + bounce), shown as-is: scene units.
        g_destination[id.xy] = float4(radiance * 3.14159265, 1.0);
        return;
    }
    const float light = 0.25 + 0.75 * saturate(dot(normal, normalize(g.sunMaxDistance.xyz)));
    g_destination[id.xy] = float4(colour * light * 1.5, 1.0);
}
)";

[[nodiscard]] u64 Mix(u64 hash, const u64 value) noexcept
{
    hash ^= value + 0x9E3779B97F4A7C15ULL + (hash << 6U) + (hash >> 2U);
    return hash;
}

[[nodiscard]] u32 Bits(const f32 value) noexcept
{
    return std::bit_cast<u32>(value);
}
} // namespace

MeshSdfScene::MeshSdfScene(
    rhi::Device& device,
    const shader::Compiler& compiler)
    : device_(device)
{
    const auto make = [&](const std::string& source, const u32 buffers)
    {
        const auto compiled = compiler.Compile({
            .source = std::string(kOctahedral) + source,
            .entryPoint = "main",
            .stage = shader::Stage::Compute,
            .debug = false});
        return device.CreateComputePipeline({
            .computeShader = {
                .data = compiled.bytecode.data(),
                .size = compiled.bytecode.size()},
            .pushConstantDwords = 0U,
            .shaderResourceBuffers = buffers});
    };
    clearPipeline_ = make(kClearShader, 6U);
    mergePipeline_ = make(kMergeShader, 9U);
    lightPipeline_ = make(kLightShader, 6U);
    primitivePipeline_ = make(kPrimitiveShader, 6U);
    terrainPipeline_ = make(kTerrainShader, 6U);
    cornerPipeline_ = make(kCornerShader, 3U);
}

void MeshSdfScene::Update(
    rhi::CommandList& commands,
    const std::span<const MeshInstance> instances,
    const math::Double3& cameraInFrameMeters,
    const SdfExtraGeometry* extra)
{
    ++tick_;
    std::erase_if(
        retired_,
        [this](const Retired& r) { return tick_ >= r.retireAtTick; });

    struct Placed
    {
        const MeshModel* model;
        const MeshModel::SdfVolume* field;
        std::array<f64, 9> linear;  // column-major-free: row r, col c = [r*3+c]
        math::Double3 translation;  // frame coordinates
    };
    std::vector<Placed> placed;
    for (const MeshInstance& instance : instances)
    {
        if (instance.model == nullptr || instance.model->Sdf() == nullptr)
        {
            continue;
        }
        const auto& r = instance.rows;
        Placed p;
        p.model = instance.model;
        p.field = instance.model->Sdf();
        p.linear = {
            r[0], r[1], r[2],
            r[4], r[5], r[6],
            r[8], r[9], r[10]};
        p.translation = {
            cameraInFrameMeters.x + static_cast<f64>(r[3]),
            cameraInFrameMeters.y + static_cast<f64>(r[7]),
            cameraInFrameMeters.z + static_cast<f64>(r[11])};
        placed.push_back(p);
    }

    // Change detection (camera independent: translations are frame-fixed).
    u64 signature = 0x1234ULL;
    for (const Placed& p : placed)
    {
        signature = Mix(signature, reinterpret_cast<std::uintptr_t>(p.model));
        signature = Mix(signature, p.model->Generation());
        for (const f64 v : p.linear)
        {
            signature = Mix(signature, static_cast<u64>(std::llround(v * 1.0e4)));
        }
        signature = Mix(signature, static_cast<u64>(std::llround(p.translation.x * 50.0)));
        signature = Mix(signature, static_cast<u64>(std::llround(p.translation.y * 50.0)));
        signature = Mix(signature, static_cast<u64>(std::llround(p.translation.z * 50.0)));
    }
    const bool hasTerrain =
        extra != nullptr && extra->terrain != nullptr &&
        extra->terrain->count > 1U &&
        extra->terrain->heights.size() >=
            static_cast<std::size_t>(extra->terrain->count) * extra->terrain->count;
    const bool hasPrimitives = extra != nullptr && !extra->primitives.empty();
    if (hasTerrain)
    {
        signature = Mix(signature, extra->terrain->revision);
    }
    if (hasPrimitives)
    {
        for (const SdfProxyPrimitive& primitive : extra->primitives)
        {
            signature = Mix(signature, static_cast<u64>(std::llround(primitive.center.x * 50.0)));
            signature = Mix(signature, static_cast<u64>(std::llround(primitive.center.y * 50.0)));
            signature = Mix(signature, static_cast<u64>(std::llround(primitive.center.z * 50.0)));
            signature = Mix(signature, static_cast<u64>(std::llround(primitive.halfExtents.x * 50.0F)));
            signature = Mix(signature, static_cast<u64>(std::llround(primitive.halfExtents.y * 50.0F)));
            signature = Mix(signature, static_cast<u64>(std::llround(primitive.halfExtents.z * 50.0F)));
            signature = Mix(signature, static_cast<u64>(std::llround(primitive.axisX.x * 1000.0F)));
            signature = Mix(signature, static_cast<u64>(std::llround(primitive.axisX.y * 1000.0F)));
            signature = Mix(signature, static_cast<u64>(std::llround(primitive.axisZ.z * 1000.0F)));
            signature = Mix(signature, primitive.box ? 1U : 0U);
        }
    }
    if (signature == signature_)
    {
        return;
    }
    signature_ = signature;

    if (placed.empty())
    {
        volume_.ready = false;
        return;
    }

    // Volume bounds: union of every mesh grid box in frame space, plus margin.
    math::Double3 lo{1.0e300, 1.0e300, 1.0e300};
    math::Double3 hi{-1.0e300, -1.0e300, -1.0e300};
    for (const Placed& p : placed)
    {
        const auto& f = *p.field;
        for (u32 corner = 0U; corner < 8U; ++corner)
        {
            const f64 c[3] = {
                static_cast<f64>(f.origin[0]) +
                    ((corner & 1U) != 0U ? f.dimensions[0] - 1U : 0U) * static_cast<f64>(f.voxelSize),
                static_cast<f64>(f.origin[1]) +
                    ((corner & 2U) != 0U ? f.dimensions[1] - 1U : 0U) * static_cast<f64>(f.voxelSize),
                static_cast<f64>(f.origin[2]) +
                    ((corner & 4U) != 0U ? f.dimensions[2] - 1U : 0U) * static_cast<f64>(f.voxelSize)};
            const math::Double3 world{
                p.linear[0] * c[0] + p.linear[1] * c[1] + p.linear[2] * c[2] + p.translation.x,
                p.linear[3] * c[0] + p.linear[4] * c[1] + p.linear[5] * c[2] + p.translation.y,
                p.linear[6] * c[0] + p.linear[7] * c[1] + p.linear[8] * c[2] + p.translation.z};
            lo = {std::min(lo.x, world.x), std::min(lo.y, world.y), std::min(lo.z, world.z)};
            hi = {std::max(hi.x, world.x), std::max(hi.y, world.y), std::max(hi.z, world.z)};
        }
    }
    // Terrain and proxies only matter near the meshes: they get a wider ring.
    const f64 margin = (hasTerrain || hasPrimitives)
        ? static_cast<f64>(kSurroundMarginMeters)
        : static_cast<f64>(kVolumeMarginMeters);
    lo = {lo.x - margin, lo.y - margin, lo.z - margin};
    hi = {hi.x + margin, hi.y + margin, hi.z + margin};

    const f64 extent = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
    const f32 voxel = std::max(
        kMinimumVoxelMeters,
        static_cast<f32>(extent / static_cast<f64>(kMaximumDimension - 2U)));
    std::array<u32, 3> dims{
        static_cast<u32>(std::ceil((hi.x - lo.x) / voxel)) + 1U,
        static_cast<u32>(std::ceil((hi.y - lo.y) / voxel)) + 1U,
        static_cast<u32>(std::ceil((hi.z - lo.z) / voxel)) + 1U};

    const std::size_t voxels = static_cast<std::size_t>(dims[0]) * dims[1] * dims[2];

    if (voxels > allocatedVoxels_ || distance_ == nullptr)
    {
        Retired old;
        old.retireAtTick = tick_ + kRetireTicks;
        for (auto* slot : {&distance_, &distanceCorners_, &albedo_, &normal_, &emissive_, &radiance_})
        {
            if (*slot != nullptr)
            {
                old.buffers.push_back(std::move(*slot));
            }
        }
        if (!old.buffers.empty())
        {
            retired_.push_back(std::move(old));
        }
        const auto allocate = [&](const std::size_t bytes)
        {
            return device_.CreateBuffer({
                .sizeBytes = bytes,
                .usage = rhi::BufferUsage::Structured,
                .memory = rhi::MemoryUsage::GpuOnly,
                .initialState = rhi::ResourceState::UnorderedAccess});
        };
        radiance_ = allocate(voxels * sizeof(f32) * 4U);
        distance_ = allocate(voxels * sizeof(f32));
        distanceCorners_ = allocate(voxels * 4U * sizeof(u32));
        albedo_ = allocate(voxels * sizeof(u32));
        normal_ = allocate(voxels * sizeof(u32));
        emissive_ = allocate(voxels * sizeof(u32));
        allocatedVoxels_ = voxels;
    }

    const auto paramsBuffer = [&](const std::vector<f32>& values)
    {
        auto buffer = device_.CreateBuffer({
            .sizeBytes = values.size() * sizeof(f32),
            .usage = rhi::BufferUsage::Structured,
            .memory = rhi::MemoryUsage::HostVisible,
            .initialState = rhi::ResourceState::ShaderResource});
        std::memcpy(buffer->Map(), values.data(), values.size() * sizeof(f32));
        buffer->Unmap();
        return buffer;
    };

    Retired temporaries;
    temporaries.retireAtTick = tick_ + kRetireTicks;

    // Clear.
    {
        std::vector<f32> params(4U, 0.0F);
        params[0] = static_cast<f32>(voxels);
        auto buffer = paramsBuffer(params);
        commands.SetComputePipeline(*clearPipeline_);
        commands.SetComputeBuffer(0U, *buffer);
        commands.SetComputeBuffer(1U, *distance_);
        commands.SetComputeBuffer(2U, *albedo_);
        commands.SetComputeBuffer(3U, *normal_);
        commands.SetComputeBuffer(4U, *emissive_);
        commands.SetComputeBuffer(5U, *radiance_);
        commands.Dispatch(static_cast<u32>((voxels + 63U) / 64U), 1U, 1U);
        temporaries.buffers.push_back(std::move(buffer));
    }
    for (auto* buffer : {distance_.get(), albedo_.get(), normal_.get(), emissive_.get(), radiance_.get()})
    {
        commands.UavBarrier(*buffer);
    }

    // Merge each instance.
    for (const Placed& p : placed)
    {
        const auto& f = *p.field;
        const auto& a = p.linear;

        // Inverse of the linear part (uniform scale + rotation: transpose / s^2,
        // but invert fully so a non-uniform edit still maps correctly).
        const f64 det =
            a[0] * (a[4] * a[8] - a[5] * a[7]) -
            a[1] * (a[3] * a[8] - a[5] * a[6]) +
            a[2] * (a[3] * a[7] - a[4] * a[6]);
        if (std::abs(det) < 1.0e-12)
        {
            continue;
        }
        const f64 inv[9] = {
            (a[4] * a[8] - a[5] * a[7]) / det, (a[2] * a[7] - a[1] * a[8]) / det, (a[1] * a[5] - a[2] * a[4]) / det,
            (a[5] * a[6] - a[3] * a[8]) / det, (a[0] * a[8] - a[2] * a[6]) / det, (a[2] * a[3] - a[0] * a[5]) / det,
            (a[3] * a[7] - a[4] * a[6]) / det, (a[1] * a[6] - a[0] * a[7]) / det, (a[0] * a[4] - a[1] * a[3]) / det};

        // Translation relative to the volume origin (frame coordinates).
        const math::Double3 volumeOrigin{lo.x, lo.y, lo.z};
        const math::Double3 t{
            p.translation.x - volumeOrigin.x,
            p.translation.y - volumeOrigin.y,
            p.translation.z - volumeOrigin.z};

        const f64 scale = std::cbrt(std::abs(det));

        std::vector<f32> params(10U * 4U, 0.0F);
        const auto put4 = [&](const std::size_t slot, const f64 x, const f64 y, const f64 z, const f64 w)
        {
            params[slot * 4U + 0U] = static_cast<f32>(x);
            params[slot * 4U + 1U] = static_cast<f32>(y);
            params[slot * 4U + 2U] = static_cast<f32>(z);
            params[slot * 4U + 3U] = static_cast<f32>(w);
        };
        // q = inv * (p - t)  =>  row = (inv row, -inv row . t)
        for (std::size_t row = 0U; row < 3U; ++row)
        {
            const f64 w = -(inv[row * 3U] * t.x + inv[row * 3U + 1U] * t.y + inv[row * 3U + 2U] * t.z);
            put4(row, inv[row * 3U], inv[row * 3U + 1U], inv[row * 3U + 2U], w);
            put4(3U + row, a[row * 3U], a[row * 3U + 1U], a[row * 3U + 2U], 0.0);
        }
        put4(6U, scale, voxel, f.emissiveScale, 0.0);
        put4(7U, f.origin[0], f.origin[1], f.origin[2], f.voxelSize);
        put4(8U, f.dimensions[0], f.dimensions[1], f.dimensions[2], 0.0);
        put4(9U, dims[0], dims[1], dims[2], 0.0);

        auto buffer = paramsBuffer(params);
        commands.SetComputePipeline(*mergePipeline_);
        commands.SetComputeBuffer(0U, *buffer);
        commands.SetComputeBuffer(1U, *distance_);
        commands.SetComputeBuffer(2U, *albedo_);
        commands.SetComputeBuffer(3U, *normal_);
        commands.SetComputeBuffer(4U, *emissive_);
        commands.SetComputeBuffer(5U, *f.distance);
        commands.SetComputeBuffer(6U, *f.albedo);
        commands.SetComputeBuffer(7U, *f.normal);
        commands.SetComputeBuffer(8U, *f.emissive);
        commands.Dispatch((dims[0] + 3U) / 4U, (dims[1] + 3U) / 4U, (dims[2] + 3U) / 4U);
        temporaries.buffers.push_back(std::move(buffer));

        for (auto* target : {distance_.get(), albedo_.get(), normal_.get(), emissive_.get()})
        {
            commands.UavBarrier(*target);
        }
    }

    const auto barrierAll = [&]()
    {
        for (auto* target : {distance_.get(), albedo_.get(), normal_.get(), emissive_.get()})
        {
            commands.UavBarrier(*target);
        }
    };

    if (hasPrimitives && primitivePipeline_ != nullptr)
    {
        // Only primitives that reach the volume.
        std::vector<f32> data;
        u32 used = 0U;
        for (const SdfProxyPrimitive& primitive : extra->primitives)
        {
            const f64 reach = static_cast<f64>(std::max(
                {primitive.halfExtents.x, primitive.halfExtents.y, primitive.halfExtents.z})) * 1.75;
            if (primitive.center.x + reach < lo.x || primitive.center.x - reach > hi.x ||
                primitive.center.y + reach < lo.y || primitive.center.y - reach > hi.y ||
                primitive.center.z + reach < lo.z || primitive.center.z - reach > hi.z)
            {
                continue;
            }
            const f32 rel[3] = {
                static_cast<f32>(primitive.center.x - lo.x),
                static_cast<f32>(primitive.center.y - lo.y),
                static_cast<f32>(primitive.center.z - lo.z)};
            const f32 rows[16] = {
                rel[0], rel[1], rel[2], primitive.box ? 1.0F : 0.0F,
                primitive.axisX.x, primitive.axisX.y, primitive.axisX.z, primitive.halfExtents.x,
                primitive.axisY.x, primitive.axisY.y, primitive.axisY.z, primitive.halfExtents.y,
                primitive.axisZ.x, primitive.axisZ.y, primitive.axisZ.z, primitive.halfExtents.z};
            data.insert(data.end(), std::begin(rows), std::end(rows));
            ++used;
        }
        if (used > 0U)
        {
            std::vector<f32> params(12U, 0.0F);
            params[0] = static_cast<f32>(dims[0]);
            params[1] = static_cast<f32>(dims[1]);
            params[2] = static_cast<f32>(dims[2]);
            params[3] = static_cast<f32>(used);
            params[4] = voxel;
            params[8] = 0.55F;
            params[9] = 0.53F;
            params[10] = 0.50F;
            auto paramsBuf = paramsBuffer(params);
            auto dataBuf = paramsBuffer(data);
            commands.SetComputePipeline(*primitivePipeline_);
            commands.SetComputeBuffer(0U, *paramsBuf);
            commands.SetComputeBuffer(1U, *distance_);
            commands.SetComputeBuffer(2U, *albedo_);
            commands.SetComputeBuffer(3U, *normal_);
            commands.SetComputeBuffer(4U, *emissive_);
            commands.SetComputeBuffer(5U, *dataBuf);
            commands.Dispatch((dims[0] + 3U) / 4U, (dims[1] + 3U) / 4U, (dims[2] + 3U) / 4U);
            temporaries.buffers.push_back(std::move(paramsBuf));
            temporaries.buffers.push_back(std::move(dataBuf));
            barrierAll();
        }
    }

    if (hasTerrain && terrainPipeline_ != nullptr)
    {
        const SdfTerrainPatch& patch = *extra->terrain;
        std::vector<f32> params(28U, 0.0F);
        params[0] = static_cast<f32>(dims[0]);
        params[1] = static_cast<f32>(dims[1]);
        params[2] = static_cast<f32>(dims[2]);
        params[4] = voxel;
        params[5] = patch.cellMeters;
        params[6] = static_cast<f32>(patch.count);
        params[8] = static_cast<f32>(patch.center.x - lo.x);
        params[9] = static_cast<f32>(patch.center.y - lo.y);
        params[10] = static_cast<f32>(patch.center.z - lo.z);
        params[12] = patch.east.x;
        params[13] = patch.east.y;
        params[14] = patch.east.z;
        params[16] = patch.north.x;
        params[17] = patch.north.y;
        params[18] = patch.north.z;
        params[20] = patch.up.x;
        params[21] = patch.up.y;
        params[22] = patch.up.z;
        params[24] = patch.albedo.x;
        params[25] = patch.albedo.y;
        params[26] = patch.albedo.z;
        auto paramsBuf = paramsBuffer(params);
        auto heightsBuf = paramsBuffer(patch.heights);
        commands.SetComputePipeline(*terrainPipeline_);
        commands.SetComputeBuffer(0U, *paramsBuf);
        commands.SetComputeBuffer(1U, *distance_);
        commands.SetComputeBuffer(2U, *albedo_);
        commands.SetComputeBuffer(3U, *normal_);
        commands.SetComputeBuffer(4U, *emissive_);
        commands.SetComputeBuffer(5U, *heightsBuf);
        commands.Dispatch((dims[0] + 3U) / 4U, (dims[1] + 3U) / 4U, (dims[2] + 3U) / 4U);
        temporaries.buffers.push_back(std::move(paramsBuf));
        temporaries.buffers.push_back(std::move(heightsBuf));
        barrierAll();
    }

    // Repack the finished field for the gather's sphere trace.
    if (cornerPipeline_ != nullptr)
    {
        std::vector<f32> params(4U, 0.0F);
        params[0] = static_cast<f32>(dims[0]);
        params[1] = static_cast<f32>(dims[1]);
        params[2] = static_cast<f32>(dims[2]);
        auto paramsBuf = paramsBuffer(params);
        commands.SetComputePipeline(*cornerPipeline_);
        commands.SetComputeBuffer(0U, *paramsBuf);
        commands.SetComputeBuffer(1U, *distance_);
        commands.SetComputeBuffer(2U, *distanceCorners_);
        commands.Dispatch((dims[0] + 3U) / 4U, (dims[1] + 3U) / 4U, (dims[2] + 3U) / 4U);
        commands.UavBarrier(*distanceCorners_);
        temporaries.buffers.push_back(std::move(paramsBuf));
    }
    retired_.push_back(std::move(temporaries));

    volume_.ready = true;
    volume_.originInFrameMeters = {lo.x, lo.y, lo.z};
    volume_.voxelSize = voxel;
    volume_.dimensions = dims;
    volume_.revision = ++revision_;
    volume_.distance = distance_.get();
    volume_.distanceCorners = distanceCorners_.get();
    volume_.albedo = albedo_.get();
    volume_.normal = normal_.get();
    volume_.emissive = emissive_.get();
    volume_.radiance = radiance_.get();
    volume_.emissiveScale = placed.front().field->emissiveScale;
}

void MeshSdfScene::Light(
    rhi::CommandList& commands,
    const SdfLightingInput& input)
{
    if (!volume_.ready || lightPipeline_ == nullptr)
    {
        return;
    }

    {
        const auto same =
            [](const math::Float3& a, const math::Float3& b)
            {
                return a.x == b.x && a.y == b.y && a.z == b.z;
            };
        const bool unchanged =
            lastLightRevision_ == volume_.revision &&
            same(lastLightInput_.toSun, input.toSun) &&
            same(lastLightInput_.skyIrradiance, input.skyIrradiance) &&
            same(lastLightInput_.up, input.up) &&
            lastLightInput_.sunIrradiance == input.sunIrradiance;
        if (unchanged)
        {
            if (stableLightFrames_ < 0xFFFFU)
            {
                ++stableLightFrames_;
            }
        }
        else
        {
            stableLightFrames_ = 0U;
            lastLightInput_ = input;
            lastLightRevision_ = volume_.revision;
        }

        // 4 quarter-refreshes per cycle x up to 8 bounces of settling.
        constexpr u32 kSettledFrames = 48U;
        if (stableLightFrames_ > kSettledFrames)
        {
            return;
        }
    }

    std::vector<f32> params(16U, 0.0F);
    params[0] = static_cast<f32>(volume_.dimensions[0]);
    params[1] = static_cast<f32>(volume_.dimensions[1]);
    params[2] = static_cast<f32>(volume_.dimensions[2]);
    params[3] = volume_.voxelSize;
    params[4] = input.toSun.x;
    params[5] = input.toSun.y;
    params[6] = input.toSun.z;
    params[7] = input.sunIrradiance;
    params[8] = input.skyIrradiance.x;
    params[9] = input.skyIrradiance.y;
    params[10] = input.skyIrradiance.z;
    params[11] = static_cast<f32>(input.frame);
    params[12] = input.up.x;
    params[13] = input.up.y;
    params[14] = input.up.z;
    params[15] = volume_.emissiveScale;

    auto buffer = device_.CreateBuffer({
        .sizeBytes = params.size() * sizeof(f32),
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::ShaderResource});
    std::memcpy(buffer->Map(), params.data(), params.size() * sizeof(f32));
    buffer->Unmap();

    commands.SetComputePipeline(*lightPipeline_);
    commands.SetComputeBuffer(0U, *buffer);
    commands.SetComputeBuffer(1U, *distance_);
    commands.SetComputeBuffer(2U, *albedo_);
    commands.SetComputeBuffer(3U, *normal_);
    commands.SetComputeBuffer(4U, *emissive_);
    commands.SetComputeBuffer(5U, *radiance_);
    commands.Dispatch(
        (volume_.dimensions[0] + 3U) / 4U,
        (volume_.dimensions[1] + 3U) / 4U,
        (volume_.dimensions[2] + 3U) / 4U);
    commands.UavBarrier(*radiance_);

    Retired temporary;
    temporary.buffers.push_back(std::move(buffer));
    temporary.retireAtTick = tick_ + kRetireTicks;
    retired_.push_back(std::move(temporary));
}

MeshSdfDebugRenderer::MeshSdfDebugRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler)
    : device_(device)
{
    const auto compiled = compiler.Compile({
        .source = std::string(kOctahedral) + kDebugShader,
        .entryPoint = "main",
        .stage = shader::Stage::Compute,
        .debug = false});
    pipeline_ = device.CreateComputePipeline({
        .computeShader = {
            .data = compiled.bytecode.data(),
            .size = compiled.bytecode.size()},
        .pushConstantDwords = 24U,
        .shaderResourceBuffers = 4U,
        .storageTextures = 1U,
        .sampledTextures = 1U});
}

void MeshSdfDebugRenderer::Draw(
    rhi::CommandList& commands,
    const SdfSceneVolume& volume,
    rhi::Texture& color,
    rhi::Texture& destination,
    const u32 width,
    const u32 height,
    const lighting::LightingView& view,
    const math::Float3& directionToSun,
    const u32 mode)
{
    if (pipeline_ == nullptr || !volume.ready || width == 0U || height == 0U)
    {
        return;
    }

    const math::Double3 originRelative =
        volume.originInFrameMeters - view.cameraPositionInFrameMeters;
    const f32 aspect = static_cast<f32>(width) / static_cast<f32>(height);

    const std::array<u32, 24> constants{
        width, height, mode, 0U,
        Bits(view.forward.x), Bits(view.forward.y), Bits(view.forward.z), Bits(aspect),
        Bits(view.up.x), Bits(view.up.y), Bits(view.up.z),
        Bits(std::tan(view.verticalFovRadians * 0.5F)),
        Bits(static_cast<f32>(originRelative.x)),
        Bits(static_cast<f32>(originRelative.y)),
        Bits(static_cast<f32>(originRelative.z)),
        Bits(volume.voxelSize),
        Bits(static_cast<f32>(volume.dimensions[0])),
        Bits(static_cast<f32>(volume.dimensions[1])),
        Bits(static_cast<f32>(volume.dimensions[2])), 0U,
        Bits(directionToSun.x), Bits(directionToSun.y), Bits(directionToSun.z),
        Bits(std::max(static_cast<f32>(volume.dimensions[0]) * volume.voxelSize, 1.0F))};

    commands.SetComputePipeline(*pipeline_);
    commands.SetComputeConstants(constants);
    commands.SetComputeBuffer(0U, *volume.distance);
    commands.SetComputeBuffer(1U, *volume.albedo);
    commands.SetComputeBuffer(2U, *volume.normal);
    commands.SetComputeBuffer(3U, *volume.radiance);
    commands.SetComputeStorageTexture(0U, destination);
    commands.SetComputeTexture(0U, color);
    commands.Dispatch((width + 7U) / 8U, (height + 7U) / 8U, 1U);
}
} // namespace orbit::mesh_render
