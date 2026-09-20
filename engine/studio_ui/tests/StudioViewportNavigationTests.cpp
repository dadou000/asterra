#include <orbit/studio_ui/StudioViewportNavigation.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace
{
void Check(const bool condition)
{
    if (!condition)
    {
        std::cerr << "Studio viewport navigation test failed.\n";
        std::exit(1);
    }
}

class FlatTerrainSource final
    : public orbit::terrain::TerrainSource
{
public:
    FlatTerrainSource(
        const orbit::f64 elevationMeters,
        const orbit::u64 revision) noexcept
        : elevationMeters_(elevationMeters),
          revision_(revision)
    {
    }

    [[nodiscard]] orbit::terrain::TerrainSample
    Sample(
        const orbit::terrain::TerrainQuery&) const noexcept override
    {
        return {
            .elevationMeters =
                elevationMeters_,
            .coarseElevationMeters =
                elevationMeters_
        };
    }

    [[nodiscard]] orbit::u64 Revision() const noexcept override
    {
        return revision_;
    }

private:
    orbit::f64 elevationMeters_{0.0};
    orbit::u64 revision_{0U};
};

[[nodiscard]] orbit::studio_session::
    StudioTerrainViewportRuntimeSnapshot
TerrainAtAltitude(const orbit::f64 altitudeMeters)
{
    return {
        .viewportId = "studio.primary",
        .worldGeneration = 3,
        .universeGeneration = 7,
        .runtimeGeneration = 11,
        .semanticBody = {
            .high = 1,
            .low = 2
        },
        .body = {
            .high = 3,
            .low = 4
        },
        .terrainObject = {
            .high = 5,
            .low = 6
        },
        .planet = {
            .radiusMeters =
                6'000'000.0,
            .id = {
                .high = 3,
                .low = 4
            }
        },
        .observer = {
            .meters = {
                6'000'000.0 +
                    altitudeMeters,
                0.0,
                0.0
            }
        }
    };
}

[[nodiscard]] orbit::f64 SurfaceDistance(
    const orbit::world::PlanetDefinition& planet,
    const orbit::math::Double3& a,
    const orbit::math::Double3& b)
{
    const orbit::f64 cosine =
        std::clamp(
            orbit::math::Dot(
                orbit::math::Normalize(a),
                orbit::math::Normalize(b)),
            -1.0,
            1.0);

    return
        std::acos(cosine) *
        planet.radiusMeters;
}
} // namespace

int main()
{
    FlatTerrainSource source(
        125.0,
        91U);

    {
        auto terrain =
            TerrainAtAltitude(
                1'125.0);

        orbit::studio_ui::
            StudioTerrainNavigationState
                state;

        const auto current =
            orbit::studio_ui::
                CurrentTerrainNavigation(
                    state,
                    terrain,
                    source);

        Check(
            std::abs(
                current.
                    altitudeAboveTerrainMeters -
                1'000.0) <
            1.0e-6);

        const orbit::u64 revisionBefore =
            source.Revision();

        const auto moved =
            orbit::studio_ui::
                AdvanceTerrainNavigation(
                    state,
                    terrain,
                    source,
                    {
                        .deltaSeconds =
                            1.0,
                        .moveForward =
                            1.0
                    });

        Check(moved.moved);
        Check(
            SurfaceDistance(
                terrain.planet,
                terrain.observer.meters,
                moved.observer.meters) >
            0.5);
        Check(
            orbit::math::Length(
                moved.observer.meters) >
            terrain.planet.radiusMeters +
                125.0);
        Check(
            source.Revision() ==
            revisionBefore);
    }

    {
        auto nearTerrain =
            TerrainAtAltitude(
                135.0);
        auto orbitTerrain =
            TerrainAtAltitude(
                1'000'125.0);

        orbit::studio_ui::
            StudioTerrainNavigationState
                nearState;
        orbit::studio_ui::
            StudioTerrainNavigationState
                orbitState;

        const auto nearMove =
            orbit::studio_ui::
                AdvanceTerrainNavigation(
                    nearState,
                    nearTerrain,
                    source,
                    {
                        .deltaSeconds = 1.0,
                        .moveForward = 1.0
                    });

        const auto orbitMove =
            orbit::studio_ui::
                AdvanceTerrainNavigation(
                    orbitState,
                    orbitTerrain,
                    source,
                    {
                        .deltaSeconds = 1.0,
                        .moveForward = 1.0
                    });

        const orbit::f64 nearDistance =
            SurfaceDistance(
                nearTerrain.planet,
                nearTerrain.observer.meters,
                nearMove.observer.meters);

        const orbit::f64 orbitDistance =
            SurfaceDistance(
                orbitTerrain.planet,
                orbitTerrain.observer.meters,
                orbitMove.observer.meters);

        Check(
            orbitDistance >
            nearDistance * 50.0);
    }

    {
        auto terrain =
            TerrainAtAltitude(
                10'125.0);

        orbit::studio_ui::
            StudioTerrainNavigationState
                state;

        const auto focused =
            orbit::studio_ui::
                FocusTerrainSurface(
                    state,
                    terrain,
                    source,
                    {0.0, 0.0, 1.0});

        Check(
            orbit::math::Dot(
                orbit::math::Normalize(
                    focused.observer.meters),
                orbit::math::Double3{
                    0.0,
                    0.0,
                    1.0}) >
            0.999999);

        Check(
            std::abs(
                focused.
                    altitudeAboveTerrainMeters -
                state.config.
                    focusSurfaceHeightMeters) <
            1.0e-6);

        Check(
            focused.localCamera.forward.y <
            -0.45F);
    }

    {
        auto terrain =
            TerrainAtAltitude(
                10'125.0);

        orbit::studio_ui::
            StudioTerrainNavigationState
                state;

        const auto focused =
            orbit::studio_ui::
                FocusTerrainBody(
                    state,
                    terrain,
                    source);

        Check(
            focused.
                altitudeAboveTerrainMeters >=
            state.config.
                focusOrbitMinimumAltitudeMeters);

        Check(
            focused.localCamera.forward.y <
            -0.8F);
    }

    {
        const orbit::world::PlanetDefinition planet{
            .radiusMeters =
                6'000'000.0
        };

        const auto ground =
            orbit::studio_ui::
                SurfaceSafeTerrainClipPlanes(
                    planet,
                    2.0);

        const auto orbit =
            orbit::studio_ui::
                SurfaceSafeTerrainClipPlanes(
                    planet,
                    1'000'000.0);

        Check(
            ground.nearPlaneMeters >
            0.0F);
        Check(
            ground.farPlaneMeters >
            ground.nearPlaneMeters);
        Check(
            orbit.nearPlaneMeters >
            ground.nearPlaneMeters);
        Check(
            orbit.farPlaneMeters >
            ground.farPlaneMeters);
    }

    return 0;
}
