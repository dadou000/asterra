#pragma once
namespace orbit::terrain_gpu::detail
{
// GPU-only depression-filling relaxation -- NOT a port of
// HydrologyGrid.cpp's CPU ConditionDepressions (a single-pass priority
// flood via a min-heap, O(N log N), one visit per cell). That algorithm
// is inherently sequential (each pop depends on every earlier pop's
// result) and has no efficient parallel form. This is a different,
// GPU-native algorithm that converges to the *same kind* of result
// (every interior cell raised just enough that a strictly-downhill path
// to an outlet exists) via bounded Jacobi relaxation instead:
//
//   drainage[cell] = raw[cell]                          if cell is an
//                                                        outlet (grid
//                                                        boundary or at
//                                                        /below sea
//                                                        level)
//   drainage[cell] = max(raw[cell],
//                        min_over_8_neighbors(drainage[neighbor])
//                            + minimumDropMeters * distanceScale)
//                                                        otherwise
//
// Iterated with drainage initialized to raw[cell] everywhere, this is
// monotone non-decreasing every pass (already-computed neighbor
// drainage values never fall, so each cell's own floor never falls
// either) and converges to the same fixed point the CPU priority flood
// reaches, once enough passes have let the "raise" propagate from every
// outlet across the grid's full diameter -- see GpuDepressionFill.cpp's
// iteration count. This is the explicit, accepted CPU/GPU non-parity
// the GPU terrain generation plan's Milestone 4 calls out: close in
// shape, not bit-identical.
inline constexpr const char* kHydrologyRelaxComputeShader = R"(
[[vk::binding(0, 0)]]
ByteAddressBuffer g_rawElevation : register(t0);

[[vk::binding(1, 0)]]
ByteAddressBuffer g_drainageIn : register(t1);

[[vk::binding(2, 0)]]
RWByteAddressBuffer g_drainageOut : register(u2);

struct PushConstants
{
    uint resolution;
    float minimumDropMeters;
    float seaLevelMeters;
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
    const float raw = asfloat(g_rawElevation.Load(index * 4u));

    const bool isOutlet =
        dispatchId.x == 0u ||
        dispatchId.y == 0u ||
        dispatchId.x == g_pc.resolution - 1u ||
        dispatchId.y == g_pc.resolution - 1u ||
        raw <= g_pc.seaLevelMeters;

    if (isOutlet)
    {
        g_drainageOut.Store(index * 4u, asuint(raw));
        return;
    }

    float minNeighborFloor = 3.402823466e+38;

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
                asfloat(g_drainageIn.Load(neighborIndex * 4u));

            const float distanceScale =
                (dx != 0 && dy != 0) ? 1.41421356 : 1.0;

            const float floorFromNeighbor =
                neighborDrainage +
                g_pc.minimumDropMeters * distanceScale;

            minNeighborFloor = min(minNeighborFloor, floorFromNeighbor);
        }
    }

    const float conditioned = max(raw, minNeighborFloor);

    g_drainageOut.Store(index * 4u, asuint(conditioned));
}
)";
} // namespace orbit::terrain_gpu::detail
