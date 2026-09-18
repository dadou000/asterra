#pragma once

namespace orbit::terrain_gpu::detail
{
// M09 surface extraction. Interior texels come from the M08 physical material
// column; the one-cell ring is neighboring M09 conditioned boundary state.
inline constexpr const char* kM09ExtractSurfaceShader = R"(
[[vk::binding(0, 0)]]
ByteAddressBuffer g_haloConditioned : register(t0);

[[vk::binding(1, 0)]]
RWByteAddressBuffer g_surfaceOut : register(u1);

[[vk::binding(2, 0)]]
RWTexture2D<float> g_bedrockHeight : register(u2);

[[vk::binding(3, 0)]]
RWTexture2D<float4> g_looseMaterials : register(u3);

struct PushConstants
{
    uint coreResolution;
    uint paddedResolution;
};

[[vk::push_constant]] PushConstants g_pc;

[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
    if (dispatchId.x >= g_pc.paddedResolution ||
        dispatchId.y >= g_pc.paddedResolution)
    {
        return;
    }

    const uint paddedIndex =
        dispatchId.y * g_pc.paddedResolution + dispatchId.x;

    const bool isHalo =
        dispatchId.x == 0u ||
        dispatchId.y == 0u ||
        dispatchId.x == g_pc.paddedResolution - 1u ||
        dispatchId.y == g_pc.paddedResolution - 1u;

    float surface;

    if (isHalo)
    {
        surface =
            asfloat(
                g_haloConditioned.Load(
                    paddedIndex * 4u));
    }
    else
    {
        const uint2 coreCoord =
            uint2(
                dispatchId.x - 1u,
                dispatchId.y - 1u);

        const float bedrock =
            g_bedrockHeight[coreCoord];

        const float4 loose =
            g_looseMaterials[coreCoord];

        surface =
            bedrock +
            loose.x +
            loose.y +
            loose.z +
            loose.w;
    }

    g_surfaceOut.Store(
        paddedIndex * 4u,
        asuint(surface));
}
)";

// Guided deterministic SFD. Guidance can only rank already-downhill
// candidates, so authored intent never makes an uphill edge eligible.
inline constexpr const char* kM09DownstreamShader = R"(
[[vk::binding(0, 0)]]
ByteAddressBuffer g_drainage : register(t0);

[[vk::binding(1, 0)]]
ByteAddressBuffer g_guidance : register(t1);

[[vk::binding(2, 0)]]
RWByteAddressBuffer g_downstreamOut : register(u2);

struct PushConstants
{
    uint coreResolution;
    uint paddedResolution;
    float spacingMeters;
    float guidanceWeight;
};

[[vk::push_constant]] PushConstants g_pc;

static const uint kNoDownstream = 0xFFFFFFFFu;

[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
    if (dispatchId.x >= g_pc.paddedResolution ||
        dispatchId.y >= g_pc.paddedResolution)
    {
        return;
    }

    const uint index =
        dispatchId.y * g_pc.paddedResolution +
        dispatchId.x;

    const bool isHalo =
        dispatchId.x == 0u ||
        dispatchId.y == 0u ||
        dispatchId.x == g_pc.paddedResolution - 1u ||
        dispatchId.y == g_pc.paddedResolution - 1u;

    if (isHalo)
    {
        g_downstreamOut.Store(
            index * 4u,
            kNoDownstream);
        return;
    }

    const float myDrainage =
        asfloat(
            g_drainage.Load(
                index * 4u));

    float bestScore = 0.0;
    uint bestIndex = kNoDownstream;

    for (int dy = -1; dy <= 1; ++dy)
    {
        for (int dx = -1; dx <= 1; ++dx)
        {
            if (dx == 0 && dy == 0)
            {
                continue;
            }

            const int nx =
                int(dispatchId.x) + dx;
            const int ny =
                int(dispatchId.y) + dy;

            if (nx < 0 ||
                ny < 0 ||
                nx >= int(g_pc.paddedResolution) ||
                ny >= int(g_pc.paddedResolution))
            {
                continue;
            }

            const uint neighborIndex =
                uint(ny) *
                    g_pc.paddedResolution +
                uint(nx);

            const float neighborDrainage =
                asfloat(
                    g_drainage.Load(
                        neighborIndex * 4u));

            const float drop =
                myDrainage -
                neighborDrainage;

            if (drop <= 0.0)
            {
                continue;
            }

            const float distanceScale =
                (dx != 0 && dy != 0)
                    ? 1.41421356237
                    : 1.0;

            const float slope =
                drop /
                (g_pc.spacingMeters *
                 distanceScale);

            float guidance = 0.0;

            const bool neighborIsCore =
                nx >= 1 &&
                ny >= 1 &&
                nx <= int(g_pc.coreResolution) &&
                ny <= int(g_pc.coreResolution);

            if (neighborIsCore)
            {
                const uint coreX =
                    uint(nx - 1);
                const uint coreY =
                    uint(ny - 1);

                const uint coreIndex =
                    coreY *
                        g_pc.coreResolution +
                    coreX;

                guidance =
                    clamp(
                        asfloat(
                            g_guidance.Load(
                                coreIndex * 4u)),
                        -1.0,
                        1.0);
            }

            const float score =
                slope *
                (1.0 +
                 g_pc.guidanceWeight *
                     guidance);

            const bool better =
                score > bestScore ||
                (score == bestScore &&
                 neighborIndex < bestIndex);

            if (better)
            {
                bestScore = score;
                bestIndex = neighborIndex;
            }
        }
    }

    g_downstreamOut.Store(
        index * 4u,
        bestIndex);
}
)";

// Initialize conserved quantities and donor counts. Halo cells carry no local
// seed: cross-page inflow is pre-mapped into the receiving core-cell inputs.
inline constexpr const char* kM09InitializeFlowShader = R"(
[[vk::binding(0, 0)]]
ByteAddressBuffer g_runoffRate : register(t0);

[[vk::binding(1, 0)]]
ByteAddressBuffer g_incomingArea : register(t1);

[[vk::binding(2, 0)]]
ByteAddressBuffer g_incomingDischarge : register(t2);

[[vk::binding(3, 0)]]
RWByteAddressBuffer g_donorCountOut : register(u3);

[[vk::binding(4, 0)]]
RWByteAddressBuffer g_areaOut : register(u4);

[[vk::binding(5, 0)]]
RWByteAddressBuffer g_dischargeOut : register(u5);

struct PushConstants
{
    uint coreResolution;
    uint paddedResolution;
    float cellAreaSquareMeters;
};

[[vk::push_constant]] PushConstants g_pc;

[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
    if (dispatchId.x >= g_pc.paddedResolution ||
        dispatchId.y >= g_pc.paddedResolution)
    {
        return;
    }

    const uint paddedIndex =
        dispatchId.y *
            g_pc.paddedResolution +
        dispatchId.x;

    g_donorCountOut.Store(
        paddedIndex * 4u,
        0u);

    const bool isHalo =
        dispatchId.x == 0u ||
        dispatchId.y == 0u ||
        dispatchId.x == g_pc.paddedResolution - 1u ||
        dispatchId.y == g_pc.paddedResolution - 1u;

    if (isHalo)
    {
        g_areaOut.Store(
            paddedIndex * 4u,
            asuint(0.0));

        g_dischargeOut.Store(
            paddedIndex * 4u,
            asuint(0.0));

        return;
    }

    const uint coreX =
        dispatchId.x - 1u;
    const uint coreY =
        dispatchId.y - 1u;

    const uint coreIndex =
        coreY *
            g_pc.coreResolution +
        coreX;

    const float runoff =
        max(
            asfloat(
                g_runoffRate.Load(
                    coreIndex * 4u)),
            0.0);

    const float incomingArea =
        max(
            asfloat(
                g_incomingArea.Load(
                    coreIndex * 4u)),
            0.0);

    const float incomingDischarge =
        max(
            asfloat(
                g_incomingDischarge.Load(
                    coreIndex * 4u)),
            0.0);

    const float area =
        g_pc.cellAreaSquareMeters +
        incomingArea;

    const float discharge =
        runoff *
            g_pc.cellAreaSquareMeters +
        incomingDischarge;

    g_areaOut.Store(
        paddedIndex * 4u,
        asuint(area));

    g_dischargeOut.Store(
        paddedIndex * 4u,
        asuint(discharge));
}
)";

// Build the inverse stream-tree adjacency. D8 SFD has at most eight donors per
// cell. The atomic only allocates each donor slot; no floating-point
// accumulation uses atomics.
inline constexpr const char* kM09BuildDonorsShader = R"(
[[vk::binding(0, 0)]]
ByteAddressBuffer g_downstream : register(t0);

[[vk::binding(1, 0)]]
RWByteAddressBuffer g_donorCount : register(u1);

[[vk::binding(2, 0)]]
RWByteAddressBuffer g_donors : register(u2);

struct PushConstants
{
    uint paddedResolution;
};

[[vk::push_constant]] PushConstants g_pc;

static const uint kNoDownstream = 0xFFFFFFFFu;
static const uint kMaxDonors = 8u;

[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
    if (dispatchId.x >= g_pc.paddedResolution ||
        dispatchId.y >= g_pc.paddedResolution)
    {
        return;
    }

    const uint index =
        dispatchId.y *
            g_pc.paddedResolution +
        dispatchId.x;

    const uint recipient =
        g_downstream.Load(
            index * 4u);

    if (recipient == kNoDownstream)
    {
        return;
    }

    uint slot = 0u;

    g_donorCount.InterlockedAdd(
        recipient * 4u,
        1u,
        slot);

    if (slot < kMaxDonors)
    {
        const uint donorOffset =
            (recipient *
                 kMaxDonors +
             slot) *
            4u;

        g_donors.Store(
            donorOffset,
            index);
    }
}
)";

// FastFlow-style rake-compress. Each round prunes leaf donors and pointer-jumps
// single-donor chains. Ping-pong state removes RAW hazards. Area and discharge
// share the exact same graph reduction and therefore remain consistent.
inline constexpr const char* kM09RakeCompressShader = R"(
[[vk::binding(0, 0)]]
ByteAddressBuffer g_donorCountIn : register(t0);

[[vk::binding(1, 0)]]
ByteAddressBuffer g_donorsIn : register(t1);

[[vk::binding(2, 0)]]
ByteAddressBuffer g_areaIn : register(t2);

[[vk::binding(3, 0)]]
ByteAddressBuffer g_dischargeIn : register(t3);

[[vk::binding(4, 0)]]
RWByteAddressBuffer g_donorCountOut : register(u4);

[[vk::binding(5, 0)]]
RWByteAddressBuffer g_donorsOut : register(u5);

[[vk::binding(6, 0)]]
RWByteAddressBuffer g_areaOut : register(u6);

[[vk::binding(7, 0)]]
RWByteAddressBuffer g_dischargeOut : register(u7);

struct PushConstants
{
    uint nodeCount;
};

[[vk::push_constant]] PushConstants g_pc;

static const uint kMaxDonors = 8u;

[numthreads(256, 1, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
    const uint cell = dispatchId.x;

    if (cell >= g_pc.nodeCount)
    {
        return;
    }

    const uint donorCount =
        min(
            g_donorCountIn.Load(
                cell * 4u),
            kMaxDonors);

    float area =
        asfloat(
            g_areaIn.Load(
                cell * 4u));

    float discharge =
        asfloat(
            g_dischargeIn.Load(
                cell * 4u));

    uint newCount = 0u;

    for (uint i = 0u;
         i < donorCount;
         ++i)
    {
        const uint donor =
            g_donorsIn.Load(
                (cell *
                     kMaxDonors +
                 i) *
                4u);

        const uint donorDonorCount =
            min(
                g_donorCountIn.Load(
                    donor * 4u),
                kMaxDonors);

        if (donorDonorCount == 0u)
        {
            area +=
                asfloat(
                    g_areaIn.Load(
                        donor * 4u));

            discharge +=
                asfloat(
                    g_dischargeIn.Load(
                        donor * 4u));

            continue;
        }

        if (donorDonorCount == 1u)
        {
            area +=
                asfloat(
                    g_areaIn.Load(
                        donor * 4u));

            discharge +=
                asfloat(
                    g_dischargeIn.Load(
                        donor * 4u));

            const uint replacement =
                g_donorsIn.Load(
                    (donor *
                         kMaxDonors) *
                    4u);

            g_donorsOut.Store(
                (cell *
                     kMaxDonors +
                 newCount) *
                    4u,
                replacement);

            ++newCount;
            continue;
        }

        g_donorsOut.Store(
            (cell *
                 kMaxDonors +
             newCount) *
                4u,
            donor);

        ++newCount;
    }

    g_donorCountOut.Store(
        cell * 4u,
        newCount);

    g_areaOut.Store(
        cell * 4u,
        asuint(area));

    g_dischargeOut.Store(
        cell * 4u,
        asuint(discharge));
}
)";
} // namespace orbit::terrain_gpu::detail
