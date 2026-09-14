#include <orbit/terrain_stream/TerrainMorphRefresh.hpp>
#include <orbit/terrain_stream/TerrainSampleStreamer.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

namespace
{
using namespace orbit;

// Different fine/coarse heights and biomes expose stale blend weights as well
// as stale XY targets, independently of terrain noise and spherical drift.
class FootprintTerrainSource final : public terrain::TerrainSource
{
public:
    [[nodiscard]] terrain::TerrainSample Sample(
        const terrain::TerrainQuery& query) const noexcept override
    {
        const f32 coarse = static_cast<f32>(query.footprintMeters / 100.0);
        return {
            .elevationMeters = query.footprintMeters,
            .coarseElevationMeters = query.footprintMeters,
            .biomes = {.ocean = 1.0F - coarse, .desert = coarse},
            .standingWaterDepthMeters = std::max(55.0 - query.footprintMeters, 0.0)
        };
    }
};

terrain_stream::TerrainSampleRequest MakeRequest(
    const terrain_view::ClipmapLayout& layout,
    const terrain_view::ClipmapMotionUpdate& motion,
    const terrain_stream::LevelResidencyUpdate& update)
{
    const u32 index = update.levelIndex;
    const auto& level = layout.levels[index];
    const bool hasCoarser = index + 1U < layout.levels.size();
    const u32 coarseIndex = hasCoarser ? index + 1U : index;
    const auto& coarse = layout.levels[coarseIndex];
    return {
        .levelIndex = index,
        .resolution = level.gridResolution,
        .spacingMeters = level.sampleSpacingMeters,
        .footprintMeters = level.terrainFootprintMeters,
        .morphToCoarser = hasCoarser,
        .morphStartHalfExtentMeters = level.morphStartHalfExtentMeters,
        .morphEndHalfExtentMeters = level.morphEndHalfExtentMeters,
        .coarseSpacingMeters = coarse.sampleSpacingMeters,
        .coarseFootprintMeters = coarse.terrainFootprintMeters,
        .surfaceFrame = motion.levels[index].surfaceFrame,
        .coarseSurfaceFrame = motion.levels[coarseIndex].surfaceFrame,
        .originX = update.originX,
        .originY = update.originY,
        .regions = update.refreshRegions
    };
}

bool RunMovementRegression()
{
    const world::PlanetDefinition planet{.radiusMeters = 6'000'000.0};
    const terrain_view::ClipmapConfig config{
        .levelCount = 3,
        .gridResolution = 33,
        .baseSpacingMeters = 10.0,
        .levelScale = 2.0,
        .overlapCells = 3
    };
    const world::WorldPosition observer{.meters = {planet.radiusMeters + 300.0, 0.0, 0.0}};
    const auto layout = terrain_view::BuildClipmapLayout(config, observer);
    terrain_view::ClipmapTracker tracker(planet, config);
    terrain_stream::ToroidalResidency residency(config);
    auto motion = tracker.Update(observer);
    jobs::JobSystem jobs(2);
    const FootprintTerrainSource source;
    terrain_stream::TerrainSampleStreamer streamer(jobs, planet, source);
    constexpr u32 resolution = 33;
    constexpr u32 sampleCount = resolution * resolution;
    std::vector<std::vector<terrain_stream::TerrainSampleValue>> resident(
        config.levelCount,
        std::vector<terrain_stream::TerrainSampleValue>(sampleCount));

    // Sustained travel, reversals, diagonal wrapping, coarse-only motion,
    // a stationary frame, and a full rebase followed by incremental motion.
    const std::vector<math::Double2> shifts{
        {0, 0}, {1, 0}, {1, 0}, {1, 1}, {-1, 0}, {0, -2},
        {8, 7}, {12, 13}, {15, 14}, {-9, -11}, {-20, -18},
        {0, 0}, {0, 0}, {40, 0}, {-1, 1}
    };
    for (std::size_t step = 0; step < shifts.size(); ++step)
    {
        if (step != 0)
        {
            for (u32 index = 0; index < config.levelCount; ++index)
            {
                auto& movement = motion.levels[index];
                const f64 scale = std::pow(2.0, static_cast<f64>(index));
                movement.cellShiftX = static_cast<i64>(shifts[step].x / scale);
                movement.cellShiftY = static_cast<i64>(shifts[step].y / scale);
                // A parent can snap even when its child stays still.
                if (step == 11 && index == 1)
                {
                    movement.cellShiftX = 1;
                }
                const f64 spacing = layout.levels[index].sampleSpacingMeters;
                movement.surfaceFrame = world::SurfaceFrameAtOffset(
                    planet, movement.surfaceFrame,
                    {static_cast<f64>(movement.cellShiftX) * spacing,
                     static_cast<f64>(movement.cellShiftY) * spacing});
                movement.centerDirection = movement.surfaceFrame.up;
                movement.fullRefresh =
                    std::abs(movement.cellShiftX) >= resolution ||
                    std::abs(movement.cellShiftY) >= resolution;
            }
        }

        auto update = residency.Apply(motion);
        terrain_stream::RefreshTerrainMorphRegions(layout, motion, update);
        for (const auto& levelUpdate : update.levels)
        {
            const u32 index = levelUpdate.levelIndex;
            auto request = MakeRequest(layout, motion, levelUpdate);
            auto batch = streamer.Submit(std::span(&request, 1));
            const auto results = streamer.WaitCollect(batch);
            std::vector<bool> touched(sampleCount, false);
            u32 touchedCount = 0;
            for (const auto& patch : results[0].patches)
            {
                for (u32 y = 0; y < patch.region.height; ++y)
                {
                    for (u32 x = 0; x < patch.region.width; ++x)
                    {
                        const u32 physical = (patch.region.y + y) * resolution + patch.region.x + x;
                        if (physical >= sampleCount || touched[physical])
                        {
                            std::cerr << "Invalid or overlapping refresh regions.\n";
                            return false;
                        }
                        touched[physical] = true;
                        ++touchedCount;
                        resident[index][physical] = patch.samples[y * patch.region.width + x];
                    }
                }
            }
            if (step == 1 && touchedCount >= sampleCount / 2)
            {
                std::cerr << "Small moves should preserve the unaffected interior.\n";
                return false;
            }
            if (step == 12 && touchedCount != 0)
            {
                std::cerr << "Stationary clipmaps should not resample.\n";
                return false;
            }

            request.regions = {{0, 0, resolution, resolution}};
            const auto fresh = streamer.GenerateBlocking(std::span(&request, 1));
            const auto& reference = fresh[0].patches[0].samples;
            for (u32 y = 0; y < resolution; ++y)
            {
                for (u32 x = 0; x < resolution; ++x)
                {
                    const u32 physical = ((y + request.originY) % resolution) * resolution +
                        (x + request.originX) % resolution;
                    const auto& actual = resident[index][physical];
                    const auto& expected = reference[physical];
                    const f64 edgeDistance = std::max(
                        std::abs(static_cast<f64>(x) - 16.0),
                        std::abs(static_cast<f64>(y) - 16.0)) * request.spacingMeters;
                    const bool morphed = request.morphToCoarser &&
                        edgeDistance > request.morphStartHalfExtentMeters;
                    // Interior XY targets are ignored by the vertex shader.
                    if ((morphed &&
                         (std::abs(actual.morphTargetXMeters - expected.morphTargetXMeters) > 1.0e-4F ||
                          std::abs(actual.morphTargetYMeters - expected.morphTargetYMeters) > 1.0e-4F)) ||
                        std::abs(actual.elevationMeters - expected.elevationMeters) > 1.0e-4F ||
                        std::abs(actual.standingWaterDepthMeters - expected.standingWaterDepthMeters) > 1.0e-4F ||
                        actual.biomeWeights0 != expected.biomeWeights0 ||
                        actual.biomeWeights1 != expected.biomeWeights1)
                    {
                        std::cerr << "Incremental terrain differs from a fresh rebuild: step " << step
                                  << ", level " << index << ", logical " << x << ',' << y
                                  << ", target X " << actual.morphTargetXMeters << " vs "
                                  << expected.morphTargetXMeters << ", height " << actual.elevationMeters
                                  << " vs " << expected.elevationMeters << '\n';
                        return false;
                    }
                }
            }
        }
    }
    return true;
}
bool RunInvalidationRegression()
{
    const world::PlanetDefinition planet{};
    const terrain_view::ClipmapConfig config{
        .levelCount = 12, .gridResolution = 65, .baseSpacingMeters = 20.0,
        .levelScale = 2.0, .overlapCells = 6};
    const auto frame = world::MakeSurfaceFrame(math::Double3{0.65, 0.35, 0.68});
    terrain_view::ClipmapTracker tracker(planet, config);
    static_cast<void>(tracker.Update({frame.up * (planet.radiusMeters + 2'000'000.0)}));
    const auto direction = world::DirectionAtSurfaceOffset(planet, frame, {31'337.0, 17'123.0});
    const world::WorldPosition observer{direction * (planet.radiusMeters + 2'000'000.0)};
    static_cast<void>(tracker.Update(observer));
    auto reference = tracker;
    const auto expected = reference.Update(observer);
    for (int revision = 0; revision < 8; ++revision)
    {
        tracker.InvalidateSamples();
        const auto rebuilt = tracker.Update(observer);
        terrain_stream::ToroidalResidency residency(config);
        const auto refreshed = residency.Apply(rebuilt);
        for (std::size_t i = 0; i < rebuilt.levels.size(); ++i)
        {
            const auto& actual = rebuilt.levels[i];
            const auto& stable = expected.levels[i];
            if (!actual.fullRefresh || !refreshed.levels[i].fullRefresh ||
                refreshed.levels[i].refreshRegions.size() != 1 ||
                math::Length(actual.surfaceFrame.up - stable.surfaceFrame.up) > 1.0e-14 ||
                math::Length(actual.surfaceFrame.east - stable.surfaceFrame.east) > 1.0e-14 ||
                math::Length(actual.surfaceFrame.north - stable.surfaceFrame.north) > 1.0e-14)
            {
                std::cerr << "Source invalidation relocated a sampling lattice or omitted its full refresh.\n";
                return false;
            }
        }
        const auto settled = tracker.Update(observer);
        for (const auto& level : settled.levels)
        {
            if (level.fullRefresh)
            {
                std::cerr << "Source invalidation was not consumed.\n";
                return false;
            }
        }
    }
    auto tierConfig = config;
    tierConfig.baseSpacingMeters *= 2.0;
    auto tierTracker = tracker.Reconfigured(tierConfig);
    const auto tier = tierTracker.Update(observer);
    for (std::size_t i = 0; i + 1 < expected.levels.size(); ++i)
    {
        if (!tier.levels[i].fullRefresh || math::Length(tier.levels[i].surfaceFrame.up -
            expected.levels[i + 1].surfaceFrame.up) > 1.0e-14 ||
            math::Length(tier.levels[i].surfaceFrame.east -
            expected.levels[i + 1].surfaceFrame.east) > 1.0e-14)
        {
            std::cerr << "Coverage tier change relocated a shared sampling grid.\n";
            return false;
        }
    }
    // Ensure this scenario detects the old reset-to-observer behaviour.
    reference.Reset();
    const auto relocated = reference.Update(observer);
    if (math::Length(relocated.levels.back().surfaceFrame.up -
        expected.levels.back().surfaceFrame.up) < 1.0e-6)
    {
        std::cerr << "Invalidation regression did not exercise a displaced outer grid.\n";
        return false;
    }
    return true;
}
} // namespace

int main()
{
    return RunInvalidationRegression() && RunMovementRegression() ? 0 : 1;
}
