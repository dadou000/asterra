#pragma once
namespace orbit::terrain_gpu::detail
{
// Steepest-descent downstream pointer: one pass, fully parallel. Reads
// the *conditioned* drainage field (GpuDepressionFill's output, not raw
// elevation) so every non-outlet cell is guaranteed a strictly lower
// neighbor to point to -- outlets (grid boundary) get the sentinel
// 0xFFFFFFFF, meaning "flow leaves the grid here".
inline constexpr const char* kFlowDownstreamComputeShader = R"(
[[vk::binding(0, 0)]]
ByteAddressBuffer g_drainage : register(t0);

[[vk::binding(1, 0)]]
RWByteAddressBuffer g_downstream : register(u1);

struct PushConstants
{
    uint resolution;
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

    const bool isOutlet =
        dispatchId.x == 0u ||
        dispatchId.y == 0u ||
        dispatchId.x == g_pc.resolution - 1u ||
        dispatchId.y == g_pc.resolution - 1u;

    if (isOutlet)
    {
        g_downstream.Store(index * 4u, kNoDownstream);
        return;
    }

    const float myDrainage = asfloat(g_drainage.Load(index * 4u));

    float bestDrainage = myDrainage;
    uint bestIndex = kNoDownstream;

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

            const float neighborDrainage =
                asfloat(g_drainage.Load(neighborIndex * 4u));

            if (neighborDrainage < bestDrainage)
            {
                bestDrainage = neighborDrainage;
                bestIndex = neighborIndex;
            }
        }
    }

    g_downstream.Store(index * 4u, bestIndex);
}
)";

// Flow accumulation: accum[cell] = runoff[cell] + sum(accum[n] for
// every neighbor n whose downstream pointer is this cell). Iterated
// with accum initialized to runoff everywhere, this is a Jacobi
// relaxation of a nilpotent system on the acyclic forest downstream[]
// describes (conditioned drainage is strictly decreasing along every
// edge, so there are no cycles) -- it reaches the *exact* fixed point
// after as many passes as the longest downstream chain in the grid,
// no atomics or scatter writes needed (see the GPU terrain generation
// plan's Milestone 4).
inline constexpr const char* kFlowAccumulationComputeShader = R"(
[[vk::binding(0, 0)]]
ByteAddressBuffer g_downstream : register(t0);

[[vk::binding(1, 0)]]
ByteAddressBuffer g_runoff : register(t1);

[[vk::binding(2, 0)]]
ByteAddressBuffer g_accumIn : register(t2);

[[vk::binding(3, 0)]]
RWByteAddressBuffer g_accumOut : register(u3);

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

    float sum = asfloat(g_runoff.Load(index * 4u));

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

            const uint neighborDownstream =
                g_downstream.Load(neighborIndex * 4u);

            if (neighborDownstream == index)
            {
                sum += asfloat(g_accumIn.Load(neighborIndex * 4u));
            }
        }
    }

    g_accumOut.Store(index * 4u, asuint(sum));
}
)";
} // namespace orbit::terrain_gpu::detail
