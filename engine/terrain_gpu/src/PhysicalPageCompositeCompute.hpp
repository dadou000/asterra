#pragma once

namespace orbit::terrain_gpu::detail
{
inline constexpr const char* kPhysicalPageCompositeShader = R"(
[[vk::binding(0, 0)]]
ByteAddressBuffer g_physicalPage : register(t0);

[[vk::binding(1, 0)]]
RWByteAddressBuffer g_samples : register(u1);

struct PushConstants
{
    float4 fineUpCenterX;
    float4 fineEastCenterY;
    float4 fineNorthRadius;
    float4 pageBounds;

    uint4 grid;
    uint4 region;
    uint4 tile;

    float4 morph;
};

[[vk::push_constant]] PushConstants g_pc;

static const uint kSampleStrideBytes = 32u;
static const uint kPhysicalTexelStrideBytes = 8u;

uint WrapIndex(int value, uint size)
{
    int m = value % int(size);
    if (m < 0) m += int(size);
    return uint(m);
}

float3 DirectionAtSurfaceOffset(
    float3 up,
    float3 east,
    float3 north,
    float2 offsetMeters,
    float radiusMeters)
{
    float3 tangent =
        east * offsetMeters.x +
        north * offsetMeters.y;

    float distanceMeters =
        length(tangent);

    if (distanceMeters <= 0.000001 ||
        radiusMeters <= 0.0)
    {
        return normalize(up);
    }

    float3 tangentDirection =
        tangent / distanceMeters;

    float angle =
        distanceMeters / radiusMeters;

    return normalize(
        up * cos(angle) +
        tangentDirection * sin(angle));
}

void DirectionToCube(
    float3 direction,
    out uint face,
    out float2 uv)
{
    float3 unit =
        normalize(direction);

    float ax = abs(unit.x);
    float ay = abs(unit.y);
    float az = abs(unit.z);

    if (ax >= ay && ax >= az)
    {
        if (unit.x >= 0.0)
        {
            face = 0u;
            uv = float2(-unit.z / ax, unit.y / ax);
        }
        else
        {
            face = 1u;
            uv = float2(unit.z / ax, unit.y / ax);
        }
    }
    else if (ay >= ax && ay >= az)
    {
        if (unit.y >= 0.0)
        {
            face = 2u;
            uv = float2(unit.x / ay, -unit.z / ay);
        }
        else
        {
            face = 3u;
            uv = float2(unit.x / ay, unit.z / ay);
        }
    }
    else
    {
        if (unit.z >= 0.0)
        {
            face = 4u;
            uv = float2(unit.x / az, unit.y / az);
        }
        else
        {
            face = 5u;
            uv = float2(-unit.x / az, unit.y / az);
        }
    }

    uv = clamp(
        uv,
        float2(-1.0, -1.0),
        float2(1.0, 1.0));
}

uint CoordinateToTileIndex(
    float coordinate,
    uint count)
{
    float normalized =
        clamp(
            coordinate * 0.5 + 0.5,
            0.0,
            1.0);

    if (normalized >= 1.0)
    {
        return count - 1u;
    }

    return uint(
        normalized *
        float(count));
}

bool BelongsToPage(
    uint face,
    float2 uv)
{
    if (face != g_pc.tile.x)
    {
        return false;
    }

    uint level =
        min(g_pc.tile.y, 30u);

    uint count =
        1u << level;

    return
        CoordinateToTileIndex(
            uv.x,
            count) == g_pc.tile.z &&
        CoordinateToTileIndex(
            uv.y,
            count) == g_pc.tile.w;
}

float2 LoadPhysicalTexel(
    uint x,
    uint y)
{
    uint resolution =
        g_pc.grid.w;

    uint index =
        y * resolution + x;

    return asfloat(
        g_physicalPage.Load2(
            index *
            kPhysicalTexelStrideBytes));
}

float2 SamplePhysicalPage(
    float2 uv)
{
    float2 extent =
        max(
            g_pc.pageBounds.zw -
                g_pc.pageBounds.xy,
            float2(
                0.0000001,
                0.0000001));

    float2 normalized =
        saturate(
            (uv -
             g_pc.pageBounds.xy) /
            extent);

    float resolutionMinusOne =
        float(
            max(
                g_pc.grid.w - 1u,
                1u));

    float2 coordinate =
        normalized *
        resolutionMinusOne;

    uint2 p0 =
        uint2(floor(coordinate));

    uint2 p1 =
        min(
            p0 + uint2(1u, 1u),
            uint2(
                g_pc.grid.w - 1u,
                g_pc.grid.w - 1u));

    float2 fraction =
        coordinate -
        float2(p0);

    float2 a =
        lerp(
            LoadPhysicalTexel(
                p0.x,
                p0.y),
            LoadPhysicalTexel(
                p1.x,
                p0.y),
            fraction.x);

    float2 b =
        lerp(
            LoadPhysicalTexel(
                p0.x,
                p1.y),
            LoadPhysicalTexel(
                p1.x,
                p1.y),
            fraction.x);

    return lerp(
        a,
        b,
        fraction.y);
}

[numthreads(8, 8, 1)]
void main(
    uint3 dispatchId :
        SV_DispatchThreadID)
{
    if (dispatchId.x >= g_pc.region.z ||
        dispatchId.y >= g_pc.region.w)
    {
        return;
    }

    uint physicalX =
        g_pc.region.x +
        dispatchId.x;

    uint physicalY =
        g_pc.region.y +
        dispatchId.y;

    uint resolution =
        g_pc.grid.x;

    uint logicalX =
        WrapIndex(
            int(physicalX) -
                int(g_pc.grid.y),
            resolution);

    uint logicalY =
        WrapIndex(
            int(physicalY) -
                int(g_pc.grid.z),
            resolution);

    float halfCells =
        (float(resolution) - 1.0) * 0.5;

    float2 localOffsetMeters =
        (float2(
             float(logicalX),
             float(logicalY)) -
         halfCells) *
        g_pc.morph.x;

    float2 offsetMeters =
        float2(
            g_pc.fineUpCenterX.w,
            g_pc.fineEastCenterY.w) +
        localOffsetMeters;

    float coarseSpacing =
        g_pc.morph.y;

    if (coarseSpacing > 0.0 &&
        g_pc.morph.w > g_pc.morph.z)
    {
        float2 coarseCoordinate =
            offsetMeters / coarseSpacing;

        float2 snappedCoarseCoordinate =
            sign(coarseCoordinate) *
            floor(
                abs(coarseCoordinate) +
                0.5);

        float2 snappedCoarseOffset =
            snappedCoarseCoordinate *
            coarseSpacing;

        float edgeDistance =
            max(
                abs(localOffsetMeters.x),
                abs(localOffsetMeters.y));

        float normalizedMorph =
            saturate(
                (edgeDistance -
                 g_pc.morph.z) /
                max(
                    g_pc.morph.w -
                        g_pc.morph.z,
                    0.0001));

        float morph =
            normalizedMorph *
            normalizedMorph *
            (3.0 -
             2.0 * normalizedMorph);

        offsetMeters =
            lerp(
                offsetMeters,
                snappedCoarseOffset,
                morph);
    }

    float3 direction =
        DirectionAtSurfaceOffset(
            g_pc.fineUpCenterX.xyz,
            g_pc.fineEastCenterY.xyz,
            g_pc.fineNorthRadius.xyz,
            offsetMeters,
            g_pc.fineNorthRadius.w);

    uint face;
    float2 uv;
    DirectionToCube(
        direction,
        face,
        uv);

    if (!BelongsToPage(
            face,
            uv))
    {
        return;
    }

    float2 physical =
        SamplePhysicalPage(uv);

    uint physicalIndex =
        physicalY *
            resolution +
        physicalX;

    uint byteOffset =
        physicalIndex *
        kSampleStrideBytes;

    g_samples.Store(
        byteOffset + 0u,
        asuint(physical.x));

    g_samples.Store(
        byteOffset + 20u,
        asuint(physical.y));
}
)";
} // namespace orbit::terrain_gpu::detail
