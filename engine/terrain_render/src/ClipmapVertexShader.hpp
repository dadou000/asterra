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
    // Level coverage. The pixel shader keeps a pixel when y <= noise < x, where
    // noise is a per-pixel value in [0, 1). Neighbouring levels use the same
    // noise with complementary x/y, so every pixel belongs to exactly one level.
    float2 lodFade : TEXCOORD13;
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
        // The hardware sin/cos are only good to about 3e-7 absolute, which times
        // the planet radius is metres: vertices of a fine level snapped onto
        // concentric rings. Small angles use the series instead (error below
        // 1e-11 relative up to 0.2 rad).
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
        direction = normalize(
            up * cosineAngle + tangentDirection * sineAngle);
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

)" R"(
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
        uint colorIndex = ((uint)round(g_pc.g_debug.x)) % 8u;
        if (g_pc.g_debug.y > 1.5 && g_pc.g_debug.y < 2.5)
        {
            // Sample health: which vertex data is bad. Material 0 = elevation not finite or beyond
            // +-20 km, 1 = morph target not finite or more than 4 cells from the vertex, 2 = fine slope
            // not finite or steeper than 20, 7 = healthy. Rows of one colour are a corrupted strip.
            const float2 unmorphedOffset = g_pc.g_centerOffsetMeters.xy + localOffsetMeters;
            const float2 morphDelta = morphTargetOffset - unmorphedOffset;
            const bool badElevation = !isfinite(bedElevation) || abs(bedElevation) > 20000.0;
            const bool badMorph = !isfinite(morphDelta.x) || !isfinite(morphDelta.y) ||
                max(abs(morphDelta.x), abs(morphDelta.y)) > 4.0 * spacing;
            const bool badSlope = !isfinite(fineSlope.x) || !isfinite(fineSlope.y) ||
                abs(fineSlope.x) > 20.0 || abs(fineSlope.y) > 20.0;
            colorIndex = badElevation ? 0u : (badMorph ? 1u : (badSlope ? 2u : 7u));
        }
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
    // A level that is still fading in or out is dithered against the level
    // around it, so the hole only becomes a hard cut once the finer level is
    // fully drawn.
    const float zoneFraction = g_pc.g_observerNorthBody.w;
    if (zoneFraction < 0.0)
    {
        // Ladder: the level's time fade, and the finer level's fade over the hole.
        const float selfFade = g_pc.g_observerEastBody.w;
        const float finerFade = g_pc.g_observerUpBody.w;
        output.lodFade = float2(selfFade, 0.0);
        if (insideHole)
        {
            if (finerFade >= 0.999)
                output.horizonClip = -1.0;
            else
                output.lodFade.y = finerFade;
        }
    }
    else
    {
        // Distance bands (experimental): coverage is a function of this vertex's
        // distance from the camera. x rises across the level's inner edge, y
        // across its outer edge (a level with no inner edge has x = 1).
        const float innerEdge = g_pc.g_observerEastBody.w;
        const float outerEdge = g_pc.g_observerUpBody.w;
        const float distanceMeters = length(localPosition);
        // Triangles span about two cells: cull a vertex only when no pixel
        // near it can have any coverage.
        const float slack = 2.0 * spacing;
        const float lowestCoverage =
            outerEdge > 0.0
                ? smoothstep(
                    outerEdge * (1.0 - zoneFraction),
                    outerEdge * (1.0 + zoneFraction),
                    distanceMeters - slack)
                : 0.0;
        const float highestCoverage =
            innerEdge > 0.0
                ? smoothstep(
                    innerEdge * (1.0 - zoneFraction),
                    innerEdge * (1.0 + zoneFraction),
                    distanceMeters + slack)
                : 1.0;
        output.lodFade = float2(
            innerEdge > 0.0
                ? smoothstep(
                    innerEdge * (1.0 - zoneFraction),
                    innerEdge * (1.0 + zoneFraction),
                    distanceMeters)
                : 1.0,
            outerEdge > 0.0
                ? smoothstep(
                    outerEdge * (1.0 - zoneFraction),
                    outerEdge * (1.0 + zoneFraction),
                    distanceMeters)
                : 0.0);
        if (highestCoverage <= 0.0 || lowestCoverage >= 1.0)
            output.horizonClip = -1.0;
    }

    if (g_pc.g_debug.y > 2.5 && g_pc.g_debug.y < 3.5)
    {
        // Hole / fade view: nothing is culled, every vertex is coloured by why it would be.
        // Material 0 = beyond the horizon, 1 = inside a finer level's hole (culled once that level is
        // fully faded in), 2 = inside the hole while the finer level is still fading, 3 = culled by the
        // distance bands, 7 = drawn normally. A level drawn through its own hole shows overlap; long
        // runs of 1 with no finer level drawn there are the black gaps.
        const float horizonValue = surfaceDirection.y - horizonCosine + 0.000002 + positiveReliefPadding;
        const bool beyondHorizon = horizonValue < 0.0;
        const bool culledNow = output.horizonClip < 0.0;
        uint holeIndex = 7u;
        if (beyondHorizon)
            holeIndex = 0u;
        else if (culledNow && zoneFraction < 0.0)
            holeIndex = 1u;
        else if (culledNow)
            holeIndex = 3u;
        else if (insideHole && zoneFraction < 0.0 && output.lodFade.y > 0.0)
            holeIndex = 2u;
        output.biome0 = float4(
            holeIndex == 0u ? 1.0 : 0.0,
            holeIndex == 1u ? 1.0 : 0.0,
            holeIndex == 2u ? 1.0 : 0.0,
            holeIndex == 3u ? 1.0 : 0.0);
        output.biome1 = float4(
            holeIndex == 4u ? 1.0 : 0.0,
            holeIndex == 5u ? 1.0 : 0.0,
            holeIndex == 6u ? 1.0 : 0.0,
            holeIndex == 7u ? 1.0 : 0.0);
        output.horizonClip = 1.0;
        output.lodFade = float2(1.0, 0.0);
    }

)" R"(
    if (g_pc.g_debug.y > 3.5 && g_pc.g_debug.y < 4.5)
    {
        // Projected position view: nothing is culled; each vertex is coloured by where it lands.
        // Material 0 = non-finite clip position, 1 = behind the camera (w <= 0), 2 = in front of the near
        // plane or beyond the far plane, 3 = inside the depth range but outside the screen sideways,
        // 7 = on screen. Triangles that straddle a 1 or 2 vertex are the ones the GPU clips away.
        const float4 clip = output.position;
        const bool finite = isfinite(clip.x) && isfinite(clip.y) && isfinite(clip.z) && isfinite(clip.w);
        uint projectedIndex = 7u;
        if (!finite)
            projectedIndex = 0u;
        else if (clip.w <= 0.0)
            projectedIndex = 1u;
        else if (clip.z < 0.0 || clip.z > clip.w)
            projectedIndex = 2u;
        else if (abs(clip.x) > clip.w || abs(clip.y) > clip.w)
            projectedIndex = 3u;
        output.biome0 = float4(
            projectedIndex == 0u ? 1.0 : 0.0,
            projectedIndex == 1u ? 1.0 : 0.0,
            projectedIndex == 2u ? 1.0 : 0.0,
            projectedIndex == 3u ? 1.0 : 0.0);
        output.biome1 = float4(0.0, 0.0, 0.0, projectedIndex == 7u ? 1.0 : 0.0);
        output.horizonClip = 1.0;
        output.lodFade = float2(1.0, 0.0);
    }

    if (g_pc.g_debug.y > 4.5)
    {
        // Shading view: nothing culled; the pixel shader reports bad interpolated inputs (it reads this
        // flag from waterDepth, which the clipmap never sets otherwise).
        output.waterDepth = 5.0;
        output.horizonClip = 1.0;
        output.lodFade = float2(1.0, 0.0);
    }

    return output;
}
)";
} // namespace orbit::terrain_render::detail
