#pragma once
namespace orbit::terrain_render::detail
{
// The clipmap vertex shader. The terrain pass uses it as is (the mesh sits on
// the true bed). The near-field water pass derives its vertex shader from it
// (BuildClipmapWaterVertexShader), so the two share one morph, residency and
// hole implementation and cannot drift apart.
inline constexpr const char* kClipmapVertexShader = R"(
struct DrawConstants
{
    row_major float4x4 g_mvp;
    float4 g_planet;
    float4 g_centerUpAndOriginX;
    float4 g_centerEastAndOriginY;
    float4 g_centerNorthAndMorphStart;
    float4 g_morph;
    float4 g_debug;
    float4 g_centerOffsetMeters;
    float4 g_observerEastBody;
    float4 g_observerUpBody;
    float4 g_observerNorthBody;
};
[[vk::push_constant]] DrawConstants g_pc;

[[vk::binding(0, 0)]]
ByteAddressBuffer g_samples : register(t0);

struct VSOutput
{
    float4 position : SV_Position;
    float elevation : TEXCOORD0;
    float4 biome0 : TEXCOORD1;
    float4 biome1 : TEXCOORD2;
    float3 terrainNormal : TEXCOORD3;
    float3 surfaceDirection : TEXCOORD4;
    float waterDepth : TEXCOORD5;
    float3 localPosition : TEXCOORD6;
    float spacingMeters : TEXCOORD7;
    float3 worldPosition : TEXCOORD8;
    float3 bodyFixedNormal : TEXCOORD9;
    float3 bodyFixedSurfaceDirection : TEXCOORD10;
    float drySurface : TEXCOORD11;
    float horizonClip : SV_ClipDistance0;
};

float4 UnpackUnorm4x8(uint packed)
{
    return float4(
        (packed & 0xFFu),
        ((packed >> 8u) & 0xFFu),
        ((packed >> 16u) & 0xFFu),
        ((packed >> 24u) & 0xFFu)) / 255.0;
}

uint PhysicalSampleIndex(
    uint logicalX,
    uint logicalY,
    uint resolution,
    uint originX,
    uint originY)
{
    const uint physicalX = (logicalX + originX) % resolution;
    const uint physicalY = (logicalY + originY) % resolution;
    return physicalY * resolution + physicalX;
}

float2 LoadFineSlope(
    uint logicalX,
    uint logicalY,
    uint resolution,
    uint originX,
    uint originY)
{
    const uint sampleIndex = PhysicalSampleIndex(
        logicalX, logicalY, resolution, originX, originY);
    const uint address = sampleIndex * 32u;
    return asfloat(g_samples.Load2(address + 24u));
}

float3 SurfaceDirectionForOffsetFromBasis(
    float2 offsetMeters,
    float planetRadius,
    float3 up,
    float3 east,
    float3 north)
{
    float3 direction = up;
    const float distanceMeters = length(offsetMeters);
    if (distanceMeters > 0.0001)
    {
        const float3 tangentDirection = normalize(
            east * offsetMeters.x + north * offsetMeters.y);
        const float angle = distanceMeters / planetRadius;
        direction = normalize(
            up * cos(angle) + tangentDirection * sin(angle));
    }
    return direction;
}

float3 SurfaceDirectionForOffset(float2 offsetMeters, float planetRadius)
{
    return SurfaceDirectionForOffsetFromBasis(
        offsetMeters,
        planetRadius,
        g_pc.g_centerUpAndOriginX.xyz,
        g_pc.g_centerEastAndOriginY.xyz,
        g_pc.g_centerNorthAndMorphStart.xyz);
}

VSOutput main(uint vertexId : SV_VertexID)
{
    const float planetRadius = g_pc.g_planet.x;
    const float observerRadius = g_pc.g_planet.y;
    const float spacing = g_pc.g_planet.z;
    const uint resolution = (uint)round(g_pc.g_planet.w);
    const uint cellsPerAxis = resolution - 1u;
    const uint cellIndex = vertexId / 6u;
    const uint cornerIndex = vertexId % 6u;
    const uint cellX = cellIndex % cellsPerAxis;
    const uint cellY = cellIndex / cellsPerAxis;

    uint2 cornerOffset = uint2(0u, 0u);
    if (cornerIndex == 0u) cornerOffset = uint2(0u, 0u);
    else if (cornerIndex == 1u) cornerOffset = uint2(0u, 1u);
    else if (cornerIndex == 2u) cornerOffset = uint2(1u, 0u);
    else if (cornerIndex == 3u) cornerOffset = uint2(1u, 0u);
    else if (cornerIndex == 4u) cornerOffset = uint2(0u, 1u);
    else cornerOffset = uint2(1u, 1u);

    const uint logicalX = cellX + cornerOffset.x;
    const uint logicalY = cellY + cornerOffset.y;
    const uint originX = (uint)round(g_pc.g_centerUpAndOriginX.w);
    const uint originY = (uint)round(g_pc.g_centerEastAndOriginY.w);
    const uint physicalIndex = PhysicalSampleIndex(
        logicalX, logicalY, resolution, originX, originY);
    const uint sampleByteOffset = physicalIndex * 32u;

    const float bedElevation = asfloat(g_samples.Load(sampleByteOffset));
    const float waterDepth = asfloat(g_samples.Load(sampleByteOffset + 20u));
    // The clipmap draws the true bed. Standing water is a separate object (the
    // water pass), so neither the mesh nor the terrain shading knows about it.
    const float elevation = bedElevation;
    const float2 morphTargetOffset = asfloat(g_samples.Load2(sampleByteOffset + 4u));
    const uint packedBiome0 = g_samples.Load(sampleByteOffset + 12u);
    const uint packedBiome1 = g_samples.Load(sampleByteOffset + 16u);

    const float halfCells = ((float)resolution - 1.0) * 0.5;
    float2 localOffsetMeters =
        (float2((float)logicalX, (float)logicalY) - halfCells) * spacing;
    float2 offsetMeters = g_pc.g_centerOffsetMeters.xy + localOffsetMeters;

    const float morphStart = g_pc.g_centerNorthAndMorphStart.w;
    const float morphEnd = g_pc.g_morph.x;
    const float hasCoarser = g_pc.g_morph.z;
    if (hasCoarser > 0.5)
    {
        const float edgeDistance = max(
            abs(localOffsetMeters.x), abs(localOffsetMeters.y));
        const float normalized = saturate(
            (edgeDistance - morphStart) / max(morphEnd - morphStart, 0.0001));
        const float morph = normalized * normalized * (3.0 - 2.0 * normalized);
        offsetMeters = lerp(offsetMeters, morphTargetOffset, morph);
    }

    const float innerHoleHalfExtentMeters = g_pc.g_morph.w;
    const float2 innerHoleCenterOffsetMeters = float2(
        g_pc.g_morph.y, g_pc.g_debug.w);
    const float3 surfaceDirection = SurfaceDirectionForOffset(
        offsetMeters, planetRadius);
    const float displacedElevation = elevation;
    const float displacedRadius = planetRadius + displacedElevation;

    const float sinSquaredFromObserver = saturate(
        surfaceDirection.x * surfaceDirection.x +
        surfaceDirection.z * surfaceDirection.z);
    const float positiveCosine = sqrt(max(1.0 - sinSquaredFromObserver, 0.0));
    const float cosineMinusOne = surfaceDirection.y >= 0.0
        ? -sinSquaredFromObserver / max(1.0 + positiveCosine, 0.000001)
        : surfaceDirection.y - 1.0;
    const float observerAltitude = observerRadius - planetRadius;
    const float3 localPosition = float3(
        surfaceDirection.x * displacedRadius,
        planetRadius * cosineMinusOne +
            displacedElevation * surfaceDirection.y - observerAltitude,
        surfaceDirection.z * displacedRadius);

    const float2 fineSlope = LoadFineSlope(
        logicalX, logicalY, resolution, originX, originY);
    const float slopeEast = fineSlope.x;
    const float slopeNorth = fineSlope.y;

    float3 tangentEast = g_pc.g_centerEastAndOriginY.xyz -
        surfaceDirection * dot(g_pc.g_centerEastAndOriginY.xyz, surfaceDirection);
    const float tangentEastLength = length(tangentEast);
    if (tangentEastLength > 0.0001)
        tangentEast /= tangentEastLength;
    else
        tangentEast = float3(1.0, 0.0, 0.0);

    float3 tangentNorth = g_pc.g_centerNorthAndMorphStart.xyz -
        surfaceDirection * dot(g_pc.g_centerNorthAndMorphStart.xyz, surfaceDirection);
    tangentNorth -= tangentEast * dot(tangentNorth, tangentEast);
    const float tangentNorthLength = length(tangentNorth);
    if (tangentNorthLength > 0.0001)
        tangentNorth /= tangentNorthLength;
    else
        tangentNorth = float3(0.0, 0.0, 1.0);

    const float3 terrainNormal = normalize(
        surfaceDirection - tangentEast * slopeEast - tangentNorth * slopeNorth);

    VSOutput output;
    output.position = mul(float4(localPosition, 1.0), g_pc.g_mvp);
    output.elevation = elevation;
    output.waterDepth = 0.0;
    output.localPosition = localPosition;
    output.spacingMeters = 0.0;
    output.worldPosition = float3(0.0, 0.0, 0.0);
    output.biome0 = UnpackUnorm4x8(packedBiome0);
    output.biome1 = UnpackUnorm4x8(packedBiome1);

    if (g_pc.g_debug.y > 0.5)
    {
        const uint colorIndex = ((uint)round(g_pc.g_debug.x)) % 8u;
        output.biome0 = float4(
            colorIndex == 0u ? 1.0 : 0.0,
            colorIndex == 1u ? 1.0 : 0.0,
            colorIndex == 2u ? 1.0 : 0.0,
            colorIndex == 3u ? 1.0 : 0.0);
        output.biome1 = float4(
            colorIndex == 4u ? 1.0 : 0.0,
            colorIndex == 5u ? 1.0 : 0.0,
            colorIndex == 6u ? 1.0 : 0.0,
            colorIndex == 7u ? 1.0 : 0.0);
    }

    output.terrainNormal = terrainNormal;
    output.surfaceDirection = surfaceDirection;
    output.bodyFixedNormal = normalize(
        g_pc.g_observerEastBody.xyz * terrainNormal.x +
        g_pc.g_observerUpBody.xyz * terrainNormal.y +
        g_pc.g_observerNorthBody.xyz * terrainNormal.z);
    output.bodyFixedSurfaceDirection = normalize(
        g_pc.g_observerEastBody.xyz * surfaceDirection.x +
        g_pc.g_observerUpBody.xyz * surfaceDirection.y +
        g_pc.g_observerNorthBody.xyz * surfaceDirection.z);
    output.drySurface = g_pc.g_centerOffsetMeters.z;

    const float horizonCosine = saturate(planetRadius / observerRadius);
    const float positiveReliefPadding = max(elevation, 0.0) / planetRadius;
    output.horizonClip = surfaceDirection.y - horizonCosine +
        0.000002 + positiveReliefPadding;

    if (g_pc.g_debug.z > 0.5 && localPosition.x < 0.0)
        output.horizonClip = -1.0;

    const float cellCenterX =
        ((float)cellX + 0.5 - (float)cellsPerAxis * 0.5) * spacing;
    const float cellCenterY =
        ((float)cellY + 0.5 - (float)cellsPerAxis * 0.5) * spacing;
    const float halfCell = spacing * 0.5;
    const bool insideHole =
        innerHoleHalfExtentMeters > 0.0 &&
        abs(cellCenterX - innerHoleCenterOffsetMeters.x) + halfCell <=
            innerHoleHalfExtentMeters &&
        abs(cellCenterY - innerHoleCenterOffsetMeters.y) + halfCell <=
            innerHoleHalfExtentMeters;
    if (insideHole)
        output.horizonClip = -1.0;

    return output;
}
)";
} // namespace orbit::terrain_render::detail
