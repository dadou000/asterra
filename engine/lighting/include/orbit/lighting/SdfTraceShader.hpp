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
#ifdef SDF_DIST_CORNERS
// Corner-packed variant: g_sdfDist holds one uint4 per voxel with that cell's
// eight trilinear corners as f16 (x: c000|c100, y: c010|c110, z: c001|c101,
// w: c011|c111), so a trace step is one 16-byte load instead of eight
// scattered 4-byte ones.
float2 SdfUnpackHalves(uint packed)
{
    return float2(f16tof32(packed & 0xFFFFu), f16tof32(packed >> 16));
}

float SdfDistance(float3 position)
{
    const int3 dims = SDF_DIMS;
    const float3 gc = (position - SDF_ORIGIN) / SDF_VOXEL;
    const float3 clamped = clamp(gc, 0.0, float3(dims - 1));
    const float outside = length((gc - clamped) * SDF_VOXEL);
    const int3 i0 = int3(floor(clamped));
    const float3 f = clamped - float3(i0);

    const uint4 packed = g_sdfDist[SdfIndex(i0)];
    const float2 z0a = SdfUnpackHalves(packed.x);
    const float2 z0b = SdfUnpackHalves(packed.y);
    const float2 z1a = SdfUnpackHalves(packed.z);
    const float2 z1b = SdfUnpackHalves(packed.w);

    return outside + lerp(
        lerp(lerp(z0a.x, z0a.y, f.x), lerp(z0b.x, z0b.y, f.x), f.y),
        lerp(lerp(z1a.x, z1a.y, f.x), lerp(z1b.x, z1b.y, f.x), f.y),
        f.z);
}
#else
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
#endif

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
        if (t > maxDistance) break;
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

// Reconstruct attributes from valid corners of the same facing surface.
// Opposing walls never average together; empty corners have no weight.
struct SdfSurfaceSample
{
    float3 radiance;
    float3 albedo;
    float3 normal;
    float valid;
};
SdfSurfaceSample SdfFetchSurface(float3 position, float3 towardViewer)
{
    SdfSurfaceSample sample = (SdfSurfaceSample)0;
    const int3 dims = SDF_DIMS;
    const float3 gc = (position - SDF_ORIGIN) / SDF_VOXEL;
    if (any(gc < 0.0) || any(gc > float3(dims-1))) return sample;
    const int3 base = int3(floor(gc));
    const float3 fraction = gc - float3(base);
    float best = 1.0e9;
    uint nearest = 0u;
    float3 referenceNormal = 0.0;
    [unroll] for (int k = 0; k < 8; ++k)
    {
        const int3 c = min(base + int3(k & 1, (k >> 1) & 1, (k >> 2) & 1), dims - 1);
        const uint index = SdfIndex(c);
        if ((g_sdfAlbedo[index] >> 24) == 0u) continue;
        const float3 normal = SdfUnpackOct(g_sdfNormal[index]);
        if (dot(normal, towardViewer) <= -0.1) continue;
        const float distance = length(float3(c) - gc);
        if (distance < best) { best = distance; nearest = index; referenceNormal = normal; }
    }
    if (best > 1.0e8) return sample;
    float weightSum = 0.0;
    [unroll] for (int k = 0; k < 8; ++k)
    {
        const int3 corner = int3(k & 1, (k >> 1) & 1, (k >> 2) & 1);
        const uint index = SdfIndex(min(base + corner, dims - 1));
        const uint packed = g_sdfAlbedo[index];
        if ((packed >> 24) == 0u) continue;
        const float3 normal = SdfUnpackOct(g_sdfNormal[index]);
        if (dot(normal, referenceNormal) < 0.75 || dot(normal, towardViewer) <= -0.1) continue;
        const float3 w = lerp(1.0-fraction, fraction, float3(corner));
        const float weight = w.x*w.y*w.z;
        sample.radiance += max(g_sdfRadiance[index].rgb, 0.0) * weight;
        sample.albedo += float3(packed & 255u, (packed >> 8) & 255u, (packed >> 16) & 255u) / 255.0 * weight;
        sample.normal += normal * weight;
        weightSum += weight;
    }
    if (weightSum < 1.0e-5)
    {
        const uint packed = g_sdfAlbedo[nearest];
        sample.radiance = max(g_sdfRadiance[nearest].rgb, 0.0);
        sample.albedo = float3(packed & 255u, (packed >> 8) & 255u, (packed >> 16) & 255u) / 255.0;
        sample.normal = referenceNormal;
    }
    else
    {
        sample.radiance /= weightSum;
        sample.albedo /= weightSum;
        sample.normal = normalize(sample.normal);
    }
    sample.valid = 1.0;
    return sample;
}
float3 SdfFetchRadiance(float3 position, float3 towardViewer)
{
    return SdfFetchSurface(position, towardViewer).radiance;
}
)";
} // namespace orbit::lighting
