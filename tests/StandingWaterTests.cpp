#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/terrain_cache/TerrainPage.hpp>
#include <orbit/terrain_region/DerivedRegionTerrainSource.hpp>
#include <orbit/terrain_stream/TerrainSampleStreamer.hpp>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace
{
using namespace orbit;
void Require(const bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
}
class Bowl final : public terrain::TerrainSource
{
public:
    world::PlanetDefinition planet{};
    world::SurfaceFrame frame{};
    terrain::TerrainSample Sample(const terrain::TerrainQuery& query) const noexcept override
    {
        const auto offset = world::SurfaceOffsetBetweenDirections(planet, frame, query.unitDirection);
        const double r2 = offset.x * offset.x + offset.y * offset.y;
        const double h = 120.0 - 50.0 * std::exp(-r2 / (4'000.0 * 4'000.0));
        return {.elevationMeters = h, .coarseElevationMeters = h};
    }
};
}
int main()
{
    using namespace orbit;
    try
    {
        // Rendering must interpolate the filled surface, not clamp the
        // interpolated bed afterward (which cracks at a wet/dry parent edge).
        const terrain::TerrainSample wet{.elevationMeters = -40.0,
            .standingWaterDepthMeters = 40.0};
        const terrain::TerrainSample dry{.elevationMeters = 20.0};
        for (int i = 0; i <= 100; ++i)
        {
            const double t = i / 100.0;
            const auto sample = terrain::LerpTerrainSample(wet, dry, t);
            const auto packed = terrain_cache::FromCachedSample(terrain_cache::ToCachedSample(sample));
            Require(std::abs(sample.elevationMeters + sample.standingWaterDepthMeters - 20.0 * t) < 1.0e-10,
                "Wet/dry morph does not follow the parent surface");
            Require(std::abs(packed.standingWaterDepthMeters - sample.standingWaterDepthMeters) < 2.0e-6,
                "Page cache lost water depth");
        }
        world::PlanetDefinition planet{};
        terrain::AnalyticTerrainDesc desc{};
        desc.global.seaLevelMeters = 123.0;
        terrain::AnalyticTerrainSource analytic(planet, desc);
        unsigned oceanSamples = 0;
        for (int i = 0; i < 1024; ++i)
        {
            const double y = 1.0 - 2.0 * (i + 0.5) / 1024.0;
            const double r = std::sqrt(1.0 - y * y);
            const math::Double3 direction{r * std::cos(i * 2.39996323), y, r * std::sin(i * 2.39996323)};
            for (const double footprint : {1.0, 100.0, 10'000.0, 200'000.0})
            {
                const auto sample = analytic.Sample({direction, footprint});
                const double expected = std::max(sample.elevationMeters, 123.0);
                Require(std::abs(sample.elevationMeters + sample.standingWaterDepthMeters - expected) < 1.0e-9,
                    "Ocean surface drifted with source LOD");
                if (sample.standingWaterDepthMeters > 0.0) ++oceanSamples;
            }
        }
        Require(oceanSamples > 100, "Ocean test did not reach water");

        jobs::JobSystem jobs(2);
        auto bowl = std::make_shared<Bowl>();
        bowl->planet = planet;
        terrain_region::DerivedTerrainRegionCacheConfig coarseConfig{};
        coarseConfig.tileLevel = 8;
        coarseConfig.region.hydrology.resolution = 33;
        coarseConfig.region.refinement.iterations = 0;
        coarseConfig.region.minimumRiverDrainageAreaSquareMeters = 1.0e30;
        auto coarse = std::make_shared<terrain_region::DerivedTerrainRegionCache>(
            planet, bowl, jobs, coarseConfig);
        const auto id = coarse->IdForDirection(math::Normalize(math::Double3{1.0, 0.2, 0.1}));
        const auto center = world::CubeToUnitDirection(world::TileCenter(id.tile));
        bowl->frame = world::MakeSurfaceFrame(center);
        Require(coarse->Request(id), "Basin request failed");
        coarse->WaitAll();
        auto fineConfig = coarseConfig;
        fineConfig.tileLevel = 10;
        auto fine = std::make_shared<terrain_region::DerivedTerrainRegionCache>(planet, bowl, jobs, fineConfig);
        terrain_region::DerivedRegionTerrainSource source(planet, bowl, coarse, fine);
        const auto before = source.Sample({center, 1.0});
        Require(before.standingWaterDepthMeters > 1.0, "Regional lake is missing");
        Require(fine->RequestDirection(center), "Fine basin request failed");
        fine->WaitAll();
        const auto after = source.Sample({center, 1.0});
        Require(std::abs(after.elevationMeters - before.elevationMeters) < 1.0e-8 &&
            std::abs(after.standingWaterDepthMeters - before.standingWaterDepthMeters) < 1.0e-8,
            "Fine cache arrival changed the authoritative lake bed or water level");
        terrain_stream::TerrainSampleStreamer streamer(jobs, planet, source);
        terrain_stream::TerrainSampleRequest request{};
        request.resolution = 9;
        request.spacingMeters = 20.0;
        request.footprintMeters = 20.0;
        request.surfaceFrame = bowl->frame;
        request.regions = {{0, 0, 9, 9}};
        const auto patches = streamer.GenerateBlocking(std::span(&request, 1));
        const auto& gpu = patches.front().patches.front().samples[40];
        const auto reference = source.Sample({center, 20.0});
        Require(std::abs(gpu.standingWaterDepthMeters - reference.standingWaterDepthMeters) < 1.0e-4,
            "GPU stream lost standing water depth");
        std::cout << "Standing water: cache/morph, ocean LOD, basin authority and GPU payload passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
