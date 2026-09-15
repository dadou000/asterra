#pragma once
namespace orbit::terrain_gpu::detail
{
// Zeroes a flat f32 buffer -- used to seed the sediment-routing ping-
// pong buffer with "no sediment yet" before GpuErosion's iterative
// pass reads it for the first time each Dispatch() call (a GPU-only
// buffer's contents are otherwise whatever an earlier, unrelated
// Dispatch() call left behind).
inline constexpr const char* kZeroFloatBufferComputeShader = R"(
[[vk::binding(0, 0)]]
RWByteAddressBuffer g_buffer : register(u0);

struct PushConstants
{
    uint resolution;
};

[[vk::push_constant]] PushConstants g_pc;

[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
    if (dispatchId.x >= g_pc.resolution || dispatchId.y >= g_pc.resolution)
    {
        return;
    }

    const uint index = dispatchId.y * g_pc.resolution + dispatchId.x;
    g_buffer.Store(index * 4u, 0u);
}
)";

// Port of engine/terrain_erosion's CPU SedimentTransport.cpp --
// erosion(cell) = clamp(erosionScaleMeters *
//                        (drainageArea/referenceDrainageArea)^areaExp *
//                        slope^slopeExp, 0, maximumErosionMeters),
// zero over "ocean" (drainage <= seaLevel), and deposition draining a
// per-cell sediment budget (incoming + local erosion) by a fraction
// that's high over ocean and rises as slope flattens on land, same
// formula, same constants.
//
// The CPU version processes cells in a single elevation-sorted sweep
// (each cell's incoming sediment needs every upstream cell already
// resolved). This GPU port uses the same trick as
// FlowAccumulationCompute.hpp: a Jacobi relaxation over the downstream
// forest (already computed by GpuFlowAccumulation and passed in here),
// gathering each cell's incoming sediment from whichever neighbors
// point to it and re-deriving erosion/slope/deposition fresh every
// pass (cheap, and avoids a separate factors buffer) -- converges to
// the same fixed point once every chain has had enough passes to
// drain, same non-parity caveat as the other Milestone 4 passes.
inline constexpr const char* kErosionRoutingComputeShader = R"(
[[vk::binding(0, 0)]]
ByteAddressBuffer g_drainage : register(t0);

[[vk::binding(1, 0)]]
ByteAddressBuffer g_accumulation : register(t1);

[[vk::binding(2, 0)]]
ByteAddressBuffer g_downstream : register(t2);

[[vk::binding(3, 0)]]
ByteAddressBuffer g_outgoingIn : register(t3);

[[vk::binding(4, 0)]]
RWByteAddressBuffer g_outgoingOut : register(u4);

[[vk::binding(5, 0)]]
RWByteAddressBuffer g_netDeltaOut : register(u5);

struct PushConstants
{
    uint resolution;
    float spacingMeters;
    float seaLevelMeters;
    float referenceDrainageAreaSquareMeters;
    float erosionScaleMeters;
    float maximumErosionMeters;
    float drainageAreaExponent;
    float slopeExponent;
    float depositionSlopeThreshold;
    float maximumLandDepositionFraction;
    float oceanDepositionFraction;
    float maximumDepositionMeters;
};

[[vk::push_constant]] PushConstants g_pc;

static const uint kNoDownstream = 0xFFFFFFFFu;

[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
    if (dispatchId.x >= g_pc.resolution || dispatchId.y >= g_pc.resolution)
    {
        return;
    }

    const uint index = dispatchId.y * g_pc.resolution + dispatchId.x;

    const float myDrainage = asfloat(g_drainage.Load(index * 4u));
    const uint downstreamIndex = g_downstream.Load(index * 4u);
    const bool hasDownstream = downstreamIndex != kNoDownstream;

    float slope = 0.0;

    if (hasDownstream)
    {
        const float downstreamDrainage =
            asfloat(g_drainage.Load(downstreamIndex * 4u));

        slope = max(
            (myDrainage - downstreamDrainage) /
                max(g_pc.spacingMeters, 0.0001),
            0.0);
    }

    const bool isOcean = myDrainage <= g_pc.seaLevelMeters;

    const float accumulation =
        asfloat(g_accumulation.Load(index * 4u));

    const float cellArea = g_pc.spacingMeters * g_pc.spacingMeters;
    const float drainageArea = max(accumulation * cellArea, 0.0);

    const float areaFactor = pow(
        max(
            drainageArea / g_pc.referenceDrainageAreaSquareMeters,
            1.0e-6),
        g_pc.drainageAreaExponent);

    const float slopeFactor = pow(max(slope, 0.0), g_pc.slopeExponent);

    const float erosion =
        isOcean
            ? 0.0
            : clamp(
                g_pc.erosionScaleMeters * areaFactor * slopeFactor,
                0.0,
                g_pc.maximumErosionMeters);

    float incoming = 0.0;

    for (int dy = -1; dy <= 1; ++dy)
    {
        for (int dx = -1; dx <= 1; ++dx)
        {
            if (dx == 0 && dy == 0)
            {
                continue;
            }

            const int nx = int(dispatchId.x) + dx;
            const int ny = int(dispatchId.y) + dy;

            if (nx < 0 || ny < 0 ||
                nx >= int(g_pc.resolution) || ny >= int(g_pc.resolution))
            {
                continue;
            }

            const uint neighborIndex =
                uint(ny) * g_pc.resolution + uint(nx);

            if (g_downstream.Load(neighborIndex * 4u) == index)
            {
                incoming += asfloat(g_outgoingIn.Load(neighborIndex * 4u));
            }
        }
    }

    const float available = max(incoming + erosion, 0.0);

    float depositionFraction = 0.0;

    if (isOcean)
    {
        depositionFraction = g_pc.oceanDepositionFraction;
    }
    else if (g_pc.depositionSlopeThreshold > 0.0)
    {
        const float lowSlope = clamp(
            1.0 - slope / g_pc.depositionSlopeThreshold, 0.0, 1.0);

        depositionFraction =
            lowSlope * g_pc.maximumLandDepositionFraction;
    }

    const float deposition = min(
        available * depositionFraction,
        g_pc.maximumDepositionMeters);

    const float outgoing = max(available - deposition, 0.0);
    const float netDelta = deposition - erosion;

    g_outgoingOut.Store(index * 4u, asuint(outgoing));
    g_netDeltaOut.Store(index * 4u, asuint(netDelta));
}
)";
} // namespace orbit::terrain_gpu::detail
