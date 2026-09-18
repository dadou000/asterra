#pragma once

namespace orbit::terrain_gpu::detail
{
inline constexpr const char* kM13InitializeAirborneShader = R"(
[[vk::binding(0, 0)]]
RWByteAddressBuffer g_airborneA : register(u0);
[[vk::binding(1, 0)]]
RWByteAddressBuffer g_airborneB : register(u1);

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
    const uint o = index * 8u;

    g_airborneA.Store(o + 0u, asuint(0.0));
    g_airborneA.Store(o + 4u, asuint(0.0));
    g_airborneB.Store(o + 0u, asuint(0.0));
    g_airborneB.Store(o + 4u, asuint(0.0));
}
)";

inline constexpr const char* kM13ExchangeShader = R"(
[[vk::binding(0, 0)]]
ByteAddressBuffer g_airborneIn : register(t0);
[[vk::binding(1, 0)]]
ByteAddressBuffer g_forcing : register(t1);
[[vk::binding(2, 0)]]
ByteAddressBuffer g_geology : register(t2);
[[vk::binding(3, 0)]]
RWByteAddressBuffer g_exchangeOut : register(u3);

[[vk::binding(4, 0)]]
RWTexture2D<float> g_bedrock : register(u4);
[[vk::binding(5, 0)]]
RWTexture2D<float4> g_loose : register(u5);
[[vk::binding(6, 0)]]
RWTexture2D<float2> g_moisture : register(u6);
[[vk::binding(7, 0)]]
RWTexture2D<uint> g_material : register(u7);

struct PushConstants
{
    uint resolution;
    float spacingMeters;
    float timeStepSeconds;

    float capacityCoefficient;
    float windSpeedExponent;

    uint shadowRayCells;
    float shadowStrength;
    float windwardExposureGain;
    float minimumExposure;
    float maximumExposure;

    float pickupRatePerSecond;
    float depositionRatePerSecond;
    float reptationFraction;

    float maximumSandPickupDepth;
    float maximumSoilPickupDepth;
    float maximumDepositionDepth;

    float moistureSuppressionExponent;

    float bedrockAbrasionRate;
    float maximumBedrockAbrasionDepth;
    float referenceSaltationWind;

    float sandDensity;
    float soilDensity;
};

[[vk::push_constant]] PushConstants g_pc;

static const uint kNone = 0u;
static const uint kPickupSand = 1u;
static const uint kPickupSoil = 2u;
static const uint kAbradeBedrock = 3u;

float Surface(uint2 p)
{
    const float4 loose = max(g_loose[p], 0.0);
    return g_bedrock[p] + loose.x + loose.y + loose.z + loose.w;
}

float4 ForcingAt(uint index)
{
    const uint o = index * 16u;

    return float4(
        asfloat(g_forcing.Load(o + 0u)),
        asfloat(g_forcing.Load(o + 4u)),
        asfloat(g_forcing.Load(o + 8u)),
        asfloat(g_forcing.Load(o + 12u)));
}

int2 QuantizeWind(float2 wind)
{
    const float2 pageWind = float2(wind.x, -wind.y);
    const float speed = length(pageWind);

    if (speed <= 1.0e-8)
    {
        return int2(0, 0);
    }

    const float angle = atan2(pageWind.y, pageWind.x);
    int sector = int(round(angle / 0.7853981633974483));
    sector = (sector % 8 + 8) % 8;

    if (sector == 0) return int2(1, 0);
    if (sector == 1) return int2(1, 1);
    if (sector == 2) return int2(0, 1);
    if (sector == 3) return int2(-1, 1);
    if (sector == 4) return int2(-1, 0);
    if (sector == 5) return int2(-1, -1);
    if (sector == 6) return int2(0, -1);
    return int2(1, -1);
}

float Exposure(uint2 p, int2 downwind)
{
    if (all(downwind == int2(0, 0)))
    {
        return 0.0;
    }

    const float source = Surface(p);
    float obstruction = 0.0;
    float windward = 0.0;

    [loop]
    for (uint step = 1u; step <= g_pc.shadowRayCells; ++step)
    {
        const int2 q =
            int2(p) -
            downwind *
            int(step);

        if (q.x < 0 || q.y < 0 ||
            q.x >= int(g_pc.resolution) ||
            q.y >= int(g_pc.resolution))
        {
            break;
        }

        const bool diagonal =
            downwind.x != 0 &&
            downwind.y != 0;

        const float distance =
            g_pc.spacingMeters *
            float(step) *
            (diagonal ? 1.4142135623730951 : 1.0);

        const float upwind =
            Surface(uint2(q));

        obstruction =
            max(
                obstruction,
                (upwind - source) /
                    max(distance, 1.0e-6));

        if (step == 1u)
        {
            windward =
                max(
                    (source - upwind) /
                        max(distance, 1.0e-6),
                    0.0);
        }
    }

    const float lee =
        exp(
            -g_pc.shadowStrength *
            max(obstruction, 0.0));

    const float windwardFactor =
        1.0 +
        g_pc.windwardExposureGain *
        windward;

    return clamp(
        lee * windwardFactor,
        g_pc.minimumExposure,
        g_pc.maximumExposure);
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
    const uint ao = index * 8u;
    const uint eo = index * 40u;

    const float4 forcing = ForcingAt(index);
    const float2 wind = forcing.xy;
    const float resistance = saturate(forcing.z);
    const float speed = length(wind);
    const int2 downwind = QuantizeWind(wind);

    int2 targetCoord =
        int2(p) +
        downwind;

    uint target = index;

    if (targetCoord.x >= 0 &&
        targetCoord.y >= 0 &&
        targetCoord.x < int(g_pc.resolution) &&
        targetCoord.y < int(g_pc.resolution))
    {
        target =
            uint(targetCoord.y) *
                g_pc.resolution +
            uint(targetCoord.x);
    }

    const float exposure =
        Exposure(p, downwind);

    const float moisture =
        saturate(g_moisture[p].x);

    const float suppression =
        pow(
            max(1.0 - moisture, 0.0),
            g_pc.moistureSuppressionExponent) *
        (1.0 - resistance);

    const float capacity =
        g_pc.capacityCoefficient *
        pow(max(speed, 0.0), g_pc.windSpeedExponent) *
        exposure *
        suppression;

    float airborneSand =
        max(asfloat(g_airborneIn.Load(ao + 0u)), 0.0);

    float airborneFines =
        max(asfloat(g_airborneIn.Load(ao + 4u)), 0.0);

    const float airborne =
        airborneSand +
        airborneFines;

    uint action = kNone;
    float sourceDepth = 0.0;
    float depositSandDepth = 0.0;
    float depositSoilDepth = 0.0;
    float reptationMassArea = 0.0;

    if (airborne > capacity)
    {
        const float fraction =
            saturate(
                g_pc.depositionRatePerSecond *
                g_pc.timeStepSeconds);

        const float desired =
            (airborne - capacity) *
            fraction;

        const float sandRatio =
            airborne > 0.0
                ? airborneSand / airborne
                : 0.0;

        const float finesRatio =
            1.0 - sandRatio;

        const float sandDepositArea =
            min(
                desired * sandRatio,
                g_pc.maximumDepositionDepth *
                    g_pc.sandDensity);

        const float finesDepositArea =
            min(
                desired * finesRatio,
                g_pc.maximumDepositionDepth *
                    g_pc.soilDensity);

        depositSandDepth =
            sandDepositArea /
            max(g_pc.sandDensity, 1.0);

        depositSoilDepth =
            finesDepositArea /
            max(g_pc.soilDensity, 1.0);

        airborneSand =
            max(
                airborneSand -
                    sandDepositArea,
                0.0);

        airborneFines =
            max(
                airborneFines -
                    finesDepositArea,
                0.0);
    }
    else if (airborne < capacity)
    {
        const float fraction =
            saturate(
                g_pc.pickupRatePerSecond *
                g_pc.timeStepSeconds);

        const float deficit =
            (capacity - airborne) *
            fraction;

        const float4 loose =
            max(g_loose[p], 0.0);

        if (loose.w <= 1.0e-6 &&
            loose.z > 1.0e-6)
        {
            action = kPickupSand;

            const float pickupMassArea =
                min(
                    min(
                        deficit,
                        loose.z *
                            g_pc.sandDensity),
                    g_pc.maximumSandPickupDepth *
                        g_pc.sandDensity);

            sourceDepth =
                pickupMassArea /
                max(
                    g_pc.sandDensity,
                    1.0);

            reptationMassArea =
                pickupMassArea *
                g_pc.reptationFraction;

            airborneSand +=
                pickupMassArea -
                reptationMassArea;
        }
        else if (
            loose.w <= 1.0e-6 &&
            loose.z <= 1.0e-6 &&
            loose.y > 1.0e-6)
        {
            action = kPickupSoil;

            const float pickupMassArea =
                min(
                    min(
                        deficit,
                        loose.y *
                            g_pc.soilDensity),
                    g_pc.maximumSoilPickupDepth *
                        g_pc.soilDensity);

            sourceDepth =
                pickupMassArea /
                max(
                    g_pc.soilDensity,
                    1.0);

            airborneFines +=
                pickupMassArea;
        }
        else if (
            loose.x <= 1.0e-6 &&
            loose.y <= 1.0e-6 &&
            loose.z <= 1.0e-6 &&
            loose.w <= 1.0e-6)
        {
            const uint materialIndex =
                g_material[p];

            const uint go =
                materialIndex * 32u;

            const float aeolian =
                saturate(
                    asfloat(
                        g_geology.Load(
                            go + 12u)));

            const float density =
                max(
                    asfloat(
                        g_geology.Load(
                            go + 28u)),
                    1.0);

            const float normalizedSpeed =
                speed /
                max(
                    g_pc.referenceSaltationWind,
                    1.0e-6);

            const float abrasionDepth =
                min(
                    g_pc.bedrockAbrasionRate *
                        pow(
                            max(normalizedSpeed, 0.0),
                            3.0) *
                        aeolian *
                        suppression *
                        exposure *
                        g_pc.timeStepSeconds,
                    g_pc.maximumBedrockAbrasionDepth);

            const float abrasionMassArea =
                min(
                    abrasionDepth *
                        density,
                    deficit);

            if (abrasionMassArea > 0.0)
            {
                action = kAbradeBedrock;

                sourceDepth =
                    abrasionMassArea /
                    density;

                airborneSand +=
                    abrasionMassArea;
            }
        }
    }

    g_exchangeOut.Store(eo + 0u, target);
    g_exchangeOut.Store(eo + 4u, action);
    g_exchangeOut.Store(eo + 8u, asuint(sourceDepth));
    g_exchangeOut.Store(eo + 12u, asuint(depositSandDepth));
    g_exchangeOut.Store(eo + 16u, asuint(depositSoilDepth));
    g_exchangeOut.Store(eo + 20u, asuint(airborneSand));
    g_exchangeOut.Store(eo + 24u, asuint(airborneFines));
    g_exchangeOut.Store(eo + 28u, asuint(reptationMassArea));
    g_exchangeOut.Store(eo + 32u, asuint(exposure));
    g_exchangeOut.Store(eo + 36u, asuint(capacity));
}
)";

inline constexpr const char* kM13ApplyExchangeShader = R"(
[[vk::binding(0, 0)]]
ByteAddressBuffer g_exchange : register(t0);

[[vk::binding(1, 0)]]
RWTexture2D<float> g_bedrock : register(u1);
[[vk::binding(2, 0)]]
RWTexture2D<float4> g_loose : register(u2);

struct PushConstants
{
    uint resolution;
    float sandDensity;
};

[[vk::push_constant]] PushConstants g_pc;

static const uint kPickupSand = 1u;
static const uint kPickupSoil = 2u;
static const uint kAbradeBedrock = 3u;

void AddReptationFrom(
    inout float4 loose,
    uint sourceIndex,
    uint targetIndex)
{
    const uint o = sourceIndex * 40u;

    if (g_exchange.Load(o + 0u) != targetIndex)
    {
        return;
    }

    const float massArea =
        max(
            asfloat(
                g_exchange.Load(
                    o + 28u)),
            0.0);

    loose.z +=
        massArea /
        max(g_pc.sandDensity, 1.0);
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
    const uint o = index * 40u;

    float bedrock = g_bedrock[p];
    float4 loose = max(g_loose[p], 0.0);

    const uint action = g_exchange.Load(o + 4u);
    const float sourceDepth =
        max(asfloat(g_exchange.Load(o + 8u)), 0.0);

    if (action == kPickupSand)
    {
        loose.z = max(loose.z - sourceDepth, 0.0);
    }
    else if (action == kPickupSoil)
    {
        loose.y = max(loose.y - sourceDepth, 0.0);
    }
    else if (action == kAbradeBedrock)
    {
        bedrock -= sourceDepth;
    }

    loose.z +=
        max(
            asfloat(
                g_exchange.Load(
                    o + 12u)),
            0.0);

    loose.y +=
        max(
            asfloat(
                g_exchange.Load(
                    o + 16u)),
            0.0);

    // Closed edge: a self-targeting reptation proposal deposits back locally.
    AddReptationFrom(
        loose,
        index,
        index);

    const int2 offsets[8] = {
        int2(-1, -1),
        int2(0, -1),
        int2(1, -1),
        int2(-1, 0),
        int2(1, 0),
        int2(-1, 1),
        int2(0, 1),
        int2(1, 1)
    };

    for (uint i = 0u; i < 8u; ++i)
    {
        const int2 q = int2(p) + offsets[i];

        if (q.x < 0 || q.y < 0 ||
            q.x >= int(g_pc.resolution) ||
            q.y >= int(g_pc.resolution))
        {
            continue;
        }

        const uint sourceIndex =
            uint(q.y) *
                g_pc.resolution +
            uint(q.x);

        AddReptationFrom(
            loose,
            sourceIndex,
            index);
    }

    g_bedrock[p] = bedrock;
    g_loose[p] = max(loose, 0.0);
}
)";

inline constexpr const char* kM13AvalancheProposalShader = R"(
[[vk::binding(0, 0)]]
RWByteAddressBuffer g_avalanche : register(u0);

[[vk::binding(1, 0)]]
RWTexture2D<float> g_bedrock : register(u1);
[[vk::binding(2, 0)]]
RWTexture2D<float4> g_loose : register(u2);

struct PushConstants
{
    uint resolution;
    float spacingMeters;
    float reposeDegrees;
    float relaxation;
    float maximumDepth;
};

[[vk::push_constant]] PushConstants g_pc;

static const uint kNoTarget = 0xFFFFFFFFu;

float Surface(uint2 p)
{
    const float4 loose = max(g_loose[p], 0.0);
    return g_bedrock[p] + loose.x + loose.y + loose.z + loose.w;
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
    const uint o = index * 8u;

    g_avalanche.Store(o + 0u, kNoTarget);
    g_avalanche.Store(o + 4u, asuint(0.0));

    const float4 loose = max(g_loose[p], 0.0);

    if (loose.w > 1.0e-6 ||
        loose.z <= 1.0e-6)
    {
        return;
    }

    const float source = Surface(p);

    const int2 offsets[8] = {
        int2(-1, -1),
        int2(0, -1),
        int2(1, -1),
        int2(-1, 0),
        int2(1, 0),
        int2(-1, 1),
        int2(0, 1),
        int2(1, 1)
    };

    float bestAngle = 0.0;
    float bestDrop = 0.0;
    float bestDistance = 1.0;
    uint bestTarget = kNoTarget;

    for (uint i = 0u; i < 8u; ++i)
    {
        const int2 q = int2(p) + offsets[i];

        if (q.x < 0 || q.y < 0 ||
            q.x >= int(g_pc.resolution) ||
            q.y >= int(g_pc.resolution))
        {
            continue;
        }

        const float drop =
            source -
            Surface(uint2(q));

        if (drop <= 0.0)
        {
            continue;
        }

        const bool diagonal =
            offsets[i].x != 0 &&
            offsets[i].y != 0;

        const float distance =
            g_pc.spacingMeters *
            (diagonal ? 1.4142135623730951 : 1.0);

        const float angle =
            atan2(drop, distance) *
            57.29577951308232;

        if (angle > bestAngle)
        {
            bestAngle = angle;
            bestDrop = drop;
            bestDistance = distance;
            bestTarget =
                uint(q.y) *
                    g_pc.resolution +
                uint(q.x);
        }
    }

    if (bestTarget == kNoTarget ||
        bestAngle <= g_pc.reposeDegrees)
    {
        return;
    }

    const float allowedDrop =
        tan(
            g_pc.reposeDegrees *
            0.017453292519943295) *
        bestDistance;

    const float depth =
        min(
            min(
                0.5 *
                    max(
                        bestDrop - allowedDrop,
                        0.0) *
                    g_pc.relaxation,
                loose.z),
            g_pc.maximumDepth);

    if (depth > 0.0)
    {
        g_avalanche.Store(o + 0u, bestTarget);
        g_avalanche.Store(o + 4u, asuint(depth));
    }
}
)";

inline constexpr const char* kM13AvalancheApplyShader = R"(
[[vk::binding(0, 0)]]
ByteAddressBuffer g_avalanche : register(t0);

[[vk::binding(1, 0)]]
RWTexture2D<float4> g_loose : register(u1);

struct PushConstants
{
    uint resolution;
};

[[vk::push_constant]] PushConstants g_pc;

static const uint kNoTarget = 0xFFFFFFFFu;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_pc.resolution || id.y >= g_pc.resolution)
    {
        return;
    }

    const uint2 p = id.xy;
    const uint index = id.y * g_pc.resolution + id.x;
    const uint o = index * 8u;

    float4 loose = max(g_loose[p], 0.0);

    const uint ownTarget =
        g_avalanche.Load(o + 0u);

    const float ownDepth =
        max(
            asfloat(
                g_avalanche.Load(
                    o + 4u)),
            0.0);

    if (ownTarget != kNoTarget)
    {
        loose.z =
            max(
                loose.z -
                    ownDepth,
                0.0);
    }

    const int2 offsets[8] = {
        int2(-1, -1),
        int2(0, -1),
        int2(1, -1),
        int2(-1, 0),
        int2(1, 0),
        int2(-1, 1),
        int2(0, 1),
        int2(1, 1)
    };

    for (uint i = 0u; i < 8u; ++i)
    {
        const int2 q = int2(p) + offsets[i];

        if (q.x < 0 || q.y < 0 ||
            q.x >= int(g_pc.resolution) ||
            q.y >= int(g_pc.resolution))
        {
            continue;
        }

        const uint sourceIndex =
            uint(q.y) *
                g_pc.resolution +
            uint(q.x);

        const uint so = sourceIndex * 8u;

        if (g_avalanche.Load(so + 0u) == index)
        {
            loose.z +=
                max(
                    asfloat(
                        g_avalanche.Load(
                            so + 4u)),
                    0.0);
        }
    }

    g_loose[p] = max(loose, 0.0);
}
)";

inline constexpr const char* kM13SaltationShader = R"(
[[vk::binding(0, 0)]]
ByteAddressBuffer g_exchange : register(t0);
[[vk::binding(1, 0)]]
ByteAddressBuffer g_forcing : register(t1);
[[vk::binding(2, 0)]]
RWByteAddressBuffer g_airborneOut : register(u2);

struct PushConstants
{
    uint resolution;
    float timeStepSeconds;
    float saltationRatePerSecond;
    float referenceWindMetersPerSecond;
};

[[vk::push_constant]] PushConstants g_pc;

float4 ForcingAt(uint index)
{
    const uint o = index * 16u;

    return float4(
        asfloat(g_forcing.Load(o + 0u)),
        asfloat(g_forcing.Load(o + 4u)),
        asfloat(g_forcing.Load(o + 8u)),
        asfloat(g_forcing.Load(o + 12u)));
}

float TransportFraction(uint index)
{
    const float speed =
        length(ForcingAt(index).xy);

    return saturate(
        g_pc.saltationRatePerSecond *
        g_pc.timeStepSeconds *
        speed /
        max(
            g_pc.referenceWindMetersPerSecond,
            1.0e-6));
}

void AddIncoming(
    inout float2 value,
    uint sourceIndex,
    uint targetIndex)
{
    const uint o = sourceIndex * 40u;

    if (g_exchange.Load(o + 0u) != targetIndex)
    {
        return;
    }

    const float fraction =
        TransportFraction(sourceIndex);

    value +=
        float2(
            max(asfloat(g_exchange.Load(o + 20u)), 0.0),
            max(asfloat(g_exchange.Load(o + 24u)), 0.0)) *
        fraction;
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
    const uint o = index * 40u;

    const float2 local =
        float2(
            max(asfloat(g_exchange.Load(o + 20u)), 0.0),
            max(asfloat(g_exchange.Load(o + 24u)), 0.0));

    const float ownFraction =
        TransportFraction(index);

    float2 next =
        local *
        (1.0 - ownFraction);

    // Closed M13 edge: self-targeting transported mass remains local.
    AddIncoming(next, index, index);

    const int2 offsets[8] = {
        int2(-1, -1),
        int2(0, -1),
        int2(1, -1),
        int2(-1, 0),
        int2(1, 0),
        int2(-1, 1),
        int2(0, 1),
        int2(1, 1)
    };

    for (uint i = 0u; i < 8u; ++i)
    {
        const int2 q = int2(p) + offsets[i];

        if (q.x < 0 || q.y < 0 ||
            q.x >= int(g_pc.resolution) ||
            q.y >= int(g_pc.resolution))
        {
            continue;
        }

        const uint sourceIndex =
            uint(q.y) *
                g_pc.resolution +
            uint(q.x);

        AddIncoming(
            next,
            sourceIndex,
            index);
    }

    const uint ao = index * 8u;

    g_airborneOut.Store(ao + 0u, asuint(max(next.x, 0.0)));
    g_airborneOut.Store(ao + 4u, asuint(max(next.y, 0.0)));
}
)";

inline constexpr const char* kM13SnapshotMaterialShader = R"(
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
