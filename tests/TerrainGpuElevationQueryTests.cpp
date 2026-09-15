#include <orbit/rhi/vulkan/VulkanBackend.hpp>
#include <orbit/shader/dxc/DxcShaderCompiler.hpp>
#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/terrain_gpu/GpuElevationQuery.hpp>
#include <orbit/terrain_gpu/GpuFieldGenerator.hpp>

#include <cmath>
#include <cstdio>
#include <iostream>

namespace
{
using namespace orbit;
} // namespace

// Milestone 3+4 integration check: GpuElevationQuery now runs field
// generation, depression fill, and erosion per query (not just the raw
// field) -- this exercises that whole per-slot dispatch sequence end to
// end, including every resource-state transition between the four GPU
// passes chained inside it, which unit tests of the individual passes
// can't catch (each of those tests owns and manages its own buffer
// states directly).
int main()
{
    const world::PlanetDefinition planet{.radiusMeters = 6'000'000.0};
    const terrain::AnalyticTerrainDesc desc{.seed = 0xA57E22AULL};
    const terrain::AnalyticTerrainSource source(planet, desc);

    const auto device = rhi::vulkan::CreateDevice({.enableValidation = true});
    const shader::dxc::DxcShaderCompiler compiler;

    terrain_gpu::GpuFieldGenerator generator(*device, compiler, planet, source);

    const auto graphicsQueue = device->CreateQueue(rhi::QueueType::Graphics);
    const auto allocator = device->CreateCommandAllocator(rhi::QueueType::Graphics);
    const auto commandList = device->CreateCommandList(*allocator);
    const auto fence = device->CreateFence(0);
    u64 nextFenceValue = 1;

    terrain_gpu::GpuElevationQuery elevationQuery(
        *device, compiler, generator, source.GlobalFields(), *fence);

    const math::Double3 direction = math::Normalize(
        math::Double3{0.365111510558, 0.187057495117, -0.911977564625});

    // Before any readback has completed, LastKnownGood must still
    // return something finite (the coarse analytic bootstrap).
    const f64 bootstrap = elevationQuery.LastKnownGood(direction);

    if (!std::isfinite(bootstrap))
    {
        std::cerr << "LastKnownGood returned a non-finite bootstrap "
                     "estimate before any readback completed.\n";
        return 1;
    }

    const terrain_gpu::ElevationQueryHandle handle =
        elevationQuery.Request(direction);

    // Drive several simulated frames -- Request/Flush/Submit/Signal --
    // until the readback resolves, mirroring how Main.cpp's render
    // loop drives this every frame.
    f64 resolved = 0.0;
    bool gotResult = false;

    for (u32 frame = 0; frame < 10 && !gotResult; ++frame)
    {
        allocator->Reset();
        commandList->Reset(*allocator);

        elevationQuery.Flush(*commandList, nextFenceValue);

        commandList->Close();
        graphicsQueue->Submit(*commandList);

        const u64 signalValue = nextFenceValue++;
        graphicsQueue->Signal(*fence, signalValue);
        fence->Wait(signalValue);

        gotResult = elevationQuery.TryGetResult(handle, resolved);
    }

    if (!gotResult)
    {
        std::cerr << "Elevation query never resolved within 10 "
                     "simulated frames.\n";
        return 1;
    }

    if (!std::isfinite(resolved))
    {
        std::cerr << "Resolved elevation is not finite: " << resolved
                  << "\n";
        return 1;
    }

    // Sanity bound: the resolved (eroded/lake-surfaced) elevation
    // should be in the same ballpark as the CPU reference at this
    // direction, not wildly different (a few hundred meters is a
    // generous allowance for a 10m-footprint local erosion estimate
    // versus the CPU's own, much coarser, footprint).
    const terrain::TerrainSample cpuSample =
        source.Sample({.unitDirection = direction, .footprintMeters = 10.0});

    const f64 delta = std::abs(resolved - cpuSample.elevationMeters);

    if (delta > 500.0)
    {
        std::cerr << "Resolved elevation " << resolved
                  << " is implausibly far from the CPU reference "
                  << cpuSample.elevationMeters << " (delta=" << delta
                  << ")\n";
        return 1;
    }

    // LastKnownGood at the same direction should now return the
    // resolved value (or something close -- the cache stores exactly
    // this), not the coarse bootstrap.
    const f64 cached = elevationQuery.LastKnownGood(direction);

    if (std::abs(cached - resolved) > 1.0e-3)
    {
        std::cerr << "LastKnownGood after resolution (" << cached
                  << ") doesn't match the resolved result (" << resolved
                  << ")\n";
        return 1;
    }

    std::printf(
        "GpuElevationQuery: bootstrap=%.2f m, resolved=%.2f m "
        "(CPU reference=%.2f m, delta=%.2f m)\n",
        bootstrap,
        resolved,
        cpuSample.elevationMeters,
        delta);

    return 0;
}
