#include <orbit/rhi/vulkan/VulkanBackend.hpp>
#include <orbit/shader/dxc/DxcShaderCompiler.hpp>
#include <orbit/terrain_gpu/GpuDepressionFill.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

namespace
{
using namespace orbit;

// Milestone 4 verification (see the GPU terrain generation plan):
// GpuDepressionFill uses a different, GPU-parallel algorithm than
// HydrologyGrid.cpp's CPU priority-flood ConditionDepressions --
// bit-identical output is explicitly not expected (see
// HydrologyRelaxCompute.hpp). What must hold instead: raw elevation is
// never lowered, boundary/outlet cells are left untouched, every
// interior sink is raised enough that a monotonically non-increasing
// downhill path to an outlet actually exists (the whole point of
// conditioning), and depressions farther from any outlet still resolve
// correctly.
} // namespace

int main()
{
    const auto device = rhi::vulkan::CreateDevice({.enableValidation = true});
    const shader::dxc::DxcShaderCompiler compiler;

    constexpr u32 kResolution = 9;
    constexpr f32 kSeaLevelMeters = -1'000.0F;
    // Naive Jacobi relaxation can take passes proportional to (local
    // elevation range) / minimumDropMeters to break out of a plateau
    // where a cluster of cells only see each other as their cheapest
    // neighbor before discovering a real, cheaper route (see
    // GpuDepressionFill.cpp's iteration-count comment) -- too fine an
    // epsilon relative to this scenario's ~45m gap-to-wall difference
    // would need far more iterations than is worth spending here.
    constexpr f32 kMinimumDropMeters = 0.5F;

    terrain_gpu::GpuDepressionFill depressionFill(
        *device, compiler, kResolution);

    // A genuinely trapped pit: the grid boundary is a low outlet (0),
    // a Chebyshev-square wall at distance 2 from the center sits well
    // above it (50) with a single gap cell lower than the wall but
    // still above the interior (10), an open moat between the wall and
    // the boundary and immediately around the pit (5), and the pit
    // itself (the single center cell) far below everything (-100).
    // Escaping requires the fill to raise the pit (and the moat cells
    // between it and the gap) up to the gap's level -- there is no
    // path out at the raw elevations at all.
    std::vector<f32> rawElevation(
        static_cast<std::size_t>(kResolution) * kResolution);

    const auto index = [&](const u32 x, const u32 y)
    {
        return static_cast<std::size_t>(y) * kResolution + x;
    };

    constexpr u32 kCenter = 4;
    constexpr u32 kGapX = 4;
    constexpr u32 kGapY = 2;

    for (u32 y = 0; y < kResolution; ++y)
    {
        for (u32 x = 0; x < kResolution; ++x)
        {
            const i32 dx = static_cast<i32>(x) - static_cast<i32>(kCenter);
            const i32 dy = static_cast<i32>(y) - static_cast<i32>(kCenter);
            const u32 chebyshev = static_cast<u32>(
                std::max(std::abs(dx), std::abs(dy)));

            f32 elevation = 5.0F;

            if (x == 0 || y == 0 ||
                x == kResolution - 1 || y == kResolution - 1)
            {
                elevation = 0.0F;
            }
            else if (chebyshev == 2)
            {
                elevation = (x == kGapX && y == kGapY) ? 10.0F : 50.0F;
            }
            else if (chebyshev == 0)
            {
                elevation = -100.0F;
            }

            rawElevation[index(x, y)] = elevation;
        }
    }

    const u64 bufferBytes =
        static_cast<u64>(kResolution) * kResolution * sizeof(f32);

    const auto rawBuffer = device->CreateBuffer({
        .sizeBytes = bufferBytes,
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::ShaderResource
    });

    {
        std::byte* mapped = rawBuffer->Map();
        std::memcpy(mapped, rawElevation.data(), bufferBytes);
        rawBuffer->Unmap();
    }

    const auto drainageBuffer = device->CreateBuffer({
        .sizeBytes = bufferBytes,
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::GpuOnly,
        .initialState = rhi::ResourceState::CopyDestination
    });

    const auto readback = device->CreateBuffer({
        .sizeBytes = bufferBytes,
        .usage = rhi::BufferUsage::Generic,
        .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::CopyDestination
    });

    const auto queue = device->CreateQueue(rhi::QueueType::Graphics);
    const auto allocator = device->CreateCommandAllocator(rhi::QueueType::Graphics);
    const auto commandList = device->CreateCommandList(*allocator);
    const auto fence = device->CreateFence(0);

    allocator->Reset();
    commandList->Reset(*allocator);

    depressionFill.Dispatch(
        *commandList,
        kResolution,
        kMinimumDropMeters,
        kSeaLevelMeters,
        *rawBuffer,
        *drainageBuffer);

    commandList->Transition(
        *drainageBuffer,
        rhi::ResourceState::CopyDestination,
        rhi::ResourceState::CopySource);

    commandList->CopyBuffer(*drainageBuffer, 0, *readback, 0, bufferBytes);

    commandList->Close();
    queue->Submit(*commandList);
    queue->Signal(*fence, 1);
    fence->Wait(1);

    std::vector<f32> drainage(
        static_cast<std::size_t>(kResolution) * kResolution);

    const std::byte* mapped = readback->Map();
    std::memcpy(drainage.data(), mapped, bufferBytes);
    readback->Unmap();

    // 1. Boundary cells are untouched outlets.
    for (u32 x = 0; x < kResolution; ++x)
    {
        for (const u32 y : {0U, kResolution - 1U})
        {
            if (drainage[index(x, y)] != rawElevation[index(x, y)])
            {
                std::cerr << "Boundary cell (" << x << "," << y
                          << ") was modified: raw="
                          << rawElevation[index(x, y)] << " drainage="
                          << drainage[index(x, y)] << "\n";
                return 1;
            }
        }
    }

    // 2. No cell was ever lowered below its raw elevation.
    for (std::size_t i = 0; i < drainage.size(); ++i)
    {
        if (drainage[i] < rawElevation[i] - 1.0e-3F)
        {
            std::cerr << "Cell " << i << " was lowered: raw="
                      << rawElevation[i] << " drainage=" << drainage[i]
                      << "\n";
            return 1;
        }
    }

    // 3. The pit was actually raised, substantially -- it started 105m
    // below its immediate surroundings and completely walled in except
    // for one gap at 10m, so a correct fill must raise it close to
    // that gap's level, not leave it near its original -100.
    const f32 pitRaw = rawElevation[index(kCenter, kCenter)];
    const f32 pitFilled = drainage[index(kCenter, kCenter)];

    if (pitFilled < pitRaw + 50.0F)
    {
        std::cerr << "Pit was not meaningfully filled: raw=" << pitRaw
                   << " drainage=" << pitFilled << " (expected close to "
                      "the gap's 10m level, not near the original -100)"
                   << "\n";
        return 1;
    }

    // 4. The center (bottom of the pit) has a strictly non-increasing,
    // strictly-downhill-or-equal steepest-descent path out to a
    // boundary cell -- i.e. conditioning actually opened a route out,
    // not just raised the pit in place with no way out.
    u32 x = 4;
    u32 y = 4;
    f32 previousDrainage = drainage[index(x, y)];
    const f32 pitDrainage = previousDrainage;

    bool reachedBoundary = false;

    for (u32 step = 0; step < kResolution * kResolution; ++step)
    {
        if (x == 0 || y == 0 || x == kResolution - 1 ||
            y == kResolution - 1)
        {
            reachedBoundary = true;
            break;
        }

        u32 bestX = x;
        u32 bestY = y;
        f32 bestDrainage = drainage[index(x, y)];

        for (i32 dy = -1; dy <= 1; ++dy)
        {
            for (i32 dx = -1; dx <= 1; ++dx)
            {
                if (dx == 0 && dy == 0)
                {
                    continue;
                }

                const i32 nx = static_cast<i32>(x) + dx;
                const i32 ny = static_cast<i32>(y) + dy;

                if (nx < 0 || ny < 0 ||
                    nx >= static_cast<i32>(kResolution) ||
                    ny >= static_cast<i32>(kResolution))
                {
                    continue;
                }

                const f32 candidate =
                    drainage[index(
                        static_cast<u32>(nx), static_cast<u32>(ny))];

                if (candidate < bestDrainage)
                {
                    bestDrainage = candidate;
                    bestX = static_cast<u32>(nx);
                    bestY = static_cast<u32>(ny);
                }
            }
        }

        if (bestX == x && bestY == y)
        {
            // Stuck: every neighbor is at least as high as this cell,
            // and this isn't the boundary -- conditioning failed to
            // open a route.
            std::cerr << "Steepest-descent got stuck at (" << x << ","
                      << y << ") with drainage " << drainage[index(x, y)]
                      << ", not at the boundary.\n";
            return 1;
        }

        if (bestDrainage > previousDrainage + 1.0e-3F)
        {
            std::cerr << "Steepest-descent path is not non-increasing "
                         "at step "
                      << step << ": " << previousDrainage << " -> "
                      << bestDrainage << "\n";
            return 1;
        }

        previousDrainage = bestDrainage;
        x = bestX;
        y = bestY;
    }

    if (!reachedBoundary)
    {
        std::cerr << "Steepest-descent from the pit never reached the "
                     "boundary.\n";
        return 1;
    }

    std::printf(
        "Depression fill: pit raw=%.2f drainage=%.2f, downhill path "
        "reached the boundary at (%u,%u).\n",
        rawElevation[index(4, 4)], pitDrainage, x, y);

    return 0;
}
