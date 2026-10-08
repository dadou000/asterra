#include <orbit/rhi/vulkan/VulkanBackend.hpp>
#include <orbit/shader/dxc/DxcShaderCompiler.hpp>
#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/terrain/BakedRivers.hpp>
#include <orbit/terrain_bake/TectonicBaker.hpp>
#include <orbit/terrain_gpu/GpuFieldGenerator.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

namespace
{
using namespace orbit;

struct SampleRecord
{
    f32 elevationMeters;
    f32 morphTargetXMeters;
    f32 morphTargetYMeters;
    u32 biomeWeights0;
    u32 biomeWeights1;
    f32 standingWaterDepthMeters;
    f32 fineSlopeEast;
    f32 fineSlopeNorth;
};
static_assert(sizeof(SampleRecord) == 32);

// Milestone 2 verification (see the GPU terrain generation plan): proves
// GpuFieldGenerator's compute shader actually compiles via DXC, runs
// without validation errors, and produces elevation close to
// AnalyticTerrainSource::Sample's CPU reference at the same logical grid
// points -- a tolerance comparison, not exact, since noise-domain
// coordinates go through float32 instead of float64 (see the precision
// notes in FieldGenerationCompute.hpp), but the underlying 64-bit hash
// lattice is bit-exact, so the tolerance should be small, not "different
// noise field" large.
} // namespace

// 0: procedural plates, 1: baked tectonics, 2: baked tectonics + a baked river.
int RunCase(const int mode, f64* const maxDeltaOut)
{
    const bool baked = mode >= 1;
    const world::PlanetDefinition planet{.radiusMeters = 6'000'000.0};
    terrain::AnalyticTerrainDesc desc{.seed = 0xA57E22AULL};
    if (baked)
    {
        // Same field, but plate fields come from baked rasters on both the
        // CPU reference and the GPU generator.
        desc.global.bakedTectonics =
            terrain_bake::BakeTectonics(planet, desc, {.resolution = 128});
        if (desc.global.bakedTectonics == nullptr)
        {
            std::cerr << "The tectonic bake did not complete.\n";
            return 1;
        }
    }
    // A surveyed lowland (not a hard-coded spot): the carve only cuts land, and
    // where land sits depends on the tectonic geography.
    const world::SurfaceFrame frame = world::MakeSurfaceFrame([&]
    {
        const terrain::AnalyticTerrainSource survey(planet, desc);
        math::Double3 best{0.365111510558, 0.187057495117, -0.911977564625};
        f64 bestScore = 1.0e30;
        constexpr u32 count = 6000;
        for (u32 i = 0; i < count; ++i)
        {
            const f64 y = 1.0 - 2.0 * (static_cast<f64>(i) + 0.5) / count;
            const f64 r = std::sqrt(std::max(0.0, 1.0 - y * y));
            const f64 a = 2.399963229728653 * static_cast<f64>(i);
            const math::Double3 d{r * std::cos(a), y, r * std::sin(a)};
            const f64 e = survey.Sample({.unitDirection = d, .footprintMeters = 50.0}).elevationMeters;
            const f64 score = std::abs(e - 900.0);
            if (score < bestScore)
            {
                bestScore = score;
                best = d;
            }
        }
        return math::Normalize(best);
    }());
    f64 riverCentreCarve = 0.0;
    if (mode == 2)
    {
        // A 400 m wide, 60 m deep channel running north-south just east of the
        // survey centre, long enough that every grid point sees the same reach.
        const auto at = [&](const f64 northMeters, const f64 eastMeters)
        {
            return math::Normalize(
                frame.up + frame.north * (northMeters / planet.radiusMeters) +
                frame.east * (eastMeters / planet.radiusMeters));
        };
        std::vector<terrain::BakedRiverNode> nodes(2);
        nodes[0].direction = at(-3000.0, 100.0);
        nodes[1].direction = at(3000.0, 100.0);
        for (auto& node : nodes)
        {
            node.widthMeters = 400.0F;
            node.depthMeters = 60.0F;
            node.dischargeCubicMetersPerSecond = 5000.0F;
        }
        desc.bakedRivers = std::make_shared<const terrain::BakedRiverNetwork>(
            terrain::BakedRiverNetwork::Build(planet.radiusMeters, 1, nodes, {{0, 1}}));
        const terrain::AnalyticTerrainSource dry(planet, [&] { auto d = desc; d.bakedRivers.reset(); return d; }());
        const terrain::AnalyticTerrainSource wet(planet, desc);
        const math::Double3 centre = at(0.0, 100.0);
        riverCentreCarve = dry.Sample({.unitDirection = centre, .footprintMeters = 50.0}).elevationMeters -
            wet.Sample({.unitDirection = centre, .footprintMeters = 50.0}).elevationMeters;
        std::printf("CPU river carve at the channel centre: %.2f m\n", riverCentreCarve);
        if (riverCentreCarve < 10.0)
        {
            std::cerr << "The test channel must visibly cut the terrain on the CPU.\n";
            return 1;
        }
    }
    const terrain::AnalyticTerrainSource source(planet, desc);

    const auto device = rhi::vulkan::CreateDevice({.enableValidation = true});
    const shader::dxc::DxcShaderCompiler compiler;

    terrain_gpu::GpuFieldGenerator generator(*device, compiler, planet, source);

    // A modest, mid-detail clipmap-like level: 17x17 logical grid, 50m
    // spacing/footprint, centered near the sandbox's own default spawn
    // direction so the survey lands somewhere with real relief. No
    // morph (morphToCoarser = false) -- keeps this first real test to
    // the core noise/tectonics/climate/biome path.
    constexpr u32 kResolution = 17;
    terrain_gpu::GpuFieldRequest request{};
    request.resolution = kResolution;
    request.spacingMeters = 50.0;
    request.footprintMeters = 50.0;
    request.morphToCoarser = false;
    request.fineNormalFootprintMeters = 50.0;
    request.fineNormalEpsilonMeters = 50.0;
    request.surfaceFrame = frame;
    request.coarseSurfaceFrame = frame;
    request.originX = 0;
    request.originY = 0;
    request.region = {.x = 0, .y = 0, .width = kResolution, .height = kResolution};

    const u64 sampleCount = static_cast<u64>(kResolution) * kResolution;
    const u64 bufferBytes = sampleCount * sizeof(SampleRecord);

    const auto gpuBuffer = device->CreateBuffer({
        .sizeBytes = bufferBytes,
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::GpuOnly,
        .initialState = rhi::ResourceState::Common
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

    commandList->Transition(
        *gpuBuffer, rhi::ResourceState::Common, rhi::ResourceState::UnorderedAccess);

    generator.Dispatch(*commandList, request, *gpuBuffer);

    commandList->Transition(
        *gpuBuffer, rhi::ResourceState::UnorderedAccess, rhi::ResourceState::CopySource);
    commandList->CopyBuffer(*gpuBuffer, 0, *readback, 0, bufferBytes);

    commandList->Close();
    queue->Submit(*commandList);
    queue->Signal(*fence, 1);
    fence->Wait(1);

    std::vector<SampleRecord> records(sampleCount);
    const std::byte* mapped = readback->Map();
    std::memcpy(records.data(), mapped, bufferBytes);
    readback->Unmap();

    // Compare against the CPU reference at every logical grid point.
    const f64 halfCells = (static_cast<f64>(kResolution) - 1.0) * 0.5;

    f64 maxAbsElevationDelta = 0.0;
    f64 sumAbsElevationDelta = 0.0;
    u32 comparedCount = 0;

    for (u32 y = 0; y < kResolution; ++y)
    {
        for (u32 x = 0; x < kResolution; ++x)
        {
            const math::Double2 offsetMeters{
                (static_cast<f64>(x) - halfCells) * request.spacingMeters,
                (static_cast<f64>(y) - halfCells) * request.spacingMeters
            };
            const math::Double3 direction =
                world::DirectionAtSurfaceOffset(planet, frame, offsetMeters);

            const terrain::TerrainSample cpuSample = source.Sample(
                {.unitDirection = direction, .footprintMeters = request.footprintMeters});

            const SampleRecord& gpuRecord = records[static_cast<std::size_t>(y) * kResolution + x];

            if (!std::isfinite(gpuRecord.elevationMeters))
            {
                std::cerr << "GPU field generator produced a non-finite elevation at ("
                          << x << ", " << y << ").\n";
                return 1;
            }

            const f64 delta = std::abs(
                static_cast<f64>(gpuRecord.elevationMeters) - cpuSample.elevationMeters);
            maxAbsElevationDelta = std::max(maxAbsElevationDelta, delta);
            sumAbsElevationDelta += delta;
            ++comparedCount;
        }
    }

    const f64 meanAbsElevationDelta = sumAbsElevationDelta / comparedCount;

    std::printf(
        "GPU vs CPU elevation (%s tectonics): mean |delta| = %.3f m, max |delta| = %.3f m over %u points\n",
        mode == 2 ? "baked tectonics + river" : (baked ? "baked" : "procedural"), meanAbsElevationDelta, maxAbsElevationDelta, comparedCount);

    // The underlying 64-bit hash lattice is bit-exact (see
    // FieldGenerationCompute.hpp); the only source of divergence is
    // float32 vs float64 noise-domain arithmetic (interpolation
    // fractions, not which lattice cell), so this should read as small
    // numerical jitter, not "a different terrain". A loose-but-meaningful
    // bound: within 5% of the configured elevation ceiling, or 50m,
    // whichever is larger.
    const f64 toleranceMeters = std::max(
        desc.maximumElevationAboveSeaLevelMeters * 0.05, 50.0);

    if (maxAbsElevationDelta > toleranceMeters)
    {
        std::cerr << "GPU elevation diverged too far from the CPU reference (max |delta| = "
                  << maxAbsElevationDelta << " m > tolerance " << toleranceMeters << " m).\n";
        return 1;
    }

    if (maxDeltaOut != nullptr)
    {
        *maxDeltaOut = maxAbsElevationDelta;
    }
    return 0;
}

int main()
{
    f64 bakedDelta = 0.0;
    f64 riverDelta = 0.0;
    if (const int procedural = RunCase(0, nullptr); procedural != 0)
    {
        return procedural;
    }
    if (const int baked = RunCase(1, &bakedDelta); baked != 0)
    {
        return baked;
    }
    if (const int rivers = RunCase(2, &riverDelta); rivers != 0)
    {
        return rivers;
    }
    // The GPU must cut the same channel as the CPU: a missing or misplaced
    // carve would add metres on top of the baked-case noise jitter.
    if (riverDelta > bakedDelta + 1.0)
    {
        std::cerr << "GPU river carve diverges from the CPU (" << riverDelta
                  << " m vs " << bakedDelta << " m without a river).\n";
        return 1;
    }
    return 0;
}
