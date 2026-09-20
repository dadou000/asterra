#include <orbit/math/Vector.hpp>
#include <orbit/rhi/vulkan/VulkanBackend.hpp>
#include <orbit/shader/dxc/DxcShaderCompiler.hpp>
#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/terrain_gpu/GpuFieldGenerator.hpp>
#include <orbit/terrain_gpu/GpuHydrologyRegion.hpp>
#include <orbit/terrain_gpu/PersistentGpuTerrainCache.hpp>
#include <orbit/world/Planet.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
using namespace orbit;

constexpr u32 kResolution = 129U;
constexpr u32 kWarmupRuns = 2U;
constexpr u32 kMeasuredRuns = 7U;

[[nodiscard]] std::string CsvEscape(
    const std::string_view value)
{
    std::string escaped;
    escaped.reserve(value.size() + 2U);
    escaped.push_back('"');

    for (const char character : value)
    {
        if (character == '"')
        {
            escaped.push_back('"');
        }

        escaped.push_back(character);
    }

    escaped.push_back('"');
    return escaped;
}

[[nodiscard]] std::shared_ptr<rhi::Buffer>
MakeOutputBuffer(
    rhi::Device& device,
    const u64 bytes)
{
    return std::shared_ptr<rhi::Buffer>(
        device.CreateBuffer({
            .sizeBytes = bytes,
            .usage = rhi::BufferUsage::Structured,
            .memory = rhi::MemoryUsage::GpuOnly,
            .initialState =
                rhi::ResourceState::CopyDestination
        }));
}

struct PerformanceRecord
{
    std::string adapter;
    f64 pageGenerationGpuMedianMs{0.0};
    u64 peakTransientBytes{0U};
    u64 persistentPageBytes{0U};
};

void WriteRecord(
    std::ostream& output,
    const PerformanceRecord& record)
{
    output
        << "schema,adapter,resolution,metric,value,unit,samples\n";

    output
        << "orbit_v004_m30,"
        << CsvEscape(record.adapter)
        << ',' << kResolution
        << ",page_generation_gpu_time,"
        << std::fixed << std::setprecision(6)
        << record.pageGenerationGpuMedianMs
        << ",ms," << kMeasuredRuns << '\n';

    output
        << "orbit_v004_m30,"
        << CsvEscape(record.adapter)
        << ',' << kResolution
        << ",peak_transient_memory,"
        << record.peakTransientBytes
        << ",bytes,1\n";

    output
        << "orbit_v004_m30,"
        << CsvEscape(record.adapter)
        << ',' << kResolution
        << ",persistent_page_memory,"
        << record.persistentPageBytes
        << ",bytes,1\n";
}
} // namespace

int main(int argc, char** argv)
{
    using namespace orbit;

    try
    {
        auto device =
            rhi::vulkan::CreateDevice({
                .enableValidation = false,
                .enableBestPracticesValidation = false,
                .enableSynchronizationValidation = false,
                .enableGpuAssistedValidation = false,
                .enableRenderDoc = false
            });

        const shader::dxc::DxcShaderCompiler
            shaderCompiler;

        const world::PlanetDefinition planet{
            .radiusMeters = 6'000'000.0,
            .id = {
                .high = 0x4D33305045524631ULL,
                .low = 0x0000000000000001ULL
            },
            .generationSeed =
                0xA57E22A60030ULL
        };

        const terrain::AnalyticTerrainSource source(
            planet,
            {
                .seed =
                    planet.generationSeed
            });

        terrain_gpu::GpuFieldGenerator
            fieldGenerator(
                *device,
                shaderCompiler,
                planet,
                source);

        terrain_gpu::GpuHydrologyRegion
            hydrology(
                *device,
                shaderCompiler,
                fieldGenerator,
                kResolution);

        const u64 cells =
            static_cast<u64>(kResolution) *
            kResolution;

        const u64 scalarBytes =
            cells * sizeof(f32);

        auto rawElevation =
            MakeOutputBuffer(
                *device,
                scalarBytes);

        auto drainage =
            MakeOutputBuffer(
                *device,
                scalarBytes);

        auto accumulation =
            MakeOutputBuffer(
                *device,
                scalarBytes);

        auto downstream =
            MakeOutputBuffer(
                *device,
                cells * sizeof(u32));

        auto netElevationDelta =
            MakeOutputBuffer(
                *device,
                scalarBytes);

        auto queue =
            device->CreateQueue(
                rhi::QueueType::Graphics);

        auto allocator =
            device->CreateCommandAllocator(
                rhi::QueueType::Graphics);

        auto commandList =
            device->CreateCommandList(
                *allocator);

        auto fence =
            device->CreateFence(0U);

        auto timestamps =
            device->CreateTimestampQueryPool(
                2U);

        const f64 timestampPeriodNs =
            device->
                TimestampPeriodNanoseconds();

        if (!(timestampPeriodNs > 0.0))
        {
            throw std::runtime_error(
                "GPU timestamp period is not positive.");
        }

        const math::Double3 direction =
            math::Normalize(
                math::Double3{
                    0.365111510558,
                    0.187057495117,
                    -0.911977564625
                });

        const terrain_gpu::
            GpuHydrologyRegionRequest request{
                .resolution =
                    kResolution,
                .spacingMeters =
                    200.0,
                .footprintMeters =
                    200.0,
                .surfaceFrame =
                    world::MakeSurfaceFrame(
                        direction),
                .seaLevelMeters =
                    0.0F,
                .minimumDropMeters =
                    0.25F,
                .erosion = {}
            };

        std::array<
            f64,
            kMeasuredRuns>
            measuredMs{};

        u64 fenceValue = 0U;

        const u32 totalRuns =
            kWarmupRuns +
            kMeasuredRuns;

        for (u32 run = 0U;
             run < totalRuns;
             ++run)
        {
            allocator->Reset();
            commandList->Reset(
                *allocator);

            commandList->
                ResetTimestampQueryPool(
                    *timestamps,
                    0U,
                    2U);

            commandList->
                WriteTimestamp(
                    *timestamps,
                    0U);

            hydrology.Dispatch(
                *commandList,
                request,
                *rawElevation,
                *drainage,
                *accumulation,
                *downstream,
                *netElevationDelta);

            commandList->
                WriteTimestamp(
                    *timestamps,
                    1U);

            commandList->Close();

            queue->Submit(
                *commandList);

            ++fenceValue;
            queue->Signal(
                *fence,
                fenceValue);

            fence->Wait(
                fenceValue);

            std::array<u64, 2U>
                ticks{};

            if (!timestamps->
                    TryGetResults(
                        0U,
                        2U,
                        ticks.data()) ||
                ticks[1] < ticks[0])
            {
                throw std::runtime_error(
                    "GPU page-generation timestamp query was unavailable.");
            }

            if (run >=
                kWarmupRuns)
            {
                measuredMs[
                    run -
                    kWarmupRuns] =
                    static_cast<f64>(
                        ticks[1] -
                        ticks[0]) *
                    timestampPeriodNs /
                    1.0e6;
            }
        }

        std::sort(
            measuredMs.begin(),
            measuredMs.end());

        const f64 medianGpuMs =
            measuredMs[
                measuredMs.size() /
                2U];

        auto cachedPage =
            std::make_shared<
                terrain_gpu::
                    CachedGpuTerrainPage>();

        cachedPage->products =
            terrain_gpu::ProductBit(
                terrain_gpu::
                    CachedTerrainProduct::
                        Drainage) |
            terrain_gpu::ProductBit(
                terrain_gpu::
                    CachedTerrainProduct::
                        Hydraulic);

        cachedPage->buffers = {
            rawElevation,
            drainage,
            accumulation,
            downstream,
            netElevationDelta
        };

        const terrain_gpu::
            PersistentGpuTerrainCacheKey
            cacheKey{
                .address = {
                    .planet = planet.id,
                    .tile =
                        world::TileForDirection(
                            direction,
                            9U)
                },
                .physicalLod = 3U,
                .revisions = {
                    .geology = 1U,
                    .climate = 1U,
                    .authoring = 1U,
                    .biome = 1U,
                    .water = 1U,
                    .processes = 1U
                }
            };

        terrain_gpu::
            PersistentGpuTerrainCache
            cache({
                .maximumResidentBytes =
                    64ULL *
                    1024ULL *
                    1024ULL,
                .maximumPages = 8U
            });

        cache.Insert(
            cacheKey,
            cachedPage);

        const auto& cacheStats =
            cache.Stats();

        if (cacheStats.residentPages !=
                1U ||
            cacheStats.residentBytes !=
                cachedPage->
                    ResidentBytes())
        {
            throw std::runtime_error(
                "M26 persistent page byte accounting diverged from the inserted solved page.");
        }

        const PerformanceRecord record{
            .adapter =
                std::string(
                    device->AdapterName()),
            .pageGenerationGpuMedianMs =
                medianGpuMs,
            .peakTransientBytes =
                hydrology.
                    TransientWorkingSetBytes(),
            .persistentPageBytes =
                cacheStats.
                    residentBytes
        };

        WriteRecord(
            std::cout,
            record);

        if (argc > 1)
        {
            std::ofstream file(
                argv[1],
                std::ios::binary |
                    std::ios::trunc);

            if (!file)
            {
                throw std::runtime_error(
                    "Unable to open M30 performance output file.");
            }

            WriteRecord(
                file,
                record);
        }

        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr
            << "Orbit V0.0.4 M30 performance diagnostic failed: "
            << error.what()
            << '\n';

        return EXIT_FAILURE;
    }
}
