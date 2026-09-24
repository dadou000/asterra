#pragma once
namespace orbit::terrain_gpu::detail
{
// Composites a GPU-computed hydrology region tile's elevation delta
// (GpuErosion's net erosion/deposition, including depression-fill's
// lake raise -- see GpuHydrologyRegion) directly into an
// already-generated clipmap sample buffer, in place. Runs as a
// separate pass *after* FieldGenerationCompute.hpp's dispatch for the
// same physical region, rather than folding the sampling into that
// shader -- keeps the (already large, already-tested) base field
// generator untouched, at the cost of recomputing each sample's
// direction a second time here.
//
// Reprojects each clipmap sample's world direction into the region
// tile's own surface frame using the same tangent-plane
// angle/projection approach FieldGenerationCompute.hpp's morph-target
// code already uses for the coarse/fine frame conversion -- not a new
// technique, just applied against a different target frame -- then
// bilinear-samples the tile's dense delta grid at that offset. Samples
// outside the tile's extent (or within its edge fade band) are left
// unmodified, so a caller can apply at most one (the best/nearest
// covering) tile per dispatch without needing to also implement
// cross-tile blending here.
inline constexpr const char* kRegionDeltaComputeShader = R"(
[[vk::binding(0, 0)]]
ByteAddressBuffer g_regionDelta : register(t0);

[[vk::binding(1, 0)]]
RWByteAddressBuffer g_samples : register(u1);

struct PushConstants
{
    // Every float4 field first -- each is naturally 16-byte aligned
    // back to back this way, with no implicit padding for the C++
    // side to have to reproduce (see GpuRegionDelta.cpp's push
    // constant layout comment).
    // fineUp.w / fineEast.w carry the clipmap window center offset
    // inside the stable spherical lattice frame.
    float4 fineUp;
    float4 fineEast;
    float4 fineNorth;
    float4 regionUp;
    float4 regionEast;
    float4 regionNorth;

    uint resolution;
    float spacingMeters;
    uint originX;
    uint originY;
    uint regionX;
    uint regionY;
    uint regionWidth;
    uint regionHeight;
    float radius;
    float regionHalfExtentMeters;
    float regionSpacingMeters;
    uint regionResolution;
    // A fraction of regionHalfExtentMeters, inside which the delta
    // applies at full strength; between that and the tile edge it
    // fades linearly to zero, so neighboring tiles (built at
    // different times, potentially with visibly different deltas at
    // their own edges) don't create a hard seam.
    float edgeFadeStartDot;
};

[[vk::push_constant]] PushConstants g_pc;

static const uint kSampleStrideBytes = 32u;

float3 DirectionAtSurfaceOffset(
    float3 up, float3 east, float3 north,
    float2 offsetMeters, float radius)
{
    float distance = length(offsetMeters);

    if (distance < 1.0e-6)
    {
        return up;
    }

    float2 tangentDirection2d = offsetMeters / distance;
    float3 tangent = east * tangentDirection2d.x + north * tangentDirection2d.y;
    float angle = distance / radius;

    return up * cos(angle) + tangent * sin(angle);
}

float BilinearSampleDelta(float2 offsetMeters)
{
    float halfExtent = g_pc.regionHalfExtentMeters;
    float spacing = g_pc.regionSpacingMeters;
    uint resolution = g_pc.regionResolution;

    float gx = (offsetMeters.x + halfExtent) / spacing;
    float gy = (offsetMeters.y + halfExtent) / spacing;

    float maxCoord = float(resolution) - 1.0;
    gx = clamp(gx, 0.0, maxCoord);
    gy = clamp(gy, 0.0, maxCoord);

    uint x0 = uint(floor(gx));
    uint y0 = uint(floor(gy));
    uint x1 = min(x0 + 1u, resolution - 1u);
    uint y1 = min(y0 + 1u, resolution - 1u);

    float fx = gx - float(x0);
    float fy = gy - float(y0);

    float d00 = asfloat(g_regionDelta.Load((y0 * resolution + x0) * 4u));
    float d10 = asfloat(g_regionDelta.Load((y0 * resolution + x1) * 4u));
    float d01 = asfloat(g_regionDelta.Load((y1 * resolution + x0) * 4u));
    float d11 = asfloat(g_regionDelta.Load((y1 * resolution + x1) * 4u));

    float top = lerp(d00, d10, fx);
    float bottom = lerp(d01, d11, fx);
    return lerp(top, bottom, fy);
}

uint WrapIndex(int value, uint resolution)
{
    int wrapped = value % int(resolution);
    if (wrapped < 0)
    {
        wrapped += int(resolution);
    }
    return uint(wrapped);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
    if (dispatchId.x >= g_pc.regionWidth || dispatchId.y >= g_pc.regionHeight)
    {
        return;
    }

    uint physicalX = g_pc.regionX + dispatchId.x;
    uint physicalY = g_pc.regionY + dispatchId.y;

    uint logicalX = WrapIndex(int(physicalX) - int(g_pc.originX), g_pc.resolution);
    uint logicalY = WrapIndex(int(physicalY) - int(g_pc.originY), g_pc.resolution);

    float halfCells = (float(g_pc.resolution) - 1.0) * 0.5;
    float2 fineOffsetMeters =
        float2(g_pc.fineUp.w, g_pc.fineEast.w) +
        (float2(float(logicalX), float(logicalY)) - halfCells) *
            g_pc.spacingMeters;

    // Sample the displaced vertex's actual location. Applying erosion at the
    // unmorphed fine location gives coincident child/parent vertices different
    // heights and reopens the ring seam after base terrain generation.
    if (g_pc.regionEast.w > 0.5 && g_pc.regionUp.w > g_pc.fineNorth.w)
    {
        float2 localOffset = fineOffsetMeters - float2(g_pc.fineUp.w, g_pc.fineEast.w);
        float edge = max(abs(localOffset.x), abs(localOffset.y));
        float t = saturate((edge - g_pc.fineNorth.w) / (g_pc.regionUp.w - g_pc.fineNorth.w));
        float2 target = asfloat(g_samples.Load2(
            (physicalY * g_pc.resolution + physicalX) * kSampleStrideBytes + 4u));
        fineOffsetMeters = lerp(fineOffsetMeters, target, t * t * (3.0 - 2.0 * t));
    }
    float3 direction = DirectionAtSurfaceOffset(
        g_pc.fineUp.xyz, g_pc.fineEast.xyz, g_pc.fineNorth.xyz,
        fineOffsetMeters, g_pc.radius);

    // Reproject into the region tile's own tangent frame -- same
    // angle/tangent-projection approach as
    // FieldGenerationCompute.hpp's morph-target code, just against
    // the hydrology tile's frame instead of a coarser clipmap level's.
    float cosine = clamp(dot(g_pc.regionUp.xyz, direction), -1.0, 1.0);
    float3 tangent = direction - g_pc.regionUp.xyz * cosine;
    float tangentLength = length(tangent);
    float angle = atan2(tangentLength, cosine);
    float2 regionOffsetMeters = float2(0.0, 0.0);

    if (angle > 1.0e-6)
    {
        if (tangentLength > 1.0e-6)
        {
            float3 tangentDirection = tangent / tangentLength;
            float distance = angle * g_pc.radius;
            regionOffsetMeters = float2(
                dot(tangentDirection, g_pc.regionEast.xyz) * distance,
                dot(tangentDirection, g_pc.regionNorth.xyz) * distance);
        }
    }

    float edgeDot = max(
        abs(regionOffsetMeters.x), abs(regionOffsetMeters.y)) /
        max(g_pc.regionHalfExtentMeters, 0.0001);

    if (edgeDot >= 1.0)
    {
        // Outside the tile entirely -- leave this sample untouched.
        return;
    }

    float fade = 1.0 - saturate(
        (edgeDot - g_pc.edgeFadeStartDot) /
        max(1.0 - g_pc.edgeFadeStartDot, 0.0001));

    float delta = BilinearSampleDelta(regionOffsetMeters) * fade;

    if (delta == 0.0)
    {
        return;
    }

    uint physicalIndex = physicalY * g_pc.resolution + physicalX;
    uint byteOffset = physicalIndex * kSampleStrideBytes;

    float elevation = asfloat(g_samples.Load(byteOffset + 0u));
    g_samples.Store(byteOffset + 0u, asuint(elevation + delta));
}
)";
} // namespace orbit::terrain_gpu::detail
