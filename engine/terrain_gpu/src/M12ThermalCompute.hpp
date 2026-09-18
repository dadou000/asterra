#pragma once

namespace orbit::terrain_gpu::detail
{
inline constexpr const char* kM12ComputeTransferShader = R"(
[[vk::binding(0, 0)]]
ByteAddressBuffer g_protection : register(t0);
[[vk::binding(1, 0)]]
ByteAddressBuffer g_geology : register(t1);
[[vk::binding(2, 0)]]
RWByteAddressBuffer g_transferOut : register(u2);

[[vk::binding(3, 0)]]
RWTexture2D<float> g_bedrock : register(u3);
[[vk::binding(4, 0)]]
RWTexture2D<float4> g_loose : register(u4);
[[vk::binding(5, 0)]]
RWTexture2D<uint> g_material : register(u5);

struct PushConstants
{
    uint resolution;
    float spacingMeters;

    float sandReposeDegrees;
    float debrisReposeDegrees;
    float regolithReposeDegrees;
    float soilReposeDegrees;

    float minimumBedrockFailureDegrees;
    float bedrockFailureAngleRangeDegrees;
    float bedrockFractureRate;

    float relaxation;
    float maximumTransferDepthMeters;

    float regolithDensity;
    float soilDensity;
    float sandDensity;
    float debrisDensity;
};

[[vk::push_constant]] PushConstants g_pc;

static const uint kNoTarget = 0xFFFFFFFFu;
static const uint kNone = 0u;
static const uint kRegolith = 1u;
static const uint kSoil = 2u;
static const uint kSand = 3u;
static const uint kDebris = 4u;
static const uint kBedrockToDebris = 5u;

float Surface(uint2 p)
{
    const float4 loose = max(g_loose[p], 0.0);
    return g_bedrock[p] + loose.x + loose.y + loose.z + loose.w;
}

float DegreesToRadians(float degrees)
{
    return degrees * 0.017453292519943295;
}

float RadiansToDegrees(float radians)
{
    return radians * 57.29577951308232;
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
    const uint transferOffset = index * 16u;

    g_transferOut.Store(transferOffset + 0u, kNoTarget);
    g_transferOut.Store(transferOffset + 4u, kNone);
    g_transferOut.Store(transferOffset + 8u, asuint(0.0));
    g_transferOut.Store(transferOffset + 12u, asuint(0.0));

    const float allowance =
        1.0 -
        saturate(
            asfloat(
                g_protection.Load(
                    index * 4u)));

    if (allowance <= 0.0)
    {
        return;
    }

    const float sourceSurface = Surface(p);

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

    uint bestTarget = kNoTarget;
    float bestDrop = 0.0;
    float bestDistance = g_pc.spacingMeters;
    float bestAngle = 0.0;

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
            sourceSurface -
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
            RadiansToDegrees(
                atan2(drop, distance));

        if (angle > bestAngle)
        {
            bestAngle = angle;
            bestDrop = drop;
            bestDistance = distance;
            bestTarget =
                uint(q.y) * g_pc.resolution +
                uint(q.x);
        }
    }

    if (bestTarget == kNoTarget)
    {
        return;
    }

    const float4 loose = max(g_loose[p], 0.0);

    uint kind = kNone;
    float criticalAngle = 0.0;
    float availableDepth = 0.0;
    float sourceDensity = 1.0;
    float depositDensity = 1.0;
    float mobility = 1.0;

    if (loose.w > 1.0e-6)
    {
        kind = kDebris;
        criticalAngle = g_pc.debrisReposeDegrees;
        availableDepth = loose.w;
        sourceDensity = g_pc.debrisDensity;
        depositDensity = g_pc.debrisDensity;
    }
    else if (loose.z > 1.0e-6)
    {
        kind = kSand;
        criticalAngle = g_pc.sandReposeDegrees;
        availableDepth = loose.z;
        sourceDensity = g_pc.sandDensity;
        depositDensity = g_pc.sandDensity;
    }
    else if (loose.y > 1.0e-6)
    {
        kind = kSoil;
        criticalAngle = g_pc.soilReposeDegrees;
        availableDepth = loose.y;
        sourceDensity = g_pc.soilDensity;
        depositDensity = g_pc.soilDensity;
    }
    else if (loose.x > 1.0e-6)
    {
        kind = kRegolith;
        criticalAngle = g_pc.regolithReposeDegrees;
        availableDepth = loose.x;
        sourceDensity = g_pc.regolithDensity;
        depositDensity = g_pc.regolithDensity;
    }
    else
    {
        kind = kBedrockToDebris;

        const uint materialIndex = g_material[p];
        const uint geologyOffset = materialIndex * 32u;

        const float hardness =
            saturate(
                asfloat(
                    g_geology.Load(
                        geologyOffset + 0u)));

        const float cohesion =
            saturate(
                asfloat(
                    g_geology.Load(
                        geologyOffset + 4u)));

        const float fracture =
            saturate(
                asfloat(
                    g_geology.Load(
                        geologyOffset + 24u)));

        sourceDensity =
            max(
                asfloat(
                    g_geology.Load(
                        geologyOffset + 28u)),
                1.0);

        depositDensity =
            max(
                g_pc.debrisDensity,
                1.0);

        const float competence =
            0.55 * hardness +
            0.45 * cohesion;

        criticalAngle =
            g_pc.minimumBedrockFailureDegrees +
            g_pc.bedrockFailureAngleRangeDegrees *
                competence;

        mobility =
            g_pc.bedrockFractureRate *
            fracture *
            (1.0 - 0.70 * hardness) *
            (1.0 - 0.50 * cohesion);

        availableDepth =
            g_pc.maximumTransferDepthMeters;
    }

    if (bestAngle <= criticalAngle ||
        availableDepth <= 0.0 ||
        mobility <= 0.0)
    {
        return;
    }

    const float allowedDrop =
        tan(
            DegreesToRadians(
                criticalAngle)) *
        bestDistance;

    const float excess =
        max(
            bestDrop - allowedDrop,
            0.0);

    const float sourceDepth =
        min(
            min(
                0.5 *
                    excess *
                    g_pc.relaxation *
                    allowance *
                    mobility,
                availableDepth),
            g_pc.maximumTransferDepthMeters);

    if (sourceDepth <= 0.0)
    {
        return;
    }

    const float depositDepth =
        sourceDepth *
        sourceDensity /
        max(depositDensity, 1.0);

    g_transferOut.Store(
        transferOffset + 0u,
        bestTarget);

    g_transferOut.Store(
        transferOffset + 4u,
        kind);

    g_transferOut.Store(
        transferOffset + 8u,
        asuint(sourceDepth));

    g_transferOut.Store(
        transferOffset + 12u,
        asuint(depositDepth));
}
)";

inline constexpr const char* kM12ApplyTransferShader = R"(
[[vk::binding(0, 0)]]
ByteAddressBuffer g_transfer : register(t0);

[[vk::binding(1, 0)]]
RWTexture2D<float> g_bedrock : register(u1);
[[vk::binding(2, 0)]]
RWTexture2D<float4> g_loose : register(u2);

struct PushConstants
{
    uint resolution;
};

[[vk::push_constant]] PushConstants g_pc;

static const uint kNoTarget = 0xFFFFFFFFu;
static const uint kRegolith = 1u;
static const uint kSoil = 2u;
static const uint kSand = 3u;
static const uint kDebris = 4u;
static const uint kBedrockToDebris = 5u;

void AddDeposit(
    inout float4 loose,
    uint kind,
    float depth)
{
    if (depth <= 0.0)
    {
        return;
    }

    if (kind == kRegolith)
    {
        loose.x += depth;
    }
    else if (kind == kSoil)
    {
        loose.y += depth;
    }
    else if (kind == kSand)
    {
        loose.z += depth;
    }
    else if (kind == kDebris ||
             kind == kBedrockToDebris)
    {
        loose.w += depth;
    }
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

    float bedrock = g_bedrock[p];
    float4 loose = max(g_loose[p], 0.0);

    const uint ownOffset = index * 16u;
    const uint ownTarget =
        g_transfer.Load(
            ownOffset + 0u);

    if (ownTarget != kNoTarget)
    {
        const uint ownKind =
            g_transfer.Load(
                ownOffset + 4u);

        const float sourceDepth =
            max(
                asfloat(
                    g_transfer.Load(
                        ownOffset + 8u)),
                0.0);

        if (ownKind == kRegolith)
        {
            loose.x = max(loose.x - sourceDepth, 0.0);
        }
        else if (ownKind == kSoil)
        {
            loose.y = max(loose.y - sourceDepth, 0.0);
        }
        else if (ownKind == kSand)
        {
            loose.z = max(loose.z - sourceDepth, 0.0);
        }
        else if (ownKind == kDebris)
        {
            loose.w = max(loose.w - sourceDepth, 0.0);
        }
        else if (ownKind == kBedrockToDebris)
        {
            bedrock -= sourceDepth;
        }
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

        const uint qIndex =
            uint(q.y) * g_pc.resolution +
            uint(q.x);

        const uint qOffset =
            qIndex * 16u;

        const uint target =
            g_transfer.Load(
                qOffset + 0u);

        if (target != index)
        {
            continue;
        }

        const uint kind =
            g_transfer.Load(
                qOffset + 4u);

        const float depositDepth =
            max(
                asfloat(
                    g_transfer.Load(
                        qOffset + 12u)),
                0.0);

        AddDeposit(
            loose,
            kind,
            depositDepth);
    }

    g_bedrock[p] = bedrock;
    g_loose[p] = max(loose, 0.0);
}
)";

inline constexpr const char* kM12SnapshotMaterialShader = R"(
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
