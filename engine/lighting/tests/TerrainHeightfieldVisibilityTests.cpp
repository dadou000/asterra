#include <orbit/lighting/TerrainHeightfieldVisibility.hpp>

#include <cmath>

namespace
{
class FlatTerrain final
    : public orbit::terrain::TerrainSource
{
public:
    explicit FlatTerrain(
        const double elevation)
        : elevation_(elevation)
    {
    }

    [[nodiscard]]
    orbit::terrain::TerrainSample Sample(
        const orbit::terrain::TerrainQuery&)
        const noexcept override
    {
        return {
            .elevationMeters =
                elevation_,
            .coarseElevationMeters =
                elevation_
        };
    }

private:
    double elevation_{0.0};
};

// Fine footprints see the true ground at radius 0; coarse footprints (a wide
// average over the surroundings, as in a valley floor) see it 20 m higher.
class FootprintTerrain final
    : public orbit::terrain::TerrainSource
{
public:
    [[nodiscard]]
    orbit::terrain::TerrainSample Sample(
        const orbit::terrain::TerrainQuery& query)
        const noexcept override
    {
        const double elevation =
            query.footprintMeters > 10.0
                ? 20.0
                : 0.0;

        return {
            .elevationMeters = elevation,
            .coarseElevationMeters = elevation
        };
    }
};
} // namespace

int main()
{
    using namespace orbit;

    frames::FrameGraph frames;

    universe::BodyRegistry bodies(frames);
    const auto system =
        bodies.CreateSystem(
            "Terrain System");

    const auto body =
        bodies.CreateBody({
            .system = system,
            .name = "Terrain Sphere",
            .shape =
                universe::SphereShape{
                    .radiusMeters =
                        10.0},
            .transformModel =
                universe::FixedBodyTransform{}
        });

    const auto* bodyRecord =
        bodies.FindBody(body);

    if (bodyRecord == nullptr)
    {
        return 1;
    }

    world::PlanetDefinition planet{
        .radiusMeters = 10.0,
        .id =
            world::PlanetId{
                .high = 5U,
                .low = 6U}
    };

    FlatTerrain source(2.0);

    lighting::TerrainHeightfieldVisibilityProvider
        provider(
            body,
            planet,
            source,
            bodies,
            frames,
            {
                .maximumAbsoluteElevationMeters =
                    5.0,
                .minimumStepMeters =
                    0.01,
                .maximumStepMeters =
                    1.0,
                .normalSampleSpacingMeters =
                    0.1,
                .maximumMarchSteps =
                    256U,
                .rootRefinementIterations =
                    16U
            });

    lighting::VisibilityRegistry registry;
    registry.Register(provider);

    lighting::VisibilityQuery query{
        .purpose =
            lighting::VisibilityPurpose::
                Shadow,
        .frame =
            bodyRecord->frame,
        .body =
            body,
        .originInFrameMeters =
            {0.0, 0.0, 20.0},
        .direction =
            {0.0F, 0.0F, -1.0F},
        .minimumDistanceMeters =
            0.001F,
        .maximumDistanceMeters =
            100.0F,
        .importance = 1.0F,
        .requirements = {
            .requireOffscreenCoverage =
                true,
            .requirePlanetaryRange =
                true,
            .maximumNominalErrorMeters =
                0.1F,
            .minimumConfidence =
                0.9F
        }
    };

    const auto hit =
        registry.Trace(query);

    if (hit.resolution !=
            lighting::VisibilityResolution::Hit ||
        hit.backend !=
            lighting::VisibilityBackendKind::
                TerrainHeightfield ||
        std::abs(
            hit.hit.distanceMeters -
            8.0F) >
            0.03F ||
        std::abs(
            hit.hit.positionInFrameMeters.z -
            12.0) >
            0.03)
    {
        return 2;
    }

    if (hit.hit.geometricNormal.z < 0.99F)
    {
        return 3;
    }

    query.direction =
        {0.0F, 1.0F, 0.0F};

    const auto miss =
        registry.Trace(query);

    if (miss.resolution !=
        lighting::VisibilityResolution::
            Unresolved)
    {
        return 4;
    }

    // Regression: an origin 3 m above the true ground must not read as buried
    // just because a coarse footprint averages the ground 20 m higher. The ray
    // goes straight up, so it has to be open sky.
    frames::FrameGraph footprintFrames;
    universe::BodyRegistry footprintBodies(footprintFrames);
    const auto footprintSystem =
        footprintBodies.CreateSystem("Footprint System");
    const auto footprintBody =
        footprintBodies.CreateBody({
            .system = footprintSystem,
            .name = "Footprint Sphere",
            .shape =
                universe::SphereShape{
                    .radiusMeters = 1000.0},
            .transformModel =
                universe::FixedBodyTransform{}
        });
    const auto* footprintRecord =
        footprintBodies.FindBody(footprintBody);

    if (footprintRecord == nullptr)
    {
        return 5;
    }

    const world::PlanetDefinition footprintPlanet{
        .radiusMeters = 1000.0,
        .id =
            world::PlanetId{
                .high = 7U,
                .low = 8U}
    };

    FootprintTerrain footprintSource;

    lighting::TerrainHeightfieldVisibilityProvider
        footprintProvider(
            footprintBody,
            footprintPlanet,
            footprintSource,
            footprintBodies,
            footprintFrames,
            {
                .maximumAbsoluteElevationMeters = 100.0,
                .minimumStepMeters = 0.05,
                .maximumStepMeters = 50.0,
                .normalSampleSpacingMeters = 0.1,
                .maximumMarchSteps = 512U,
                .rootRefinementIterations = 16U
            });

    lighting::VisibilityRegistry footprintRegistry;
    footprintRegistry.Register(footprintProvider);

    const lighting::VisibilityQuery skyQuery{
        .purpose =
            lighting::VisibilityPurpose::SkyVisibility,
        .frame = footprintRecord->frame,
        .body = footprintBody,
        .originInFrameMeters = {0.0, 0.0, 1003.0},
        .direction = {0.0F, 0.0F, 1.0F},
        .minimumDistanceMeters = 0.1F,
        .maximumDistanceMeters = 2000.0F,
        .importance = 1.0F,
        .requirements = {
            .requireOffscreenCoverage = true
        }
    };

    const auto openSky =
        footprintRegistry.TraceNearest(skyQuery);

    if (openSky.resolution ==
        lighting::VisibilityResolution::Hit)
    {
        return 6;
    }

    // The same coarse ground is still honoured when the origin really is
    // buried at the fine footprint.
    auto buried = skyQuery;
    buried.originInFrameMeters = {0.0, 0.0, 999.0};

    if (footprintRegistry.TraceNearest(buried).resolution !=
        lighting::VisibilityResolution::Hit)
    {
        return 7;
    }

    return 0;
}
