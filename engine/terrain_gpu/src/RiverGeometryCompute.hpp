#pragma once
namespace orbit::terrain_gpu::detail
{
// Port of engine/terrain_erosion's CPU RiverCarving.cpp
// (BuildRiverCarvingField) and terrain_hydrology's RiverGraph.cpp
// (BuildRiverGraph's drainage-area gate) -- a direct parallel kernel,
// no iteration needed, once accumulation (GpuFlowAccumulation) exists:
// every cell's channel half-width/depth/bed elevation/river gate is a
// pure function of its own drainage area (accumulation * cellArea).
// Same formula and constants as the CPU version; unlike the relaxation
// passes, this one has no non-parity concern at all -- it's the same
// closed-form expression evaluated per cell either way.
inline constexpr const char* kRiverGeometryComputeShader = R"(
[[vk::binding(0, 0)]]
ByteAddressBuffer g_drainage : register(t0);

[[vk::binding(1, 0)]]
ByteAddressBuffer g_accumulation : register(t1);

[[vk::binding(2, 0)]]
RWByteAddressBuffer g_output : register(u2);

struct PushConstants
{
    uint resolution;
    float spacingMeters;
    float seaLevelMeters;
    float referenceDrainageAreaSquareMeters;
    float minimumDrainageAreaSquareMeters;
    float baseChannelHalfWidthMeters;
    float minimumChannelHalfWidthMeters;
    float maximumChannelHalfWidthMeters;
    float widthExponent;
    float baseDepthMeters;
    float minimumDepthMeters;
    float maximumDepthMeters;
    float depthExponent;
    float maximumIncisionMeters;
};

[[vk::push_constant]] PushConstants g_pc;

static const uint kOutputStrideBytes = 16u;

[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
    if (dispatchId.x >= g_pc.resolution || dispatchId.y >= g_pc.resolution)
    {
        return;
    }

    const uint index = dispatchId.y * g_pc.resolution + dispatchId.x;

    const float drainageElevation = asfloat(g_drainage.Load(index * 4u));
    const float accumulation = asfloat(g_accumulation.Load(index * 4u));

    const float cellArea = g_pc.spacingMeters * g_pc.spacingMeters;
    const float drainageArea = max(accumulation * cellArea, 0.0);

    const float areaRatio = max(
        drainageArea / g_pc.referenceDrainageAreaSquareMeters, 1.0e-6);

    const float halfWidth = clamp(
        g_pc.baseChannelHalfWidthMeters *
            pow(areaRatio, g_pc.widthExponent),
        g_pc.minimumChannelHalfWidthMeters,
        g_pc.maximumChannelHalfWidthMeters);

    const float depth = clamp(
        g_pc.baseDepthMeters * pow(areaRatio, g_pc.depthExponent),
        g_pc.minimumDepthMeters,
        g_pc.maximumDepthMeters);

    const float bedElevation = max(
        drainageElevation - depth,
        drainageElevation - g_pc.maximumIncisionMeters);

    const bool isOcean = drainageElevation <= g_pc.seaLevelMeters;

    const bool isRiver =
        !isOcean && drainageArea >= g_pc.minimumDrainageAreaSquareMeters;

    const uint byteOffset = index * kOutputStrideBytes;

    g_output.Store(byteOffset + 0u, asuint(halfWidth));
    g_output.Store(byteOffset + 4u, asuint(depth));
    g_output.Store(byteOffset + 8u, asuint(bedElevation));
    g_output.Store(byteOffset + 12u, isRiver ? 1u : 0u);
}
)";
} // namespace orbit::terrain_gpu::detail
