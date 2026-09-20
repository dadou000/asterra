#include <orbit/studio_session/StudioTerrainAuthoringInvalidation.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace orbit::studio_session
{
namespace
{
constexpr f64 Pi =
    3.1415926535897932384626433832795;

[[nodiscard]] math::Double3 Unit(
    const math::Double3& direction)
{
    const f64 length =
        math::Length(direction);

    if (!std::isfinite(length) ||
        length <= 1.0e-12)
    {
        throw std::invalid_argument(
            "Terrain authoring invalidation requires finite non-zero directions.");
    }

    return direction / length;
}

[[nodiscard]] f64 AngularDistance(
    const math::Double3& a,
    const math::Double3& b) noexcept
{
    return std::acos(
        std::clamp(
            math::Dot(a, b),
            -1.0,
            1.0));
}

[[nodiscard]] math::Double3 Slerp(
    const math::Double3& a,
    const math::Double3& b,
    const f64 t) noexcept
{
    const f64 angle =
        AngularDistance(a, b);

    if (angle <= 1.0e-12)
    {
        return a;
    }

    const f64 sine =
        std::sin(angle);

    if (std::abs(sine) <= 1.0e-12)
    {
        return math::Normalize(
            a * (1.0 - t) +
            b * t);
    }

    return math::Normalize(
        a *
            (std::sin((1.0 - t) * angle) /
             sine) +
        b *
            (std::sin(t * angle) /
             sine));
}

struct TileHash
{
    [[nodiscard]] std::size_t operator()(
        const world::PlanetTileId& tile) const noexcept
    {
        std::size_t value =
            static_cast<std::size_t>(
                tile.level);

        value =
            value * 1315423911U +
            static_cast<std::size_t>(
                tile.face);
        value =
            value * 1315423911U +
            static_cast<std::size_t>(
                tile.x);
        value =
            value * 1315423911U +
            static_cast<std::size_t>(
                tile.y);
        return value;
    }
};
} // namespace

std::vector<
    terrain_dependency::TerrainInvalidationRequest>
BuildTerrainAuthoringInvalidations(
    const world::PlanetDefinition& planet,
    const std::span<
        const math::Double3>
        controlUnitDirections,
    const f64 influenceRadiusMeters,
    const u8 physicalTileLevel,
    const u32 downstreamRadiusTiles,
    const terrain_dependency::TerrainChangeKind kind)
{
    if (!planet.id.IsValid() ||
        !std::isfinite(planet.radiusMeters) ||
        planet.radiusMeters <= 0.0 ||
        !std::isfinite(influenceRadiusMeters) ||
        influenceRadiusMeters < 0.0 ||
        controlUnitDirections.empty() ||
        physicalTileLevel > 30U ||
        downstreamRadiusTiles > 64U)
    {
        throw std::invalid_argument(
            "Terrain authoring invalidation input is invalid.");
    }

    std::vector<math::Double3> points;
    points.reserve(
        controlUnitDirections.size());

    for (const auto& point :
         controlUnitDirections)
    {
        points.push_back(
            Unit(point));
    }

    const u64 tilesPerFace =
        u64{1} <<
        physicalTileLevel;

    // Cube-map tiles are not angularly uniform. This is intentionally smaller
    // than face-average span so bounds over-cover face corners/seams.
    const f64 conservativeTileMeters =
        std::max(
            planet.radiusMeters /
                static_cast<f64>(
                    tilesPerFace) *
                0.50,
            0.001);

    const u32 maximumLocalRadius =
        downstreamRadiusTiles >= 64U
            ? 0U
            : 64U -
                downstreamRadiusTiles;

    if (maximumLocalRadius == 0U &&
        influenceRadiusMeters > 0.0)
    {
        throw std::invalid_argument(
            "Downstream invalidation radius leaves no room for authored footprint.");
    }

    const u32 requestedRadius =
        std::max<u32>(
            1U,
            static_cast<u32>(
                std::ceil(
                    influenceRadiusMeters /
                    conservativeTileMeters)) +
                1U);

    const u32 radiusTiles =
        std::min(
            requestedRadius,
            std::max<u32>(
                maximumLocalRadius,
                1U));

    const f64 coverageMeters =
        std::max(
            conservativeTileMeters,
            static_cast<f64>(
                radiusTiles) *
                conservativeTileMeters);

    const f64 maximumAngularStep =
        std::clamp(
            coverageMeters /
                planet.radiusMeters,
            1.0e-9,
            Pi * 0.25);

    std::vector<math::Double3> samples;

    if (points.size() == 1U)
    {
        samples.push_back(
            points.front());
    }
    else
    {
        samples.push_back(
            points.front());

        for (std::size_t index = 1U;
             index < points.size();
             ++index)
        {
            const f64 angle =
                AngularDistance(
                    points[index - 1U],
                    points[index]);

            const u32 steps =
                std::max<u32>(
                    1U,
                    static_cast<u32>(
                        std::ceil(
                            angle /
                            maximumAngularStep)));

            for (u32 step = 1U;
                 step <= steps;
                 ++step)
            {
                samples.push_back(
                    Slerp(
                        points[index - 1U],
                        points[index],
                        static_cast<f64>(step) /
                            static_cast<f64>(
                                steps)));
            }
        }
    }

    std::unordered_set<
        world::PlanetTileId,
        TileHash>
        seen;

    std::vector<
        terrain_dependency::
            TerrainInvalidationRequest>
        result;

    result.reserve(
        samples.size());

    for (const auto& sample :
         samples)
    {
        const auto tile =
            world::TileForDirection(
                sample,
                physicalTileLevel);

        if (!seen.insert(tile).second)
        {
            continue;
        }

        terrain_dependency::
            TerrainInvalidationRequest
            request{
                .kind = kind,
                .scope = {
                    .planet =
                        planet.id,
                    .global =
                        false,
                    .center =
                        tile,
                    .radiusTiles =
                        radiusTiles,
                    .downstreamRadiusTiles =
                        downstreamRadiusTiles
                }
            };

        if (!request.scope.IsValid())
        {
            throw std::logic_error(
                "M09 produced an invalid bounded M27 terrain scope.");
        }

        result.push_back(
            request);
    }

    return result;
}
} // namespace orbit::studio_session
