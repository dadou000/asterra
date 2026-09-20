#include <orbit/math/Vector.hpp>
#include <orbit/rhi/vulkan/VulkanBackend.hpp>
#include <orbit/shader/dxc/DxcShaderCompiler.hpp>
#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/terrain_geology/GeologicalMaterial.hpp>
#include <orbit/terrain_gpu/GpuAeolianErosionPage.hpp>
#include <orbit/terrain_gpu/GpuBiomeScatterPage.hpp>
#include <orbit/terrain_gpu/GpuDrainagePage.hpp>
#include <orbit/terrain_gpu/GpuFieldGenerator.hpp>
#include <orbit/terrain_gpu/GpuHydraulicErosionPage.hpp>
#include <orbit/terrain_gpu/GpuHydrologyRegion.hpp>
#include <orbit/terrain_gpu/GpuMaterialColumnResources.hpp>
#include <orbit/terrain_gpu/PersistentGpuTerrainCache.hpp>
#include <orbit/terrain_material_column/MaterialColumnPage.hpp>
#include <orbit/world/Planet.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using namespace orbit;

constexpr u32 kResolution = 129U;
constexpr u32 kWarmupRuns = 2U;
constexpr u32 kMeasuredRuns = 7U;
constexpr u32 kCacheProbeCount = 256U;

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

template <typename T>
[[nodiscard]] std::unique_ptr<rhi::Buffer>
MakeInputBuffer(
    rhi::Device& device,
    const std::span<const T> values)
{
    const u64 bytes =
        static_cast<u64>(
            values.size_bytes());

    auto buffer =
        device.CreateBuffer({
            .sizeBytes = bytes,
            .usage = rhi::BufferUsage::Structured,
            .memory = rhi::MemoryUsage::HostVisible,
            .initialState =
                rhi::ResourceState::ShaderResource
        });

    std::byte* mapped =
        buffer->Map();

    std::memcpy(
        mapped,
        values.data(),
        values.size_bytes());

    buffer->Unmap();

    return buffer;
}

struct PerformanceRecord
{
    std::string adapter;
    f64 pageGenerationGpuMedianMs{0.0};
    u64 peakTransientBytes{0U};
    u64 persistentPageBytes{0U};
    f64 cacheHitRatePercent{0.0};
    f64 hydraulicIterationGpuMedianMs{0.0};
    f64 aeolianIterationGpuMedianMs{0.0};
    f64 drainageBuildGpuMedianMs{0.0};
    f64 scatterGenerationGpuMedianMs{0.0};
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

    output
        << "orbit_v004_m30,"
        << CsvEscape(record.adapter)
        << ',' << kResolution
        << ",cache_hit_rate,"
        << std::fixed << std::setprecision(6)
        << record.cacheHitRatePercent
        << ",percent," << kCacheProbeCount << '\n';

    output
        << "orbit_v004_m30,"
        << CsvEscape(record.adapter)
        << ',' << kResolution
        << ",hydraulic_iteration_gpu_time,"
        << std::fixed << std::setprecision(6)
        << record.hydraulicIterationGpuMedianMs
        << ",ms," << kMeasuredRuns << '\n';

    output
        << "orbit_v004_m30,"
        << CsvEscape(record.adapter)
        << ',' << kResolution
        << ",aeolian_iteration_gpu_time,"
        << std::fixed << std::setprecision(6)
        << record.aeolianIterationGpuMedianMs
        << ",ms," << kMeasuredRuns << '\n';

    output
        << "orbit_v004_m30,"
        << CsvEscape(record.adapter)
        << ',' << kResolution
        << ",drainage_build_gpu_time,"
        << std::fixed << std::setprecision(6)
        << record.drainageBuildGpuMedianMs
        << ",ms," << kMeasuredRuns << '\n';

    output
        << "orbit_v004_m30,"
        << CsvEscape(record.adapter)
        << ',' << kResolution
        << ",scatter_generation_gpu_time,"
        << std::fixed << std::setprecision(6)
        << record.scatterGenerationGpuMedianMs
        << ",ms," << kMeasuredRuns << '\n';
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

        terrain_geology::GeologicalMaterialLibrary
            geology;

        geology.Upsert({
            .id =
                terrain_geology::
                    reference_rock::Basalt,
            .name =
                "M30 performance basalt",
            .hardness = 0.82F,
            .cohesion = 0.78F,
            .hydraulicErodibility = 0.34F,
            .aeolianErodibility = 0.24F,
            .permeability = 0.18F,
            .chemicalWeatherability = 0.22F,
            .fractureTendency = 0.36F,
            .density = 2'900.0F
        });

        const auto geologyTable =
            geology.BuildGpuTable();

        terrain_material_column::
            MaterialColumnPage materialPage(
                kResolution,
                4.0);

        for (u32 y = 0U;
             y < kResolution;
             ++y)
        {
            for (u32 x = 0U;
                 x < kResolution;
                 ++x)
            {
                materialPage.SetCell(
                    x,
                    y,
                    {
                        .bedrockHeightMeters =
                            1'000.0F -
                            static_cast<f32>(x) *
                                0.35F +
                            static_cast<f32>(y) *
                                0.05F,
                        .referenceBedrockHeightMeters =
                            1'000.0F -
                            static_cast<f32>(x) *
                                0.35F +
                            static_cast<f32>(y) *
                                0.05F,
                        .bedrockMaterial =
                            terrain_geology::
                                reference_rock::Basalt,
                        .regolithMeters = 0.15F,
                        .soilMeters = 0.20F,
                        .sandMeters = 0.18F,
                        .debrisMeters = 0.02F,
                        .moisture = 0.12F,
                        .temporaryScalar = 0.0F
                    });
            }
        }

        const auto packedMaterial =
            terrain_material_column::
                PackGpuPage(
                    materialPage,
                    geologyTable);

        terrain_gpu::GpuMaterialColumnResources
            hydraulicMaterial(
                *device,
                kResolution);

        terrain_gpu::GpuMaterialColumnResources
            aeolianMaterial(
                *device,
                kResolution);

        terrain_gpu::GpuMaterialColumnResources
            drainageMaterial(
                *device,
                kResolution);

        terrain_gpu::GpuHydraulicErosionPage
            hydraulic(
                *device,
                shaderCompiler,
                kResolution,
                static_cast<u32>(
                    geologyTable.materials.size()));

        terrain_gpu::GpuAeolianErosionPage
            aeolian(
                *device,
                shaderCompiler,
                kResolution,
                static_cast<u32>(
                    geologyTable.materials.size()));

        terrain_gpu::GpuDrainagePage
            drainageBuilder(
                *device,
                shaderCompiler,
                kResolution);

        terrain_gpu::GpuBiomeScatterPage
            scatterBuilder(
                *device,
                shaderCompiler,
                kResolution);

        hydraulic.UploadGeologyTable(
            geologyTable);

        aeolian.UploadGeologyTable(
            geologyTable);

        const u64 cells =
            static_cast<u64>(kResolution) *
            kResolution;

        const u64 scalarBytes =
            cells * sizeof(f32);

        auto windForcing =
            device->CreateBuffer({
                .sizeBytes =
                    cells *
                    sizeof(f32) *
                    4U,
                .usage =
                    rhi::BufferUsage::Structured,
                .memory =
                    rhi::MemoryUsage::HostVisible,
                .initialState =
                    rhi::ResourceState::ShaderResource
            });

        {
            std::vector<std::array<f32, 4U>>
                wind(
                    static_cast<std::size_t>(
                        cells),
                    {14.0F, 1.5F, 0.10F, 0.0F});

            std::byte* mapped =
                windForcing->Map();

            std::memcpy(
                mapped,
                wind.data(),
                wind.size() *
                    sizeof(wind.front()));

            windForcing->Unmap();
        }

        const u32 paddedResolution =
            terrain_gpu::
                GpuDrainagePage::
                    PaddedResolution(
                        kResolution);

        const u64 paddedCells =
            static_cast<u64>(
                paddedResolution) *
            paddedResolution;

        std::vector<f32>
            haloConditionedValues(
                static_cast<std::size_t>(
                    paddedCells),
                900.0F);

        std::vector<f32>
            runoffValues(
                static_cast<std::size_t>(
                    cells),
                0.0025F);

        std::vector<f32>
            guidanceValues(
                static_cast<std::size_t>(
                    cells),
                0.0F);

        std::vector<f32>
            zeroCoreValues(
                static_cast<std::size_t>(
                    cells),
                0.0F);

        auto haloConditioned =
            MakeInputBuffer(
                *device,
                std::span<const f32>(
                    haloConditionedValues));

        auto runoffRate =
            MakeInputBuffer(
                *device,
                std::span<const f32>(
                    runoffValues));

        auto authoredGuidance =
            MakeInputBuffer(
                *device,
                std::span<const f32>(
                    guidanceValues));

        auto incomingArea =
            MakeInputBuffer(
                *device,
                std::span<const f32>(
                    zeroCoreValues));

        auto incomingDischarge =
            MakeInputBuffer(
                *device,
                std::span<const f32>(
                    zeroCoreValues));

        auto drainagePageOut =
            MakeOutputBuffer(
                *device,
                paddedCells *
                    sizeof(f32));

        auto drainageAreaOut =
            MakeOutputBuffer(
                *device,
                paddedCells *
                    sizeof(f32));

        auto dischargeOut =
            MakeOutputBuffer(
                *device,
                paddedCells *
                    sizeof(f32));

        auto downstreamPageOut =
            MakeOutputBuffer(
                *device,
                paddedCells *
                    sizeof(u32));

        const terrain_gpu::
            GpuDrainagePageRequest
            drainageRequest{
                .resolution =
                    kResolution,
                .spacingMeters =
                    4.0F,
                .minimumDrainageDropMeters =
                    0.01F,
                .seaLevelMeters =
                    -1.0e6F,
                .authoredGuidanceWeight =
                    0.20F,
                .depressionPolicy =
                    terrain_hydrology::
                        DepressionRoutingPolicy::
                            FillToBoundary
            };

        terrain_scatter::
            ScatterPageRequest
            scatterRequest{
                .identity = {
                    .planet =
                        planet.id,
                    .tile =
                        world::TileForDirection(
                            math::Normalize(
                                math::Double3{
                                    0.31,
                                    0.27,
                                    0.91}),
                            10U),
                    .sourceRevision = 31U,
                    .scatterRevision = 22U,
                    .generationSeed =
                        0xA57E22A60022ULL
                },
                .gridResolution =
                    kResolution,
                .cellSizeMeters =
                    4.0F,
                .rule = {
                    .id = {
                        .high =
                            0x4D33305045524632ULL,
                        .low =
                            0x0000000000000022ULL
                    },
                    .kind =
                        terrain_biome::
                            BiomeScatterKind::Tree,
                    .densityPerSquareMeter =
                        0.035F,
                    .minimumSpacingMeters =
                        4.0F,
                    .seedSalt = 220U,
                    .compatibleExposed =
                        terrain_biome::
                            BiomeExposedMaterialMask::
                                Soil,
                    .requiresSoil = true,
                    .minimumSoilDepthMeters =
                        0.10F,
                    .minimumSlopeDegrees =
                        0.0F,
                    .maximumSlopeDegrees =
                        35.0F,
                    .slopeFalloffDegrees =
                        8.0F,
                    .minimumMoisture =
                        0.05F,
                    .maximumMoisture =
                        0.90F,
                    .moistureFalloff =
                        0.10F,
                    .minimumScale =
                        0.85F,
                    .maximumScale =
                        1.15F,
                    .enabled = true
                },
                .biomeDensityMultiplier =
                    1.0F
            };

        std::vector<
            terrain_scatter::
                ScatterCellInput>
            scatterCells(
                static_cast<std::size_t>(
                    cells));

        for (u32 y = 0U;
             y < kResolution;
             ++y)
        {
            for (u32 x = 0U;
                 x < kResolution;
                 ++x)
            {
                auto& cell =
                    scatterCells[
                        static_cast<
                            std::size_t>(
                                y) *
                            kResolution +
                        x];

                cell = {
                    .biomeWeight =
                        0.85F,
                    .exposedMaterial =
                        terrain_material_column::
                            ExposedSurfaceKind::Soil,
                    .slopeDegrees =
                        6.0F +
                        static_cast<f32>(
                            (x + y) % 7U),
                    .soilDepthMeters =
                        0.20F,
                    .moisture =
                        0.45F,
                    .exclusionMask =
                        0.0F,
                    .authoredDensity =
                        1.0F
                };
            }
        }

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

        u64 fenceValue = 0U;

        const auto measureGpuMedianMs =
            [&](auto&& prepare,
                auto&& measured)
            {
                std::array<
                    f64,
                    kMeasuredRuns>
                    measuredMs{};

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

                    prepare(
                        *commandList);

                    commandList->
                        WriteTimestamp(
                            *timestamps,
                            0U);

                    measured(
                        *commandList);

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
                        ticks[1] <
                            ticks[0])
                    {
                        throw std::runtime_error(
                            "GPU performance timestamp query was unavailable.");
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

                return
                    measuredMs[
                        measuredMs.size() /
                        2U];
            };

        const f64 medianGpuMs =
            measureGpuMedianMs(
                [](rhi::CommandList&)
                {
                },
                [&](rhi::CommandList& list)
                {
                    hydrology.Dispatch(
                        list,
                        request,
                        *rawElevation,
                        *drainage,
                        *accumulation,
                        *downstream,
                        *netElevationDelta);
                });

        const terrain_gpu::
            GpuHydraulicErosionConfig
            hydraulicConfig{};

        const f64 hydraulicIterationMs =
            measureGpuMedianMs(
                [&](rhi::CommandList& list)
                {
                    hydraulicMaterial.Upload(
                        list,
                        packedMaterial);

                    hydraulic.Reset(
                        list,
                        kResolution,
                        0.01F,
                        0.0F);
                },
                [&](rhi::CommandList& list)
                {
                    hydraulic.DispatchSteps(
                        list,
                        kResolution,
                        4.0F,
                        1U,
                        hydraulicConfig,
                        hydraulicMaterial);
                });

        const terrain_gpu::
            GpuAeolianErosionConfig
            aeolianConfig{};

        const f64 aeolianIterationMs =
            measureGpuMedianMs(
                [&](rhi::CommandList& list)
                {
                    aeolianMaterial.Upload(
                        list,
                        packedMaterial);

                    aeolian.Reset(
                        list,
                        kResolution);
                },
                [&](rhi::CommandList& list)
                {
                    aeolian.DispatchSteps(
                        list,
                        kResolution,
                        4.0F,
                        1U,
                        aeolianConfig,
                        aeolianMaterial,
                        *windForcing);
                });

        const f64 drainageBuildMs =
            measureGpuMedianMs(
                [&](rhi::CommandList& list)
                {
                    drainageMaterial.Upload(
                        list,
                        packedMaterial);
                },
                [&](rhi::CommandList& list)
                {
                    drainageBuilder.Dispatch(
                        list,
                        drainageRequest,
                        drainageMaterial,
                        *haloConditioned,
                        *runoffRate,
                        *authoredGuidance,
                        *incomingArea,
                        *incomingDischarge,
                        *drainagePageOut,
                        *drainageAreaOut,
                        *dischargeOut,
                        *downstreamPageOut);
                });

        const f64 scatterGenerationMs =
            measureGpuMedianMs(
                [](rhi::CommandList&)
                {
                },
                [&](rhi::CommandList& list)
                {
                    scatterBuilder.Dispatch(
                        list,
                        scatterRequest,
                        scatterCells);
                });

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

        terrain_gpu::
            PersistentGpuTerrainCache
            hitRateCache({
                .maximumResidentBytes =
                    64ULL *
                    1024ULL *
                    1024ULL,
                .maximumPages = 8U
            });

        u64 hitRateGeneratorCalls = 0U;

        for (u32 probe = 0U;
             probe < kCacheProbeCount;
             ++probe)
        {
            static_cast<void>(
                hitRateCache.GetOrCreate(
                    cacheKey,
                    [&]()
                    {
                        ++hitRateGeneratorCalls;
                        return cachedPage;
                    }));
        }

        const auto hitRateStats =
            hitRateCache.Stats();

        const u64 cacheLookups =
            hitRateStats.hits +
            hitRateStats.misses;

        if (hitRateGeneratorCalls !=
                1U ||
            hitRateStats.generations !=
                1U ||
            hitRateStats.misses !=
                1U ||
            hitRateStats.hits !=
                kCacheProbeCount -
                    1U ||
            cacheLookups !=
                kCacheProbeCount)
        {
            throw std::runtime_error(
                "M26 controlled cache-hit workload did not produce one cold generation followed by resident hits.");
        }

        const f64 cacheHitRatePercent =
            100.0 *
            static_cast<f64>(
                hitRateStats.hits) /
            static_cast<f64>(
                cacheLookups);

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
                    residentBytes,
            .cacheHitRatePercent =
                cacheHitRatePercent,
            .hydraulicIterationGpuMedianMs =
                hydraulicIterationMs,
            .aeolianIterationGpuMedianMs =
                aeolianIterationMs,
            .drainageBuildGpuMedianMs =
                drainageBuildMs,
            .scatterGenerationGpuMedianMs =
                scatterGenerationMs
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
