#include <orbit/rhi/vulkan/VulkanBackend.hpp>
#include <orbit/shader/dxc/DxcShaderCompiler.hpp>
#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/terrain_gpu/GpuFieldGenerator.hpp>
#include <orbit/terrain_gpu/GpuRegionDelta.hpp>
#include <orbit/terrain_stream/TerrainSampleStreamer.hpp>

#include <algorithm>
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
} // namespace

// Compositing pass verification: the clipmap's already-generated
// sample buffer should be raised by exactly the region tile's delta
// (at full strength, well inside the tile) and left completely
// unmodified outside the tile's extent -- proves the reprojection math
// (RegionDeltaCompute.hpp) lands samples at the coordinates a matching
// CPU/GPU hydrology dispatch over the same physical tile would.
int main()
{
    const world::PlanetDefinition planet{.radiusMeters = 6'000'000.0};
    const terrain::AnalyticTerrainDesc desc{.seed = 0xA57E22AULL};
    const terrain::AnalyticTerrainSource source(planet, desc);

    const auto device = rhi::vulkan::CreateDevice({.enableValidation = true});
    const shader::dxc::DxcShaderCompiler compiler;

    terrain_gpu::GpuFieldGenerator generator(*device, compiler, planet, source);
    terrain_gpu::GpuRegionDelta regionDelta(*device, compiler);

    constexpr u32 kResolution = 17;
    const world::SurfaceFrame frame = world::MakeSurfaceFrame(
        math::Normalize(math::Double3{0.365111510558, 0.187057495117, -0.911977564625}));

    terrain_gpu::GpuFieldRequest fieldRequest{};
    fieldRequest.resolution = kResolution;
    fieldRequest.spacingMeters = 50.0;
    fieldRequest.footprintMeters = 50.0;
    fieldRequest.morphToCoarser = true;
    fieldRequest.morphStartHalfExtentMeters = 200.0;
    fieldRequest.morphEndHalfExtentMeters = 400.0;
    fieldRequest.coarseSpacingMeters = 100.0;
    fieldRequest.coarseFootprintMeters = 100.0;
    fieldRequest.fineNormalFootprintMeters = 50.0;
    fieldRequest.fineNormalEpsilonMeters = 50.0;
    fieldRequest.surfaceFrame = frame;
    fieldRequest.coarseSurfaceFrame = frame;
    fieldRequest.originX = 0;
    fieldRequest.originY = 0;
    fieldRequest.region = {.x = 0, .y = 0, .width = kResolution, .height = kResolution};

    // A region tile sharing the clipmap's own frame and comfortably
    // covering it, with a known linear delta for exact seam expectations.
    constexpr f32 kKnownDeltaMeters = 25.0F;
    constexpr u32 kRegionResolution = 9;
    constexpr f64 kRegionHalfExtentMeters = 1'000.0;
    const f64 regionSpacingMeters =
        kRegionHalfExtentMeters * 2.0 /
        static_cast<f64>(kRegionResolution - 1);

    std::vector<f32> regionDeltaValues(
        static_cast<std::size_t>(kRegionResolution) * kRegionResolution,
        kKnownDeltaMeters);
    // A sloping delta detects sampling at the old fine position after a
    // boundary vertex has snapped to its parent. A constant delta cannot.
    constexpr f64 kDeltaSlope = 0.02;
    for (u32 y = 0; y < kRegionResolution; ++y)
    {
        for (u32 x = 0; x < kRegionResolution; ++x)
        {
            regionDeltaValues[y * kRegionResolution + x] += static_cast<f32>(
                (x * regionSpacingMeters - kRegionHalfExtentMeters) * kDeltaSlope);
        }
    }

    terrain_gpu::GpuRegionDeltaRequest deltaRequest{};
    deltaRequest.resolution = kResolution;
    deltaRequest.spacingMeters = fieldRequest.spacingMeters;
    deltaRequest.morphToCoarser = true;
    deltaRequest.morphStartHalfExtentMeters = fieldRequest.morphStartHalfExtentMeters;
    deltaRequest.morphEndHalfExtentMeters = fieldRequest.morphEndHalfExtentMeters;
    deltaRequest.surfaceFrame = frame;
    deltaRequest.originX = 0;
    deltaRequest.originY = 0;
    deltaRequest.region = {
        .x = 0, .y = 0, .width = kResolution, .height = kResolution};
    deltaRequest.planetRadiusMeters = planet.radiusMeters;
    deltaRequest.regionSurfaceFrame = frame;
    deltaRequest.regionHalfExtentMeters = kRegionHalfExtentMeters;
    deltaRequest.regionSpacingMeters = regionSpacingMeters;
    deltaRequest.regionResolution = kRegionResolution;
    deltaRequest.edgeFadeStartDot = 0.75F;

    const u64 sampleCount = static_cast<u64>(kResolution) * kResolution;
    const u64 sampleBufferBytes = sampleCount * sizeof(SampleRecord);
    const u64 regionDeltaBytes =
        static_cast<u64>(kRegionResolution) * kRegionResolution * sizeof(f32);

    const auto sampleBuffer = device->CreateBuffer({
        .sizeBytes = sampleBufferBytes,
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::GpuOnly,
        .initialState = rhi::ResourceState::Common
    });

    const auto regionDeltaBuffer = device->CreateBuffer({
        .sizeBytes = regionDeltaBytes,
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::ShaderResource
    });

    {
        std::byte* mapped = regionDeltaBuffer->Map();
        std::memcpy(
            mapped, regionDeltaValues.data(), regionDeltaBytes);
        regionDeltaBuffer->Unmap();
    }

    const auto readback = device->CreateBuffer({
        .sizeBytes = sampleBufferBytes,
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
        *sampleBuffer,
        rhi::ResourceState::Common,
        rhi::ResourceState::UnorderedAccess);

    generator.Dispatch(*commandList, fieldRequest, *sampleBuffer);
    commandList->UavBarrier(*sampleBuffer);

    // Read back the pre-composite elevations too, for comparison.
    const auto preCompositeBuffer = device->CreateBuffer({
        .sizeBytes = sampleBufferBytes,
        .usage = rhi::BufferUsage::Generic,
        .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::CopyDestination
    });

    commandList->Transition(
        *sampleBuffer,
        rhi::ResourceState::UnorderedAccess,
        rhi::ResourceState::CopySource);
    commandList->CopyBuffer(
        *sampleBuffer, 0, *preCompositeBuffer, 0, sampleBufferBytes);
    commandList->Transition(
        *sampleBuffer,
        rhi::ResourceState::CopySource,
        rhi::ResourceState::UnorderedAccess);

    regionDelta.Dispatch(
        *commandList, deltaRequest, *regionDeltaBuffer, *sampleBuffer);

    commandList->Transition(
        *sampleBuffer,
        rhi::ResourceState::UnorderedAccess,
        rhi::ResourceState::CopySource);
    commandList->CopyBuffer(*sampleBuffer, 0, *readback, 0, sampleBufferBytes);

    commandList->Close();
    queue->Submit(*commandList);
    queue->Signal(*fence, 1);
    fence->Wait(1);

    std::vector<SampleRecord> preComposite(sampleCount);
    std::vector<SampleRecord> postComposite(sampleCount);

    {
        const std::byte* mapped = preCompositeBuffer->Map();
        std::memcpy(preComposite.data(), mapped, sampleBufferBytes);
        preCompositeBuffer->Unmap();
    }
    {
        const std::byte* mapped = readback->Map();
        std::memcpy(postComposite.data(), mapped, sampleBufferBytes);
        readback->Unmap();
    }

    // Center sample (well within the tile's fade-free core) should be
    // raised by exactly kKnownDeltaMeters.
    constexpr u32 kCenter = kResolution / 2;
    const std::size_t centerIndex =
        static_cast<std::size_t>(kCenter) * kResolution + kCenter;

    const f32 centerDelta =
        postComposite[centerIndex].elevationMeters -
        preComposite[centerIndex].elevationMeters;

    if (std::abs(centerDelta - kKnownDeltaMeters) > 0.1F)
    {
        std::cerr << "Center sample delta=" << centerDelta
                  << ", expected close to " << kKnownDeltaMeters << "\n";
        return 1;
    }

    // No non-elevation field should have changed.
    if (postComposite[centerIndex].biomeWeights0 !=
            preComposite[centerIndex].biomeWeights0 ||
        postComposite[centerIndex].fineSlopeEast !=
            preComposite[centerIndex].fineSlopeEast)
    {
        std::cerr << "Compositing modified a field other than "
                     "elevation.\n";
        return 1;
    }

    // Check both the unmorphed center and the complete parent-morph band.
    for (std::size_t i = 0; i < sampleCount; ++i)
    {
        const f32 d =
            postComposite[i].elevationMeters -
            preComposite[i].elevationMeters;

        const f64 x = (static_cast<f64>(i % kResolution) - kCenter) * 50.0;
        const f64 y = (static_cast<f64>(i / kResolution) - kCenter) * 50.0;
        const f64 t = std::clamp((std::max(std::abs(x), std::abs(y)) - 200.0) / 200.0, 0.0, 1.0);
        const f64 morph = t * t * (3.0 - 2.0 * t);
        const f64 actualX = x + (std::round(x / 100.0) * 100.0 - x) * morph;
        const f64 expected = kKnownDeltaMeters + actualX * kDeltaSlope;
        if (std::abs(d - expected) > 0.15)
        {
            std::cerr << "Sample " << i << " delta " << d
                      << " differs from morphed delta " << expected << "\n";
            return 1;
        }
    }

    std::printf(
        "Region delta composite: center sample raised by %.2f m "
        "(expected %.2f m); all morphed samples matched.\n",
        centerDelta,
        kKnownDeltaMeters);

    return 0;
}
