#pragma once
namespace orbit::terrain_gpu::detail
{
// GPU connected-component labeling for lake basins -- a "lake cell" is
// one GpuDepressionFill raised significantly above its raw elevation
// (a cell that got flooded to open a drainage path is, physically, a
// lake). Labels propagate via iterative min-label relaxation (each
// lake cell adopts the smallest label among itself and its lake
// neighbors) instead of engine/terrain_water's CPU LakeWater.cpp's
// BFS/flood-fill component grow -- same end result (every connected
// blob of lake cells ends up sharing one label, distinct blobs get
// distinct labels), reached by the GPU-parallel route the other
// Milestone 4 passes use (see the GPU terrain generation plan).
inline constexpr const char* kLakeBasinInitComputeShader = R"(
[[vk::binding(0, 0)]]
ByteAddressBuffer g_rawElevation : register(t0);

[[vk::binding(1, 0)]]
ByteAddressBuffer g_drainage : register(t1);

[[vk::binding(2, 0)]]
RWByteAddressBuffer g_labelOut : register(u2);

struct PushConstants
{
    uint resolution;
    float fillThresholdMeters;
};

[[vk::push_constant]] PushConstants g_pc;

static const uint kNotLake = 0xFFFFFFFFu;

[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
    if (dispatchId.x >= g_pc.resolution || dispatchId.y >= g_pc.resolution)
    {
        return;
    }

    const uint index = dispatchId.y * g_pc.resolution + dispatchId.x;

    const float raw = asfloat(g_rawElevation.Load(index * 4u));
    const float drainage = asfloat(g_drainage.Load(index * 4u));

    const bool isLake = (drainage - raw) > g_pc.fillThresholdMeters;

    g_labelOut.Store(index * 4u, isLake ? index : kNotLake);
}
)";

inline constexpr const char* kLakeBasinPropagateComputeShader = R"(
[[vk::binding(0, 0)]]
ByteAddressBuffer g_labelIn : register(t0);

[[vk::binding(1, 0)]]
RWByteAddressBuffer g_labelOut : register(u1);

struct PushConstants
{
    uint resolution;
};

[[vk::push_constant]] PushConstants g_pc;

static const uint kNotLake = 0xFFFFFFFFu;

[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
    if (dispatchId.x >= g_pc.resolution || dispatchId.y >= g_pc.resolution)
    {
        return;
    }

    const uint index = dispatchId.y * g_pc.resolution + dispatchId.x;
    const uint myLabel = g_labelIn.Load(index * 4u);

    if (myLabel == kNotLake)
    {
        g_labelOut.Store(index * 4u, kNotLake);
        return;
    }

    uint best = myLabel;

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

            const uint neighborLabel = g_labelIn.Load(neighborIndex * 4u);

            if (neighborLabel != kNotLake && neighborLabel < best)
            {
                best = neighborLabel;
            }
        }
    }

    g_labelOut.Store(index * 4u, best);
}
)";
} // namespace orbit::terrain_gpu::detail
