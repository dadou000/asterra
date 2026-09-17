#include <orbit/math/Vector.hpp>
#include <orbit/terrain/TerrainFields.hpp>
#include <orbit/terrain_cache/TerrainPage.hpp>
#include <orbit/world/Planet.hpp>

#include <cmath>
#include <iostream>

namespace
{
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

[[nodiscard]] orbit::terrain_cache::TerrainPage ConstantPage(
    const orbit::world::CubeFace face,
    const orbit::f64 elevationMeters)
{
    using namespace orbit;

    terrain::TerrainSample sample{};
    sample.elevationMeters = elevationMeters;
    sample.coarseElevationMeters = elevationMeters;

    terrain_cache::TerrainPage page{};
    page.desc = {
        .tile = {
            .face = face,
            .level = 0,
            .x = 0,
            .y = 0
        },
        .resolution = 2
    };

    page.samples.assign(
        4,
        terrain_cache::ToCachedSample(sample));

    return page;
}
} // namespace

int main()
{
    using namespace orbit;

    const terrain_cache::TerrainPage positiveX =
        ConstantPage(
            world::CubeFace::PositiveX,
            42.0);

    const terrain_cache::TerrainPage positiveY =
        ConstantPage(
            world::CubeFace::PositiveY,
            42.0);

    const terrain_cache::TerrainPage positiveZ =
        ConstantPage(
            world::CubeFace::PositiveZ,
            42.0);

    const math::Double3 xzEdge =
        math::Normalize(
            math::Double3{1.0, 0.0, 1.0});

    const auto fromX =
        positiveX.SampleDirection(xzEdge);
    const auto fromZ =
        positiveZ.SampleDirection(xzEdge);

    bool ok = true;
    ok &= Check(
        std::abs(
            fromX.elevationMeters -
            42.0) < 1.0e-6,
        "Positive-X page must sample its X/Z boundary.");
    ok &= Check(
        std::abs(
            fromZ.elevationMeters -
            42.0) < 1.0e-6,
        "Positive-Z page must sample the same X/Z boundary.");
    ok &= Check(
        std::abs(
            fromX.elevationMeters -
            fromZ.elevationMeters) < 1.0e-6,
        "Adjacent cube pages must return equivalent edge samples.");

    const math::Double3 xyzCorner =
        math::Normalize(
            math::Double3{1.0, 1.0, 1.0});

    const auto cornerX =
        positiveX.SampleDirection(xyzCorner);
    const auto cornerY =
        positiveY.SampleDirection(xyzCorner);
    const auto cornerZ =
        positiveZ.SampleDirection(xyzCorner);

    ok &= Check(
        std::abs(cornerX.elevationMeters - 42.0) < 1.0e-6 &&
            std::abs(cornerY.elevationMeters - 42.0) < 1.0e-6 &&
            std::abs(cornerZ.elevationMeters - 42.0) < 1.0e-6,
        "All three cube pages sharing a corner must accept that physical sample.");

    return ok ? 0 : 1;
}
