#pragma once

namespace orbit::terrain_gpu::detail
{
inline constexpr const char* kM11InitializeStateShader = R"(
[[vk::binding(0, 0)]]
RWByteAddressBuffer g_water : register(u0);
[[vk::binding(1, 0)]]
RWByteAddressBuffer g_flux : register(u1);
[[vk::binding(2, 0)]]
RWByteAddressBuffer g_velocity : register(u2);
[[vk::binding(3, 0)]]
RWByteAddressBuffer g_sediment : register(u3);
[[vk::binding(4, 0)]]
RWByteAddressBuffer g_sedimentLocal : register(u4);

struct PushConstants
{
    uint resolution;
    float initialWaterDepthMeters;
    float initialSedimentKgPerSquareMeter;
};

[[vk::push_constant]] PushConstants g_pc;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_pc.resolution || id.y >= g_pc.resolution)
    {
        return;
    }

    const uint index = id.y * g_pc.resolution + id.x;

    g_water.Store(
        index * 4u,
        asuint(max(g_pc.initialWaterDepthMeters, 0.0)));

    const uint fluxOffset = index * 16u;
    g_flux.Store(fluxOffset + 0u, asuint(0.0));
    g_flux.Store(fluxOffset + 4u, asuint(0.0));
    g_flux.Store(fluxOffset + 8u, asuint(0.0));
    g_flux.Store(fluxOffset + 12u, asuint(0.0));

    const uint velocityOffset = index * 8u;
    g_velocity.Store(velocityOffset + 0u, asuint(0.0));
    g_velocity.Store(velocityOffset + 4u, asuint(0.0));

    const float sediment =
        max(g_pc.initialSedimentKgPerSquareMeter, 0.0);

    g_sediment.Store(index * 4u, asuint(sediment));
    g_sedimentLocal.Store(index * 4u, asuint(sediment));
}
)";

inline constexpr const char* kM11RainFluxShader = R"(
[[vk::binding(0, 0)]]
ByteAddressBuffer g_waterIn : register(t0);
[[vk::binding(1, 0)]]
ByteAddressBuffer g_fluxIn : register(t1);
[[vk::binding(2, 0)]]
ByteAddressBuffer g_rainfallRate : register(t2);
[[vk::binding(3, 0)]]
RWByteAddressBuffer g_waterRainOut : register(u3);
[[vk::binding(4, 0)]]
RWByteAddressBuffer g_fluxOut : register(u4);

[[vk::binding(5, 0)]]
RWTexture2D<float> g_bedrock : register(u5);
[[vk::binding(6, 0)]]
RWTexture2D<float4> g_loose : register(u6);

struct PushConstants
{
    uint resolution;
    float spacingMeters;
    float timeStepSeconds;
    float uniformRainMetersPerSecond;
    float gravityMetersPerSecondSquared;
    float pipeCrossSectionSquareMeters;
};

[[vk::push_constant]] PushConstants g_pc;

float Surface(uint2 p)
{
    const float4 loose = g_loose[p];
    return g_bedrock[p] + loose.x + loose.y + loose.z + loose.w;
}

float WaterAfterRain(uint2 p)
{
    const uint index = p.y * g_pc.resolution + p.x;
    const float water = max(asfloat(g_waterIn.Load(index * 4u)), 0.0);
    const float localRain =
        max(asfloat(g_rainfallRate.Load(index * 4u)), 0.0);

    return water +
        (g_pc.uniformRainMetersPerSecond + localRain) *
        g_pc.timeStepSeconds;
}

float UpdatedFlux(
    float oldFlux,
    float sourceHead,
    int2 neighbor)
{
    if (neighbor.x < 0 || neighbor.y < 0 ||
        neighbor.x >= int(g_pc.resolution) ||
        neighbor.y >= int(g_pc.resolution))
    {
        return 0.0;
    }

    const uint2 q = uint2(neighbor);
    const float targetHead = Surface(q) + WaterAfterRain(q);
    const float dh = sourceHead - targetHead;

    return max(
        0.0,
        oldFlux +
            g_pc.timeStepSeconds *
            g_pc.pipeCrossSectionSquareMeters *
            g_pc.gravityMetersPerSecondSquared *
            dh /
            max(g_pc.spacingMeters, 1.0e-6));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_pc.resolution || id.y >= g_pc.resolution)
    {
        return;
    }

    const uint2 p = id.xy;
    const uint index = id.y * g_pc.resolution + id.x;
    const uint fluxOffset = index * 16u;

    const float water = WaterAfterRain(p);
    g_waterRainOut.Store(index * 4u, asuint(water));

    const float sourceHead = Surface(p) + water;

    float4 flux = float4(
        asfloat(g_fluxIn.Load(fluxOffset + 0u)),
        asfloat(g_fluxIn.Load(fluxOffset + 4u)),
        asfloat(g_fluxIn.Load(fluxOffset + 8u)),
        asfloat(g_fluxIn.Load(fluxOffset + 12u)));

    flux.x = UpdatedFlux(flux.x, sourceHead, int2(p) + int2(-1, 0));
    flux.y = UpdatedFlux(flux.y, sourceHead, int2(p) + int2(1, 0));
    flux.z = UpdatedFlux(flux.z, sourceHead, int2(p) + int2(0, -1));
    flux.w = UpdatedFlux(flux.w, sourceHead, int2(p) + int2(0, 1));

    const float totalFlux = flux.x + flux.y + flux.z + flux.w;

    if (totalFlux > 0.0)
    {
        const float cellArea =
            g_pc.spacingMeters * g_pc.spacingMeters;

        const float volume = water * cellArea;

        const float scale = min(
            1.0,
            volume /
                max(
                    totalFlux * g_pc.timeStepSeconds,
                    1.0e-12));

        flux *= scale;
    }

    g_fluxOut.Store(fluxOffset + 0u, asuint(flux.x));
    g_fluxOut.Store(fluxOffset + 4u, asuint(flux.y));
    g_fluxOut.Store(fluxOffset + 8u, asuint(flux.z));
    g_fluxOut.Store(fluxOffset + 12u, asuint(flux.w));
}
)";

inline constexpr const char* kM11WaterVelocityShader = R"(
[[vk::binding(0, 0)]]
ByteAddressBuffer g_waterRain : register(t0);
[[vk::binding(1, 0)]]
ByteAddressBuffer g_flux : register(t1);
[[vk::binding(2, 0)]]
RWByteAddressBuffer g_waterOut : register(u2);
[[vk::binding(3, 0)]]
RWByteAddressBuffer g_velocityOut : register(u3);

struct PushConstants
{
    uint resolution;
    float spacingMeters;
    float timeStepSeconds;
};

[[vk::push_constant]] PushConstants g_pc;

float4 FluxAt(uint2 p)
{
    const uint index = p.y * g_pc.resolution + p.x;
    const uint o = index * 16u;

    return float4(
        asfloat(g_flux.Load(o + 0u)),
        asfloat(g_flux.Load(o + 4u)),
        asfloat(g_flux.Load(o + 8u)),
        asfloat(g_flux.Load(o + 12u)));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_pc.resolution || id.y >= g_pc.resolution)
    {
        return;
    }

    const uint2 p = id.xy;
    const uint index = id.y * g_pc.resolution + id.x;

    const float4 own = FluxAt(p);

    float inWest = 0.0;
    float inEast = 0.0;
    float inNorth = 0.0;
    float inSouth = 0.0;

    if (id.x > 0u)
    {
        inWest = FluxAt(uint2(id.x - 1u, id.y)).y;
    }

    if (id.x + 1u < g_pc.resolution)
    {
        inEast = FluxAt(uint2(id.x + 1u, id.y)).x;
    }

    if (id.y > 0u)
    {
        inNorth = FluxAt(uint2(id.x, id.y - 1u)).w;
    }

    if (id.y + 1u < g_pc.resolution)
    {
        inSouth = FluxAt(uint2(id.x, id.y + 1u)).z;
    }

    const float incoming =
        inWest + inEast + inNorth + inSouth;

    const float outgoing =
        own.x + own.y + own.z + own.w;

    const float cellArea =
        g_pc.spacingMeters * g_pc.spacingMeters;

    const float oldDepth =
        max(asfloat(g_waterRain.Load(index * 4u)), 0.0);

    const float volume =
        max(
            oldDepth * cellArea +
                g_pc.timeStepSeconds *
                (incoming - outgoing),
            0.0);

    const float depth =
        volume / max(cellArea, 1.0e-12);

    g_waterOut.Store(index * 4u, asuint(depth));

    const float crossSection =
        max(depth * g_pc.spacingMeters, 1.0e-9);

    const float qx =
        0.5 * ((own.y + inWest) - (own.x + inEast));

    const float qy =
        0.5 * ((own.w + inNorth) - (own.z + inSouth));

    const uint vo = index * 8u;

    g_velocityOut.Store(vo + 0u, asuint(qx / crossSection));
    g_velocityOut.Store(vo + 4u, asuint(qy / crossSection));
}
)";

inline constexpr const char* kM11ErodeDepositShader = R"(
[[vk::binding(0, 0)]]
ByteAddressBuffer g_water : register(t0);
[[vk::binding(1, 0)]]
ByteAddressBuffer g_velocity : register(t1);
[[vk::binding(2, 0)]]
ByteAddressBuffer g_sedimentIn : register(t2);
[[vk::binding(3, 0)]]
ByteAddressBuffer g_geology : register(t3);
[[vk::binding(4, 0)]]
RWByteAddressBuffer g_sedimentLocalOut : register(u4);

[[vk::binding(5, 0)]]
RWTexture2D<float> g_bedrock : register(u5);
[[vk::binding(6, 0)]]
RWTexture2D<float4> g_loose : register(u6);
[[vk::binding(7, 0)]]
RWTexture2D<uint> g_material : register(u7);

struct PushConstants
{
    uint resolution;
    float spacingMeters;
    float timeStepSeconds;
    float capacityCoefficient;
    float maximumConcentration;
    float erosionRatePerSecond;
    float depositionRatePerSecond;
    float maximumErosionDepth;
    float maximumDepositionDepth;
    float regolithMobility;
    float soilMobility;
    float sandMobility;
    float debrisMobility;
    float regolithDensity;
    float soilDensity;
    float sandDensity;
    float debrisDensity;
};

[[vk::push_constant]] PushConstants g_pc;

float Surface(uint2 p)
{
    const float4 loose = g_loose[p];
    return g_bedrock[p] + loose.x + loose.y + loose.z + loose.w;
}

float4 ReadRock(uint materialIndex)
{
    const uint o = materialIndex * 32u;

    return float4(
        asfloat(g_geology.Load(o + 0u)),
        asfloat(g_geology.Load(o + 4u)),
        asfloat(g_geology.Load(o + 8u)),
        asfloat(g_geology.Load(o + 28u)));
}

float MaximumSlope(uint2 p)
{
    const float source = Surface(p);
    float slope = 0.0;

    const int2 offsets[4] = {
        int2(-1, 0),
        int2(1, 0),
        int2(0, -1),
        int2(0, 1)
    };

    for (uint i = 0u; i < 4u; ++i)
    {
        const int2 q = int2(p) + offsets[i];

        if (q.x < 0 || q.y < 0 ||
            q.x >= int(g_pc.resolution) ||
            q.y >= int(g_pc.resolution))
        {
            continue;
        }

        slope = max(
            slope,
            (source - Surface(uint2(q))) /
                max(g_pc.spacingMeters, 1.0e-6));
    }

    return max(slope, 0.0);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_pc.resolution || id.y >= g_pc.resolution)
    {
        return;
    }

    const uint2 p = id.xy;
    const uint index = id.y * g_pc.resolution + id.x;

    const float water =
        max(asfloat(g_water.Load(index * 4u)), 0.0);

    const uint vo = index * 8u;
    const float2 velocity = float2(
        asfloat(g_velocity.Load(vo + 0u)),
        asfloat(g_velocity.Load(vo + 4u)));

    const float speed = length(velocity);
    const float slope = MaximumSlope(p);

    const float concentration =
        clamp(
            g_pc.capacityCoefficient *
                speed *
                slope,
            0.0,
            g_pc.maximumConcentration);

    // kg/m2: concentration [kg/m3] * water depth [m].
    const float capacity =
        concentration * water;

    float sediment =
        max(asfloat(g_sedimentIn.Load(index * 4u)), 0.0);

    float4 loose = max(g_loose[p], 0.0);
    float bedrock = g_bedrock[p];

    const uint materialIndex = g_material[p];
    const float4 rock = ReadRock(materialIndex);

    const float hardness = clamp(rock.x, 0.0, 1.0);
    const float cohesion = clamp(rock.y, 0.0, 1.0);
    const float hydraulic = clamp(rock.z, 0.0, 1.0);
    const float bedrockDensity = max(rock.w, 1.0);

    if (sediment < capacity &&
        g_pc.maximumErosionDepth > 0.0)
    {
        float mobility = 0.0;
        float exposedDensity = bedrockDensity;

        if (loose.w > 1.0e-6)
        {
            mobility = g_pc.debrisMobility;
            exposedDensity = g_pc.debrisDensity;
        }
        else if (loose.z > 1.0e-6)
        {
            mobility = g_pc.sandMobility;
            exposedDensity = g_pc.sandDensity;
        }
        else if (loose.y > 1.0e-6)
        {
            mobility = g_pc.soilMobility;
            exposedDensity = g_pc.soilDensity;
        }
        else if (loose.x > 1.0e-6)
        {
            mobility = g_pc.regolithMobility;
            exposedDensity = g_pc.regolithDensity;
        }
        else
        {
            mobility =
                hydraulic *
                (1.0 - 0.65 * hardness) *
                (1.0 - 0.35 * cohesion);
        }

        const float exchange =
            saturate(
                g_pc.erosionRatePerSecond *
                g_pc.timeStepSeconds);

        const float requestedMassArea =
            (capacity - sediment) *
            exchange *
            clamp(mobility, 0.0, 1.0);

        float remaining =
            min(
                requestedMassArea /
                    max(exposedDensity, 1.0),
                g_pc.maximumErosionDepth);

        float removedMassArea = 0.0;

        const float takeDebris = min(loose.w, remaining);
        loose.w -= takeDebris;
        remaining -= takeDebris;
        removedMassArea += takeDebris * g_pc.debrisDensity;

        const float takeSand = min(loose.z, remaining);
        loose.z -= takeSand;
        remaining -= takeSand;
        removedMassArea += takeSand * g_pc.sandDensity;

        const float takeSoil = min(loose.y, remaining);
        loose.y -= takeSoil;
        remaining -= takeSoil;
        removedMassArea += takeSoil * g_pc.soilDensity;

        const float takeRegolith = min(loose.x, remaining);
        loose.x -= takeRegolith;
        remaining -= takeRegolith;
        removedMassArea += takeRegolith * g_pc.regolithDensity;

        if (remaining > 0.0)
        {
            bedrock -= remaining;
            removedMassArea += remaining * bedrockDensity;
        }

        sediment += removedMassArea;
    }
    else if (sediment > capacity &&
             g_pc.maximumDepositionDepth > 0.0)
    {
        const float exchange =
            saturate(
                g_pc.depositionRatePerSecond *
                g_pc.timeStepSeconds);

        const float depositMassArea =
            min(
                (sediment - capacity) * exchange,
                g_pc.maximumDepositionDepth *
                    g_pc.sandDensity);

        const float depth =
            depositMassArea /
            max(g_pc.sandDensity, 1.0);

        loose.z += depth;
        sediment = max(sediment - depositMassArea, 0.0);
    }

    g_bedrock[p] = bedrock;
    g_loose[p] = loose;
    g_sedimentLocalOut.Store(
        index * 4u,
        asuint(sediment));
}
)";

inline constexpr const char* kM11SedimentTransportShader = R"(
[[vk::binding(0, 0)]]
ByteAddressBuffer g_sedimentLocal : register(t0);
[[vk::binding(1, 0)]]
ByteAddressBuffer g_waterBeforeFlow : register(t1);
[[vk::binding(2, 0)]]
ByteAddressBuffer g_flux : register(t2);
[[vk::binding(3, 0)]]
RWByteAddressBuffer g_sedimentOut : register(u3);

struct PushConstants
{
    uint resolution;
    float spacingMeters;
    float timeStepSeconds;
};

[[vk::push_constant]] PushConstants g_pc;

float4 FluxAt(uint2 p)
{
    const uint index = p.y * g_pc.resolution + p.x;
    const uint o = index * 16u;

    return float4(
        asfloat(g_flux.Load(o + 0u)),
        asfloat(g_flux.Load(o + 4u)),
        asfloat(g_flux.Load(o + 8u)),
        asfloat(g_flux.Load(o + 12u)));
}

float TransportFraction(uint2 p, float4 flux)
{
    const uint index = p.y * g_pc.resolution + p.x;
    const float depth =
        max(asfloat(g_waterBeforeFlow.Load(index * 4u)), 0.0);

    const float cellArea =
        g_pc.spacingMeters * g_pc.spacingMeters;

    const float volume = depth * cellArea;
    const float totalFlux = flux.x + flux.y + flux.z + flux.w;

    if (volume <= 0.0 || totalFlux <= 0.0)
    {
        return 0.0;
    }

    return saturate(
        g_pc.timeStepSeconds *
        totalFlux /
        volume);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_pc.resolution || id.y >= g_pc.resolution)
    {
        return;
    }

    const uint2 p = id.xy;
    const uint index = id.y * g_pc.resolution + id.x;

    const float ownSediment =
        max(asfloat(g_sedimentLocal.Load(index * 4u)), 0.0);

    const float4 ownFlux = FluxAt(p);
    const float ownFraction =
        TransportFraction(p, ownFlux);

    float result =
        ownSediment *
        (1.0 - ownFraction);

    if (id.x > 0u)
    {
        const uint2 q = uint2(id.x - 1u, id.y);
        const uint qi = q.y * g_pc.resolution + q.x;
        const float4 qf = FluxAt(q);
        const float qsum = qf.x + qf.y + qf.z + qf.w;

        if (qsum > 0.0)
        {
            result +=
                max(asfloat(g_sedimentLocal.Load(qi * 4u)), 0.0) *
                TransportFraction(q, qf) *
                qf.y / qsum;
        }
    }

    if (id.x + 1u < g_pc.resolution)
    {
        const uint2 q = uint2(id.x + 1u, id.y);
        const uint qi = q.y * g_pc.resolution + q.x;
        const float4 qf = FluxAt(q);
        const float qsum = qf.x + qf.y + qf.z + qf.w;

        if (qsum > 0.0)
        {
            result +=
                max(asfloat(g_sedimentLocal.Load(qi * 4u)), 0.0) *
                TransportFraction(q, qf) *
                qf.x / qsum;
        }
    }

    if (id.y > 0u)
    {
        const uint2 q = uint2(id.x, id.y - 1u);
        const uint qi = q.y * g_pc.resolution + q.x;
        const float4 qf = FluxAt(q);
        const float qsum = qf.x + qf.y + qf.z + qf.w;

        if (qsum > 0.0)
        {
            result +=
                max(asfloat(g_sedimentLocal.Load(qi * 4u)), 0.0) *
                TransportFraction(q, qf) *
                qf.w / qsum;
        }
    }

    if (id.y + 1u < g_pc.resolution)
    {
        const uint2 q = uint2(id.x, id.y + 1u);
        const uint qi = q.y * g_pc.resolution + q.x;
        const float4 qf = FluxAt(q);
        const float qsum = qf.x + qf.y + qf.z + qf.w;

        if (qsum > 0.0)
        {
            result +=
                max(asfloat(g_sedimentLocal.Load(qi * 4u)), 0.0) *
                TransportFraction(q, qf) *
                qf.z / qsum;
        }
    }

    g_sedimentOut.Store(
        index * 4u,
        asuint(max(result, 0.0)));
}
)";

inline constexpr const char* kM11InfiltrationEvaporationShader = R"(
[[vk::binding(0, 0)]]
RWByteAddressBuffer g_water : register(u0);
[[vk::binding(1, 0)]]
ByteAddressBuffer g_geology : register(t1);

[[vk::binding(2, 0)]]
RWTexture2D<float2> g_moisture : register(u2);
[[vk::binding(3, 0)]]
RWTexture2D<uint> g_material : register(u3);

struct PushConstants
{
    uint resolution;
    float timeStepSeconds;
    float infiltrationMetersPerSecond;
    float moistureCapacityDepthMeters;
    float evaporationRatePerSecond;
};

[[vk::push_constant]] PushConstants g_pc;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_pc.resolution || id.y >= g_pc.resolution)
    {
        return;
    }

    const uint2 p = id.xy;
    const uint index = id.y * g_pc.resolution + id.x;

    float water =
        max(asfloat(g_water.Load(index * 4u)), 0.0);

    float2 moistureProcess = g_moisture[p];
    const float moisture = saturate(moistureProcess.x);

    const uint materialIndex = g_material[p];
    const uint geologyOffset = materialIndex * 32u;

    const float permeability =
        saturate(
            asfloat(
                g_geology.Load(
                    geologyOffset + 16u)));

    const float infiltration =
        min(
            water,
            g_pc.infiltrationMetersPerSecond *
                g_pc.timeStepSeconds *
                (0.25 + 0.75 * permeability) *
                (1.0 - moisture));

    water = max(water - infiltration, 0.0);

    moistureProcess.x =
        saturate(
            moisture +
            infiltration /
                max(
                    g_pc.moistureCapacityDepthMeters,
                    1.0e-6));

    const float evaporation =
        max(
            0.0,
            1.0 -
                g_pc.evaporationRatePerSecond *
                g_pc.timeStepSeconds);

    water *= evaporation;

    g_water.Store(index * 4u, asuint(water));
    g_moisture[p] = moistureProcess;
}
)";

inline constexpr const char* kM11SnapshotMaterialShader = R"(
[[vk::binding(0, 0)]]
RWByteAddressBuffer g_materialSnapshot : register(u0);

[[vk::binding(1, 0)]]
RWTexture2D<float> g_bedrock : register(u1);
[[vk::binding(2, 0)]]
RWTexture2D<float4> g_loose : register(u2);
[[vk::binding(3, 0)]]
RWTexture2D<float2> g_moisture : register(u3);

struct PushConstants
{
    uint resolution;
};

[[vk::push_constant]] PushConstants g_pc;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_pc.resolution || id.y >= g_pc.resolution)
    {
        return;
    }

    const uint index = id.y * g_pc.resolution + id.x;
    const uint o = index * 24u;

    const float bedrock = g_bedrock[id.xy];
    const float4 loose = g_loose[id.xy];
    const float moisture = g_moisture[id.xy].x;

    g_materialSnapshot.Store(o + 0u, asuint(bedrock));
    g_materialSnapshot.Store(o + 4u, asuint(loose.x));
    g_materialSnapshot.Store(o + 8u, asuint(loose.y));
    g_materialSnapshot.Store(o + 12u, asuint(loose.z));
    g_materialSnapshot.Store(o + 16u, asuint(loose.w));
    g_materialSnapshot.Store(o + 20u, asuint(moisture));
}
)";
} // namespace orbit::terrain_gpu::detail
