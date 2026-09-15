#include <orbit/rhi/vulkan/VulkanBackend.hpp>
#include <orbit/shader/dxc/DxcShaderCompiler.hpp>
#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/terrain_gpu/GpuFieldGenerator.hpp>
#include <orbit/terrain_gpu/GpuHydrologyRegion.hpp>
#include <orbit/terrain_region/DerivedTerrainRegionGpu.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

namespace
{
using namespace orbit;

template <typename T>
[[nodiscard]] std::unique_ptr<rhi::Buffer> CreateReadbackBuffer(
    rhi::Device& device,
    const u64 elementCount)
{
    return device.CreateBuffer({
        .sizeBytes = elementCount * sizeof(T),
        .usage = rhi::BufferUsage::Generic,
        .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::CopyDestination
    });
}
} // namespace

// End-to-end proof for the last piece of the GPU terrain generation
// plan's Milestone 4: a GPU-computed hydrology region readback feeding
// the *unchanged* CPU BuildRiverGraph/BuildRegionalElevationDeltaField/
// BuildRiverCarvingField/BuildRiverWaterNetwork/BuildLakeWaterField
// pipeline (see DerivedTerrainRegionGpu.cpp) produces a well-formed
// DerivedTerrainRegion, the same shape TerrainPreviewRenderer/
// RiverWaterRenderer/the 2D map already consume today from the CPU
// path.
int main()
{
    const world::PlanetDefinition planet{.radiusMeters = 6'000'000.0};
    const terrain::AnalyticTerrainDesc desc{.seed = 0xA57E22AULL};
    const terrain::AnalyticTerrainSource source(planet, desc);

    const auto device = rhi::vulkan::CreateDevice({.enableValidation = true});
    const shader::dxc::DxcShaderCompiler compiler;

    terrain_gpu::GpuFieldGenerator generator(*device, compiler, planet, source);

    constexpr u32 kResolution = 65;
    terrain_gpu::GpuHydrologyRegion hydrologyRegion(
        *device, compiler, generator, kResolution);

    const math::Double3 direction = math::Normalize(
        math::Double3{-0.119446064616, -0.964767868815, 0.234426101257});

    const world::SurfaceFrame frame = world::MakeSurfaceFrame(direction);

    constexpr f64 kHalfExtentMeters = 8'000.0;
    const f64 spacingMeters =
        kHalfExtentMeters * 2.0 / static_cast<f64>(kResolution - 1);

    terrain_gpu::GpuHydrologyRegionRequest request{};
    request.resolution = kResolution;
    request.spacingMeters = spacingMeters;
    request.footprintMeters = spacingMeters;
    request.surfaceFrame = frame;
    request.seaLevelMeters = 0.0F;
    request.minimumDropMeters = 0.25F;

    const u64 floatBytes =
        static_cast<u64>(kResolution) * kResolution * sizeof(f32);
    const u64 uintBytes =
        static_cast<u64>(kResolution) * kResolution * sizeof(u32);

    const auto rawBuffer = device->CreateBuffer({
        .sizeBytes = floatBytes, .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::GpuOnly,
        .initialState = rhi::ResourceState::CopyDestination
    });
    const auto drainageBuffer = device->CreateBuffer({
        .sizeBytes = floatBytes, .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::GpuOnly,
        .initialState = rhi::ResourceState::CopyDestination
    });
    const auto accumulationBuffer = device->CreateBuffer({
        .sizeBytes = floatBytes, .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::GpuOnly,
        .initialState = rhi::ResourceState::CopyDestination
    });
    const auto downstreamBuffer = device->CreateBuffer({
        .sizeBytes = uintBytes, .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::GpuOnly,
        .initialState = rhi::ResourceState::CopyDestination
    });
    const auto netDeltaBuffer = device->CreateBuffer({
        .sizeBytes = floatBytes, .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::GpuOnly,
        .initialState = rhi::ResourceState::CopyDestination
    });

    const auto rawReadback =
        CreateReadbackBuffer<f32>(*device, kResolution * kResolution);
    const auto drainageReadback =
        CreateReadbackBuffer<f32>(*device, kResolution * kResolution);
    const auto accumulationReadback =
        CreateReadbackBuffer<f32>(*device, kResolution * kResolution);
    const auto downstreamReadback =
        CreateReadbackBuffer<u32>(*device, kResolution * kResolution);
    const auto netDeltaReadback =
        CreateReadbackBuffer<f32>(*device, kResolution * kResolution);

    const auto queue = device->CreateQueue(rhi::QueueType::Graphics);
    const auto allocator = device->CreateCommandAllocator(rhi::QueueType::Graphics);
    const auto commandList = device->CreateCommandList(*allocator);
    const auto fence = device->CreateFence(0);

    allocator->Reset();
    commandList->Reset(*allocator);

    hydrologyRegion.Dispatch(
        *commandList,
        request,
        *rawBuffer,
        *drainageBuffer,
        *accumulationBuffer,
        *downstreamBuffer,
        *netDeltaBuffer);

    const auto CopyOut = [&](rhi::Buffer& source, rhi::Buffer& readback,
                              const u64 bytes)
    {
        commandList->Transition(
            source,
            rhi::ResourceState::CopyDestination,
            rhi::ResourceState::CopySource);
        commandList->CopyBuffer(source, 0, readback, 0, bytes);
    };

    CopyOut(*rawBuffer, *rawReadback, floatBytes);
    CopyOut(*drainageBuffer, *drainageReadback, floatBytes);
    CopyOut(*accumulationBuffer, *accumulationReadback, floatBytes);
    CopyOut(*downstreamBuffer, *downstreamReadback, uintBytes);
    CopyOut(*netDeltaBuffer, *netDeltaReadback, floatBytes);

    commandList->Close();
    queue->Submit(*commandList);
    queue->Signal(*fence, 1);
    fence->Wait(1);

    const std::size_t cellCount =
        static_cast<std::size_t>(kResolution) * kResolution;

    std::vector<f32> raw(cellCount);
    std::vector<f32> drainage(cellCount);
    std::vector<f32> accumulation(cellCount);
    std::vector<u32> downstream(cellCount);
    std::vector<f32> netDelta(cellCount);

    const auto ReadInto = [](rhi::Buffer& buffer, void* dest, const u64 bytes)
    {
        const std::byte* mapped = buffer.Map();
        std::memcpy(dest, mapped, bytes);
        buffer.Unmap();
    };

    ReadInto(*rawReadback, raw.data(), floatBytes);
    ReadInto(*drainageReadback, drainage.data(), floatBytes);
    ReadInto(*accumulationReadback, accumulation.data(), floatBytes);
    ReadInto(*downstreamReadback, downstream.data(), uintBytes);
    ReadInto(*netDeltaReadback, netDelta.data(), floatBytes);

    for (std::size_t i = 0; i < cellCount; ++i)
    {
        if (!std::isfinite(raw[i]) || !std::isfinite(drainage[i]) ||
            !std::isfinite(accumulation[i]) || !std::isfinite(netDelta[i]))
        {
            std::cerr << "Non-finite GPU hydrology readback at cell " << i
                      << "\n";
            return 1;
        }

        if (drainage[i] < raw[i] - 1.0e-3F)
        {
            std::cerr << "Cell " << i
                      << " drainage is below raw elevation (raw="
                      << raw[i] << " drainage=" << drainage[i] << ")\n";
            return 1;
        }
    }

    terrain_region::GpuHydrologyReadback readback{};
    readback.resolution = kResolution;
    readback.spacingMeters = spacingMeters;
    readback.seaLevelMeters = request.seaLevelMeters;
    readback.surfaceFrame = frame;
    readback.rawElevationMeters = raw;
    readback.drainageElevationMeters = drainage;
    readback.accumulation = accumulation;
    readback.downstream = downstream;
    readback.netElevationDeltaMeters = netDelta;

    terrain_region::DerivedTerrainRegionConfig config{};
    config.hydrology.resolution = kResolution;
    config.minimumRiverDrainageAreaSquareMeters = 20'000.0;

    const terrain_region::DerivedTerrainRegionId id{
        .tile = {}, .sourceRevision = source.Revision(),
        .generatorVersion = config.generatorVersion
    };

    const terrain_region::DerivedTerrainRegion region =
        terrain_region::BuildDerivedTerrainRegionFromGpuReadback(
            id,
            /*approximateTileWidthMeters=*/kHalfExtentMeters * 2.0,
            kHalfExtentMeters,
            readback,
            config);

    if (region.hydrology.cells.size() != cellCount)
    {
        std::cerr << "Resulting region's hydrology grid has the wrong "
                     "cell count.\n";
        return 1;
    }

    if (region.elevationDelta.elevationDeltaMeters.size() != cellCount)
    {
        std::cerr << "Resulting region's elevation delta field has the "
                     "wrong cell count.\n";
        return 1;
    }

    // Every river node's drainage area should genuinely clear the
    // configured threshold -- proves BuildRiverGraph actually consumed
    // the GPU-populated flowAccumulation field, not zeros.
    for (const auto& node : region.rivers.nodes)
    {
        if (node.drainageAreaSquareMeters <
            config.minimumRiverDrainageAreaSquareMeters)
        {
            std::cerr << "River node has drainage area below the "
                         "configured minimum.\n";
            return 1;
        }
    }

    std::printf(
        "DerivedTerrainRegion from GPU readback: %zu hydrology cells, "
        "%zu river nodes, %zu river segments, %zu lake cells.\n",
        region.hydrology.cells.size(),
        region.rivers.nodes.size(),
        region.rivers.segments.size(),
        region.lakes.cells.size());

    return 0;
}
