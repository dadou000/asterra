#include <orbit/jobs/JobSystem.hpp>
#include <orbit/terrain/TerrainSource.hpp>
#include <orbit/terrain_cache/CachedTerrainSource.hpp>
#include <orbit/terrain_cache/TerrainPageBuilder.hpp>
#include <orbit/terrain_erosion/RegionalElevationDelta.hpp>
#include <orbit/terrain_hydrology/HydrologyGrid.hpp>
#include <orbit/world/Planet.hpp>

#include <cmath>
#include <iostream>
#include <memory>
#include <vector>

namespace
{
class RecordingTerrainSource final :
    public orbit::terrain::TerrainSource
{
public:
    explicit RecordingTerrainSource(
        const orbit::world::PlanetId expectedPlanet)
        : expectedPlanet_(expectedPlanet)
    {
    }

    [[nodiscard]] orbit::terrain::TerrainSample Sample(
        const orbit::terrain::TerrainQuery& query)
        const noexcept override
    {
        ++calls_;

        const auto position =
            query.SurfacePosition();

        if (position.planet != expectedPlanet_ ||
            std::abs(
                orbit::math::Length(
                    position.unitDirection) -
                1.0) > 1.0e-12 ||
            !query.Footprint().IsValid())
        {
            canonical_ = false;
        }

        const auto& direction =
            position.unitDirection;

        const orbit::f64 elevation =
            direction.x * 1'000.0 +
            direction.y * 100.0 +
            direction.z * 10.0;

        return {
            .elevationMeters = elevation,
            .coarseElevationMeters = elevation,
            .climate = {
                .temperatureC = 10.0F,
                .humidity = 0.5F,
                .precipitation = 0.5F,
                .continentality = 0.5F
            }
        };
    }

    [[nodiscard]] bool Canonical() const noexcept
    {
        return canonical_;
    }

    [[nodiscard]] orbit::u64 Calls() const noexcept
    {
        return calls_;
    }

private:
    orbit::world::PlanetId expectedPlanet_{};
    mutable bool canonical_{true};
    mutable orbit::u64 calls_{0};
};

[[nodiscard]] bool Check(
    const bool condition,
    const char* message)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << message << '\n';
        return false;
    }

    return true;
}
} // namespace

int main()
{
    using namespace orbit;

    const world::PlanetId planetId{
        .high = 0x4d3031434f4f5244ULL,
        .low = 0x494e415445530001ULL
    };

    const world::PlanetDefinition planet{
        .radiusMeters = 6'000'000.0,
        .id = planetId,
        .generationSeed = 0xA57E22AULL
    };

    auto source =
        std::make_shared<
            RecordingTerrainSource>(
                planetId);

    bool ok = true;

    // A legacy caller can still omit planet identity, but the planet-bound
    // cache upgrades the request before authoritative sampling.
    jobs::JobSystem jobs(2);
    terrain_cache::CachedTerrainSource cached(
        planet,
        source,
        jobs,
        {
            .pageResolution = 9,
            .requestMissThreshold = 16,
            .minimumTileLevel = 6,
            .maximumTileLevel = 6
        });

    static_cast<void>(
        cached.Sample({
            .unitDirection = {2.0, 0.25, -0.5},
            .footprintMeters = 250.0
        }));

    ok &= Check(
        source->Canonical() &&
            source->Calls() == 1U,
        "CachedTerrainSource did not canonicalize a legacy query before fallback sampling.");

    // Exact cube-face edges belong to both adjacent physical pages. Sampling
    // the same direction through either page must therefore agree.
    const terrain_cache::TerrainPage positiveX =
        terrain_cache::BuildTerrainPage(
            planet,
            *source,
            {
                .planet = planetId,
                .tile = {
                    .face = world::CubeFace::PositiveX,
                    .level = 0,
                    .x = 0,
                    .y = 0
                },
                .resolution = 17
            });

    const terrain_cache::TerrainPage negativeZ =
        terrain_cache::BuildTerrainPage(
            planet,
            *source,
            {
                .planet = planetId,
                .tile = {
                    .face = world::CubeFace::NegativeZ,
                    .level = 0,
                    .x = 0,
                    .y = 0
                },
                .resolution = 17
            });

    const math::Double3 seamDirection =
        world::CubeToUnitDirection({
            .face = world::CubeFace::PositiveX,
            .uv = {1.0, 0.0}
        });

    const terrain::TerrainSample xSample =
        positiveX.SampleDirection(
            seamDirection);

    const terrain::TerrainSample zSample =
        negativeZ.SampleDirection(
            seamDirection);

    ok &= Check(
        std::abs(
            xSample.elevationMeters -
            zSample.elevationMeters) < 1.0e-5,
        "Adjacent terrain pages disagree at an identical physical cube-face seam sample.");

    const math::Double3 hydrologyDirection =
        math::Normalize(
            math::Double3{
                0.25,
                0.9,
                -0.35
            });

    const world::SurfaceFrame hydrologyFrame =
        world::MakeSurfaceFrame(
            hydrologyDirection);

    const terrain_hydrology::HydrologyGrid hydrology =
        terrain_hydrology::BuildHydrologyGrid(
            planet,
            *source,
            hydrologyFrame,
            {
                .resolution = 5,
                .halfExtentMeters = 1'000.0,
                .footprintMeters = 100.0,
                .useCoarseElevation = true,
                .conditionDepressions = false
            });

    ok &= Check(
        hydrology.origin.planet == planetId &&
            math::Dot(
                hydrology.origin.unitDirection,
                hydrologyDirection) >
                1.0 - 1.0e-12 &&
            source->Canonical(),
        "Hydrology did not preserve canonical planet-space origin and sampling identity.");

    const std::vector<f32> zeroDelta(
        hydrology.cells.size(),
        0.0F);

    const auto regional =
        terrain_erosion::BuildRegionalElevationDeltaField(
            hydrology,
            zeroDelta);

    ok &= Check(
        regional.origin.planet == planetId &&
            math::Dot(
                regional.origin.unitDirection,
                hydrology.origin.unitDirection) >
                1.0 - 1.0e-12,
        "Regional erosion field lost the canonical hydrology surface origin.");

    return ok ? 0 : 1;
}
