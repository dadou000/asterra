#pragma once

namespace orbit::terrain_gpu::detail
{
inline constexpr const char* kM22BiomeScatterShader = R"(
struct ScatterConstants
{
    uint resolution;
    float cellSizeMeters;
    float minimumSpacingMeters;
    float densityPerSquareMeter;

    float biomeDensityMultiplier;
    uint kind;
    uint pageHash;
    uint ruleHash;

    uint compatibilityMask;
    uint requiresSoil;
    float minimumSoilDepthMeters;
    float minimumSlopeDegrees;

    float maximumSlopeDegrees;
    float slopeFalloffDegrees;
    float minimumMoisture;
    float maximumMoisture;

    float moistureFalloff;
    float minimumScale;
    float maximumScale;
};
[[vk::push_constant]] ScatterConstants g_pc;

[[vk::binding(0, 0)]]
RWByteAddressBuffer g_fields : register(u0);

[[vk::binding(1, 0)]]
RWByteAddressBuffer g_instances : register(u1);

uint Hash32(uint value)
{
    value ^= value >> 16u;
    value *= 0x7FEB352Du;
    value ^= value >> 15u;
    value *= 0x846CA68Bu;
    value ^= value >> 16u;
    return value;
}

float UnitRandom(uint value)
{
    return (float)(value >> 8u) * (1.0 / 16777216.0);
}

float SmoothStep01(float value)
{
    const float t = saturate(value);
    return t * t * (3.0 - 2.0 * t);
}

float RangeMask(
    float value,
    float minimum,
    float maximum,
    float falloff)
{
    if (value >= minimum && value <= maximum)
    {
        return 1.0;
    }

    if (falloff <= 0.0)
    {
        return 0.0;
    }

    if (value < minimum)
    {
        if (value <= minimum - falloff)
        {
            return 0.0;
        }

        return SmoothStep01(
            (value - (minimum - falloff)) /
            falloff);
    }

    if (value >= maximum + falloff)
    {
        return 0.0;
    }

    return 1.0 -
        SmoothStep01(
            (value - maximum) /
            falloff);
}

uint CellAddress(uint x, uint y)
{
    return (y * g_pc.resolution + x) * 32u;
}

float LoadFieldFloat(uint address, uint offset)
{
    return asfloat(g_fields.Load(address + offset));
}

uint LoadExposedMaterial(uint address)
{
    return g_fields.Load(address + 24u);
}

uint CandidateBaseHash(uint x, uint y)
{
    return Hash32(
        g_pc.pageHash ^
        g_pc.ruleHash ^
        Hash32(x * 0x9E3779B9u) ^
        Hash32(y * 0x85EBCA6Bu));
}

float2 CandidatePosition(
    uint x,
    uint y,
    uint baseHash)
{
    const float jitterX =
        UnitRandom(
            Hash32(
                baseHash ^
                0xA341316Cu));

    const float jitterY =
        UnitRandom(
            Hash32(
                baseHash ^
                0xC8013EA4u));

    const float halfGrid =
        (float)g_pc.resolution *
        0.5;

    return float2(
        ((float)x + jitterX - halfGrid) *
            g_pc.cellSizeMeters,
        ((float)y + jitterY - halfGrid) *
            g_pc.cellSizeMeters);
}

float CandidateProbability(uint x, uint y)
{
    const uint address =
        CellAddress(x, y);

    const float biomeWeight =
        LoadFieldFloat(address, 0u);

    const float slopeDegrees =
        LoadFieldFloat(address, 4u);

    const float soilDepthMeters =
        LoadFieldFloat(address, 8u);

    const float moisture =
        LoadFieldFloat(address, 12u);

    const float exclusionMask =
        LoadFieldFloat(address, 16u);

    const float authoredDensity =
        LoadFieldFloat(address, 20u);

    const uint exposedMaterial =
        LoadExposedMaterial(address);

    const uint exposedBit =
        1u << min(exposedMaterial, 31u);

    if ((g_pc.compatibilityMask &
         exposedBit) == 0u)
    {
        return 0.0;
    }

    // ExposedSurfaceKind::Soil == 2.
    if (g_pc.requiresSoil != 0u &&
        (exposedMaterial != 2u ||
         soilDepthMeters <
             g_pc.minimumSoilDepthMeters))
    {
        return 0.0;
    }

    const float slope =
        RangeMask(
            slopeDegrees,
            g_pc.minimumSlopeDegrees,
            g_pc.maximumSlopeDegrees,
            g_pc.slopeFalloffDegrees);

    const float moistureMask =
        RangeMask(
            moisture,
            g_pc.minimumMoisture,
            g_pc.maximumMoisture,
            g_pc.moistureFalloff);

    const float exclusion =
        1.0 -
        saturate(exclusionMask);

    const float cellArea =
        g_pc.cellSizeMeters *
        g_pc.cellSizeMeters;

    return saturate(
        g_pc.densityPerSquareMeter *
        cellArea *
        g_pc.biomeDensityMultiplier *
        saturate(biomeWeight) *
        max(authoredDensity, 0.0) *
        slope *
        moistureMask *
        exclusion);
}

bool DensityAccepted(
    uint x,
    uint y,
    uint baseHash)
{
    const float draw =
        UnitRandom(
            Hash32(
                baseHash ^
                0x7E95761Eu));

    return
        draw <
        CandidateProbability(x, y);
}

bool LowerPriorityConflict(
    uint selfX,
    uint selfY,
    float2 selfPosition,
    uint selfPriority)
{
    const float minimumSpacingSquared =
        g_pc.minimumSpacingMeters *
        g_pc.minimumSpacingMeters;

    [unroll]
    for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx)
        {
            if (dx == 0 && dy == 0)
            {
                continue;
            }

            const int nx =
                (int)selfX +
                dx;

            const int ny =
                (int)selfY +
                dy;

            if (nx < 0 ||
                ny < 0 ||
                nx >=
                    (int)g_pc.resolution ||
                ny >=
                    (int)g_pc.resolution)
            {
                continue;
            }

            const uint ux =
                (uint)nx;

            const uint uy =
                (uint)ny;

            const uint neighborBase =
                CandidateBaseHash(
                    ux,
                    uy);

            if (!DensityAccepted(
                    ux,
                    uy,
                    neighborBase))
            {
                continue;
            }

            const float2 neighborPosition =
                CandidatePosition(
                    ux,
                    uy,
                    neighborBase);

            const float2 delta =
                neighborPosition -
                selfPosition;

            const float distanceSquared =
                dot(delta, delta);

            if (distanceSquared >=
                minimumSpacingSquared)
            {
                continue;
            }

            const uint neighborPriority =
                Hash32(
                    neighborBase ^
                    0xAD90777Du);

            if (neighborPriority <
                    selfPriority ||
                (neighborPriority ==
                     selfPriority &&
                 (uy <
                      selfY ||
                  (uy ==
                       selfY &&
                   ux <
                       selfX))))
            {
                return true;
            }
        }
    }

    return false;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
    const uint x = dispatchId.x;
    const uint y = dispatchId.y;

    if (x >= g_pc.resolution ||
        y >= g_pc.resolution)
    {
        return;
    }

    const uint cell =
        y * g_pc.resolution +
        x;

    const uint outputAddress =
        cell * 48u;

    const uint baseHash =
        CandidateBaseHash(
            x,
            y);

    const float2 position =
        CandidatePosition(
            x,
            y,
            baseHash);

    const uint priority =
        Hash32(
            baseHash ^
            0xAD90777Du);

    bool active =
        DensityAccepted(
            x,
            y,
            baseHash);

    if (active)
    {
        active =
            !LowerPriorityConflict(
                x,
                y,
                position,
                priority);
    }

    const float yaw =
        UnitRandom(
            Hash32(
                baseHash ^
                0xB8E1AFEDu)) *
        6.283185307179586;

    const float scaleT =
        UnitRandom(
            Hash32(
                baseHash ^
                0x6C8E9CF5u));

    const float scale =
        lerp(
            g_pc.minimumScale,
            g_pc.maximumScale,
            scaleT);

    const uint idHighLow =
        Hash32(
            baseHash ^
            0xD3A2646Cu);

    const uint idHighHigh =
        baseHash;

    const uint idLowLow =
        Hash32(
            baseHash ^
            0xB55A4F09u);

    const uint idLowHigh =
        Hash32(
            baseHash ^
            0xFD7046C5u);

    g_instances.Store(
        outputAddress + 0u,
        asuint(position.x));

    g_instances.Store(
        outputAddress + 4u,
        asuint(position.y));

    g_instances.Store(
        outputAddress + 8u,
        asuint(yaw));

    g_instances.Store(
        outputAddress + 12u,
        asuint(scale));

    g_instances.Store(
        outputAddress + 16u,
        idHighLow);

    g_instances.Store(
        outputAddress + 20u,
        idHighHigh);

    g_instances.Store(
        outputAddress + 24u,
        idLowLow);

    g_instances.Store(
        outputAddress + 28u,
        idLowHigh);

    g_instances.Store(
        outputAddress + 32u,
        g_pc.kind);

    g_instances.Store(
        outputAddress + 36u,
        active ? 1u : 0u);

    g_instances.Store(
        outputAddress + 40u,
        x);

    g_instances.Store(
        outputAddress + 44u,
        y);
}
)";
} // namespace orbit::terrain_gpu::detail
