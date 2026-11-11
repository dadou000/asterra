#pragma once

#include <orbit/core/Types.hpp>

namespace orbit::terrain
{
// Terrain sky-visibility ("ambient") channel carried by the toroidal clipmap.
//
// One value per clipmap sample, stored in a buffer that shares the level's
// toroidal addressing (physical = (logical + origin) % resolution), so it
// scrolls, streams and covers exactly like the terrain it describes. It holds
// geometry only (how open the sky is above the sample, and which way the open
// part leans), never the time of day or sky colour: lighting multiplies by the
// current sky at shading time, so it never goes stale when the sun moves.
//
// Texel (u32): bits 0..7 sky visibility (0..255 = 0..1), 8..15 lean east and
// 16..23 lean north (offset-128 snorm, units of 1/3), 24..31 valid flag.
//
// Per-level info (80 bytes, five uint4), indexed by clipmap level:
//   [0] up.xyz, planet radius (f32)
//   [1] east.xyz, sample spacing metres (f32)
//   [2] north.xyz, (resolution - 1) / 2 (f32)
//   [3] centre offset x, y (f32), origin x, y (u32)
//   [4] resolution (u32), first sample of the level in the ambient buffer (u32),
//       flags (bit 0 = level active and its ambient valid), reserved
// The basis vectors are body-fixed, the same frame lighting works in.
inline constexpr u32 kTerrainAmbientLevelInfoBytes = 80U;
inline constexpr u32 kTerrainAmbientMaxLevels = 32U;
inline constexpr u32 kTerrainAmbientAzimuths = 16U;
inline constexpr u32 kTerrainAmbientTexelBytes = 4U;

// HLSL shared by the compute pass and the lighting lookup. The includer
// defines TA_BUFFER (ByteAddressBuffer or RWByteAddressBuffer) first.
inline constexpr const char* kTerrainAmbientCommonShader = R"(
float3 TaSurfaceDirection(float2 offsetMeters, float planetRadius, float3 up, float3 east, float3 north)
{
    float3 direction = up;
    const float distanceMeters = length(offsetMeters);
    if (distanceMeters > 0.0001)
    {
        const float3 tangentDirection = normalize(east * offsetMeters.x + north * offsetMeters.y);
        const float angle = distanceMeters / planetRadius;
        float sineAngle;
        float cosineAngle;
        if (angle < 0.2)
        {
            const float a2 = angle * angle;
            sineAngle = angle * (1.0 - a2 / 6.0 * (1.0 - a2 / 20.0 * (1.0 - a2 / 42.0)));
            cosineAngle = 1.0 - a2 / 2.0 * (1.0 - a2 / 12.0 * (1.0 - a2 / 30.0));
        }
        else
        {
            sineAngle = sin(angle);
            cosineAngle = cos(angle);
        }
        direction = normalize(up * cosineAngle + tangentDirection * sineAngle);
    }
    return direction;
}

// Inverse of TaSurfaceDirection plus the level's lattice: the fractional
// logical sample coordinate of a body-fixed direction.
float2 TaLogicalFromDirection(float3 d, float3 up, float3 east, float3 north, float planetRadius,
    float2 centerOffset, float spacing, float halfCells)
{
    const float cosA = dot(d, up);
    const float3 tangent = d - up * cosA;
    const float tangentLength = length(tangent);
    float2 offset = float2(0.0, 0.0);
    if (tangentLength > 1.0e-9)
    {
        const float angle = atan2(tangentLength, cosA);
        const float3 t = tangent / tangentLength;
        offset = float2(dot(t, east), dot(t, north)) * (angle * planetRadius);
    }
    return (offset - centerOffset) / spacing + halfCells;
}

uint TaPhysicalIndex(uint x, uint y, uint resolution, uint originX, uint originY)
{
    return ((y + originY) % resolution) * resolution + ((x + originX) % resolution);
}

// x = visibility, yz = lean (east, north), w = valid.
float4 TaDecode(uint packed)
{
    const float visibility = float(packed & 0xFFu) / 255.0;
    const float leanEast = (float((packed >> 8u) & 0xFFu) - 128.0) / 127.0 / 3.0;
    const float leanNorth = (float((packed >> 16u) & 0xFFu) - 128.0) / 127.0 / 3.0;
    return float4(visibility, leanEast, leanNorth, float((packed >> 24u) & 0xFFu));
}

// Bilinear read; false when the footprint leaves the grid or touches a texel
// that has not been computed yet.
bool TaSampleLevel(TA_BUFFER ambient, uint firstSample, uint resolution, uint originX, uint originY,
    float2 logical, out float visibility, out float2 lean)
{
    visibility = 1.0;
    lean = float2(0.0, 0.0);
    const float top = float(resolution - 1u);
    if (logical.x < 0.0 || logical.y < 0.0 || logical.x > top || logical.y > top)
        return false;
    const float2 base = floor(min(logical, top - 0.001));
    const float2 f = logical - base;
    const uint x0 = uint(base.x), y0 = uint(base.y);
    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    [unroll]
    for (uint corner = 0u; corner < 4u; ++corner)
    {
        const uint cx = x0 + (corner & 1u), cy = y0 + (corner >> 1u);
        const float w = ((corner & 1u) != 0u ? f.x : 1.0 - f.x) * ((corner >> 1u) != 0u ? f.y : 1.0 - f.y);
        const float4 t = TaDecode(ambient.Load((firstSample + TaPhysicalIndex(cx, cy, resolution, originX, originY)) * 4u));
        if (t.w < 0.5)
            return false;
        sum += float4(t.xyz, 1.0) * w;
    }
    visibility = sum.x;
    lean = sum.yz;
    return true;
}
)";

// Lighting lookup (needs kTerrainAmbientCommonShader with TA_BUFFER =
// ByteAddressBuffer first). Returns (visibility, bent normal xyz), or
// visibility < 0 when no level covers `position` (body-fixed metres).
inline constexpr const char* kTerrainAmbientLookupShader = R"(
float4 TerrainAmbientLookup(ByteAddressBuffer levels, ByteAddressBuffer ambient, uint levelCount, float3 position)
{
    const float3 d = normalize(position);
    for (uint level = 0u; level < levelCount; ++level)
    {
        const uint base = level * 80u;
        const uint4 info4 = levels.Load4(base + 64u);
        if ((info4.z & 1u) == 0u)
            continue;
        const float4 a = asfloat(levels.Load4(base));
        const float4 b = asfloat(levels.Load4(base + 16u));
        const float4 c = asfloat(levels.Load4(base + 32u));
        const float2 centerOffset = asfloat(levels.Load2(base + 48u));
        const uint2 origin = levels.Load2(base + 56u);
        const uint resolution = info4.x;
        const float2 logical = TaLogicalFromDirection(d, a.xyz, b.xyz, c.xyz, a.w, centerOffset, b.w, c.w);
        // Stay off the edge, where horizons were truncated and the finer
        // level's neighbour is about to take over.
        const float margin = 2.0;
        if (logical.x < margin || logical.y < margin ||
            logical.x > float(resolution - 1u) - margin || logical.y > float(resolution - 1u) - margin)
            continue;
        float visibility;
        float2 lean;
        if (!TaSampleLevel(ambient, info4.y, resolution, origin.x, origin.y, logical, visibility, lean))
            continue;
        const float3 east = normalize(b.xyz - d * dot(b.xyz, d));
        const float3 north = normalize(c.xyz - d * dot(c.xyz, d));
        return float4(visibility, normalize(d + 2.0 * (east * lean.x + north * lean.y)));
    }
    return float4(-1.0, 0.0, 0.0, 0.0);
}
)";
} // namespace orbit::terrain
