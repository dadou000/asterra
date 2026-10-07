#pragma once

namespace orbit::lighting
{
// HLSL for sphere tracing the merged mesh distance field (see mesh_render's
// MeshSdfScene) and reading the surface radiance stored beside it. Shared by
// the voxel lighting pass and the final gather's world-space fallback.
//
// The including shader must declare, before this text:
//   RWStructuredBuffer<float>  g_sdfDist;
//   RWStructuredBuffer<uint>   g_sdfAlbedo;    // alpha != 0 on surface voxels
//   RWStructuredBuffer<uint>   g_sdfNormal;    // octahedral, world space
//   RWStructuredBuffer<float4> g_sdfRadiance;  // outgoing diffuse radiance
// and these macros (positions are in whatever space SDF_ORIGIN is given in):
//   SDF_ORIGIN  float3: position of voxel (0,0,0)'s centre
//   SDF_VOXEL   float:  voxel edge length
//   SDF_DIMS    int3:   volume dimensions
inline constexpr const char* kSdfTraceHlsl = R"(
uint SdfIndex(int3 c)
{
    const int3 dims = SDF_DIMS;
    return (uint(c.z) * uint(dims.y) + uint(c.y)) * uint(dims.x) + uint(c.x);
}

float3 SdfUnpackOct(uint packed)
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

// Trilinear distance at a position; outside the volume the distance to the
// volume box is added so rays leave it cleanly.
float SdfDistance(float3 position)
{
    const int3 dims = SDF_DIMS;
    const float3 gc = (position - SDF_ORIGIN) / SDF_VOXEL;
    const float3 clamped = clamp(gc, 0.0, float3(dims - 1));
    const float outside = length((gc - clamped) * SDF_VOXEL);
    const int3 i0 = int3(floor(clamped));
    const int3 i1 = min(i0 + 1, dims - 1);
    const float3 f = clamped - float3(i0);

    const float c000 = g_sdfDist[SdfIndex(int3(i0.x, i0.y, i0.z))];
    const float c100 = g_sdfDist[SdfIndex(int3(i1.x, i0.y, i0.z))];
    const float c010 = g_sdfDist[SdfIndex(int3(i0.x, i1.y, i0.z))];
    const float c110 = g_sdfDist[SdfIndex(int3(i1.x, i1.y, i0.z))];
    const float c001 = g_sdfDist[SdfIndex(int3(i0.x, i0.y, i1.z))];
    const float c101 = g_sdfDist[SdfIndex(int3(i1.x, i0.y, i1.z))];
    const float c011 = g_sdfDist[SdfIndex(int3(i0.x, i1.y, i1.z))];
    const float c111 = g_sdfDist[SdfIndex(int3(i1.x, i1.y, i1.z))];

    return outside + lerp(
        lerp(lerp(c000, c100, f.x), lerp(c010, c110, f.x), f.y),
        lerp(lerp(c001, c101, f.x), lerp(c011, c111, f.x), f.y),
        f.z);
}

// Sphere traces from `origin` along `direction` for up to `maxDistance`.
// Returns true with the hit point when a surface is reached.
bool SdfTrace(float3 origin, float3 direction, float maxDistance, out float3 hitPosition, out float hitT)
{
    hitPosition = origin;
    hitT = 0.0;
    float t = 0.25 * SDF_VOXEL;
    [loop]
    for (uint i = 0u; i < 56u; ++i)
    {
        const float3 position = origin + direction * t;
        const float d = SdfDistance(position);
        if (d < 0.4 * SDF_VOXEL)
        {
            hitPosition = position;
            hitT = t;
            return true;
        }
        t += max(d * 0.9, 0.2 * SDF_VOXEL);
        if (t > maxDistance)
        {
            break;
        }
    }
    return false;
}

// Soft sun shadow: 1 lit .. 0 occluded, from the smallest distance-to-ray
// ratio along the way (a cone of half angle ~ 1 / softness).
float SdfSoftShadow(float3 origin, float3 toLight, float maxDistance, float softness)
{
    float visibility = 1.0;
    float t = 0.5 * SDF_VOXEL;
    [loop]
    for (uint i = 0u; i < 40u; ++i)
    {
        const float d = SdfDistance(origin + toLight * t);
        if (d < 0.05 * SDF_VOXEL)
        {
            return 0.0;
        }
        visibility = min(visibility, softness * d / t);
        t += max(d, 0.3 * SDF_VOXEL);
        if (t > maxDistance)
        {
            break;
        }
    }
    return saturate(visibility);
}

// Outgoing radiance of the surface voxel nearest to `position` whose normal
// faces `towardViewer` (the ray came from there).
float3 SdfFetchRadiance(float3 position, float3 towardViewer)
{
    const int3 dims = SDF_DIMS;
    const float3 gc = (position - SDF_ORIGIN) / SDF_VOXEL;
    const int3 base = int3(floor(gc));
    float best = 1.0e9;
    float3 radiance = 0.0;
    [unroll]
    for (int k = 0; k < 8; ++k)
    {
        const int3 c = clamp(base + int3(k & 1, (k >> 1) & 1, (k >> 2) & 1), 0, dims - 1);
        const uint index = SdfIndex(c);
        if ((g_sdfAlbedo[index] >> 24) != 0u)
        {
            const float3 n = SdfUnpackOct(g_sdfNormal[index]);
            if (dot(n, towardViewer) > -0.1)
            {
                const float d = length(float3(c) - gc);
                if (d < best)
                {
                    best = d;
                    radiance = g_sdfRadiance[index].rgb;
                }
            }
        }
    }
    return radiance;
}
)";
} // namespace orbit::lighting
