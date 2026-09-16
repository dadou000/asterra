#include <orbit/surface/SurfaceRegistry.hpp>

#include <orbit/terrain/TerrainSource.hpp>

#include <cassert>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace
{
class FlatTerrain final
    : public orbit::terrain::TerrainSource
{
public:
    [[nodiscard]] orbit::terrain::TerrainSample
    Sample(
        const orbit::terrain::TerrainQuery&)
        const noexcept override
    {
        return {};
    }
};
}

int main()
{
    orbit::frames::FrameGraph frames;
    orbit::universe::BodyRegistry bodies(
        frames);

    const auto system =
        bodies.CreateSystem("Test");

    const auto sphere =
        bodies.CreateBody({
            .system = system,
            .name = "Sphere",
            .shape =
                orbit::universe::SphereShape{
                    .radiusMeters =
                        1'000.0
                }
        });

    const auto ellipsoid =
        bodies.CreateBody({
            .system = system,
            .name = "Ellipsoid",
            .shape =
                orbit::universe::
                    EllipsoidShape{
                        .radiiMeters = {
                            2'000.0,
                            1'000.0,
                            500.0
                        }
                    }
        });

    orbit::surface::SurfaceRegistry
        surfaces(bodies);

    assert(
        surfaces.FindTerrainSurface(
            sphere) == nullptr);
    assert(
        surfaces.FindTerrainSurface(
            ellipsoid) == nullptr);

    const auto spherePoint =
        surfaces.BodyPointFromSurface(
            sphere,
            {
                .unitDirection = {
                    1.0,
                    0.0,
                    0.0
                },
                .radialOffsetMeters =
                    25.0
            });

    assert(spherePoint.has_value());
    assert(
        std::abs(
            spherePoint->x -
            1'025.0) <
        1e-9);

    const auto ellipsoidPoint =
        surfaces.BodyPointFromSurface(
            ellipsoid,
            {
                .unitDirection = {
                    1.0,
                    0.0,
                    0.0
                },
                .radialOffsetMeters =
                    10.0
            });

    assert(ellipsoidPoint.has_value());
    assert(
        std::abs(
            ellipsoidPoint->x -
            2'010.0) <
        1e-9);

    const auto roundTrip =
        surfaces.SurfaceFromBodyPoint(
            ellipsoid,
            *ellipsoidPoint);

    assert(roundTrip.has_value());
    assert(
        std::abs(
            roundTrip->
                radialOffsetMeters -
            10.0) <
        1e-9);

    auto flat =
        std::make_shared<FlatTerrain>();

    surfaces.AttachTerrain(
        sphere,
        flat);

    assert(
        surfaces.FindTerrainSurface(
            sphere) != nullptr);

    bool rejectedEllipsoid = false;

    try
    {
        surfaces.AttachTerrain(
            ellipsoid,
            flat);
    }
    catch (const std::invalid_argument&)
    {
        rejectedEllipsoid = true;
    }

    assert(rejectedEllipsoid);

    // An ordinary celestial body remains valid without any attached
    // terrain capability.
    assert(
        surfaces.FindTerrainSurface(
            ellipsoid) == nullptr);

    return 0;
}
