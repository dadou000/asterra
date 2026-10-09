#include <orbit/studio_session/StudioTerrainAuthoringInvalidation.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_set>

namespace orbit::studio_session
{
namespace
{
constexpr f64 Pi =
    3.1415926535897932384626433832795;

[[nodiscard]] u64 MixImpact(const terrain_impacts::ImpactRecord& event) noexcept
{
    u64 hash = terrain::StableCombine64(event.id.high, event.id.low);
    const auto mix = [&](const u64 value) { hash = terrain::StableCombine64(hash, value); };
    for (const f64 value : {
             event.centerUnitDirection.x, event.centerUnitDirection.y,
             event.centerUnitDirection.z, event.radiusMeters,
             event.simpleDepthRatio, event.complexDepthRatio, event.rimHeightRatio,
             event.ejectaThicknessRatio, event.ejectaExtentRadii, event.rayStrength,
             event.degradation, event.formationAgeYears, event.impactAngleDegrees,
             event.impactAzimuthRadians, event.shapeIrregularity, event.meltFraction,
             event.brecciaFraction, event.multiringStrength,
             event.impactorDiameterMeters, event.impactVelocityMetersPerSecond,
             event.impactorDensityKgPerCubicMeter, event.binarySeparationRadii,
             event.binaryCompanionRadiusRatio, event.binaryAzimuthRadians,
             event.secondaryRadiusRatio, event.secondaryRayAlignment})
        mix(std::bit_cast<u64>(value));
    mix(static_cast<u64>(event.profile));
    mix(event.rayCount);
    mix(event.ageOrder);
    mix(event.secondaryCount);
    mix(event.enabled ? 1U : 0U);
    mix(event.authored ? 1U : 0U);
    return hash;
}

[[nodiscard]] u64 MixResurfacing(const terrain_impacts::ResurfacingRecord& event) noexcept
{
    u64 hash = terrain::StableCombine64(event.id.high, event.id.low);
    hash = terrain::StableCombine64(hash, static_cast<u64>(event.kind));
    hash = terrain::StableCombine64(hash, std::bit_cast<u64>(event.widthMeters));
    hash = terrain::StableCombine64(hash, std::bit_cast<u64>(event.thicknessMeters));
    hash = terrain::StableCombine64(hash, std::bit_cast<u64>(event.formationAgeYears));
    hash = terrain::StableCombine64(hash, event.ageOrder);
    hash = terrain::StableCombine64(hash, event.enabled ? 1U : 0U);
    for (const auto& point : event.centerlineUnitDirections)
    {
        hash = terrain::StableCombine64(hash, std::bit_cast<u64>(point.x));
        hash = terrain::StableCombine64(hash, std::bit_cast<u64>(point.y));
        hash = terrain::StableCombine64(hash, std::bit_cast<u64>(point.z));
    }
    return hash;
}

[[nodiscard]] u64 MixGlobalImpactRecipe(
    const terrain_impacts::ImpactFieldDefinition& recipe) noexcept
{
    u64 hash = terrain::StableCombine64(recipe.seed, static_cast<u64>(recipe.environment));
    for (const f64 value : {recipe.surfaceAgeYears,
             recipe.surfaceGravityMetersPerSecondSquared,
             recipe.targetDensityKgPerCubicMeter, recipe.targetStrengthPascals,
             recipe.complexTransitionRadiusMeters,
             recipe.procedural.minimumRadiusMeters,
             recipe.procedural.maximumRadiusMeters,
             recipe.procedural.cumulativeExponent})
        hash = terrain::StableCombine64(hash, std::bit_cast<u64>(value));
    hash = terrain::StableCombine64(hash, recipe.procedural.count);
    if (recipe.iceFractures != nullptr)
    {
        const auto& ice = *recipe.iceFractures;
        hash = terrain::StableCombine64(hash, ice.seed);
        hash = terrain::StableCombine64(hash, ice.fractureCount);
        hash = terrain::StableCombine64(hash, ice.segmentsPerFracture);
        for (const f64 value : {ice.tidalAxis.x, ice.tidalAxis.y, ice.tidalAxis.z,
                 ice.spinAxis.x, ice.spinAxis.y, ice.spinAxis.z, ice.tidalStress,
                 ice.rotationalStress, ice.tensileStrength, ice.maximumLengthMeters,
                 ice.widthMeters, ice.grooveDepthMeters, ice.ridgeHeightMeters,
                 ice.branchProbability})
            hash = terrain::StableCombine64(hash, std::bit_cast<u64>(value));
        hash = terrain::StableCombine64(hash, ice.enabled ? 1U : 0U);
    }
    return hash;
}

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

std::vector<terrain_dependency::TerrainInvalidationRequest>
BuildImpactHistoryInvalidations(
    const world::PlanetDefinition& planet,
    const std::string_view previousToml,
    const std::string_view nextToml,
    const u8 physicalTileLevel,
    const u32 downstreamRadiusTiles)
{
    std::optional<terrain_impacts::ImpactFieldDefinition> previous;
    std::optional<terrain_impacts::ImpactFieldDefinition> next;
    if (!previousToml.empty()) previous = terrain_impacts::ParseImpactFieldToml(previousToml);
    if (!nextToml.empty()) next = terrain_impacts::ParseImpactFieldToml(nextToml);
    const auto validForPlanet = [&](const auto& recipe)
    {
        return !recipe.has_value() || recipe->planet == planet.id;
    };
    if (!validForPlanet(previous) || !validForPlanet(next))
        throw std::invalid_argument("Impact history invalidation recipe belongs to another planet.");

    const auto globalHash = [](const auto& recipe)
    {
        return recipe.has_value()
            ? MixGlobalImpactRecipe(*recipe)
            : MixGlobalImpactRecipe(terrain_impacts::ImpactFieldDefinition{});
    };
    if (globalHash(previous) != globalHash(next))
    {
        return {terrain_dependency::TerrainInvalidationRequest{
            .kind = terrain_dependency::TerrainChangeKind::TerrainAuthoring,
            .scope = {.planet = planet.id, .global = true}}};
    }

    using Id = std::pair<u64, u64>;
    std::map<Id, u64> priorImpacts;
    std::map<Id, u64> nextImpacts;
    std::map<Id, u64> priorFlows;
    std::map<Id, u64> nextFlows;
    if (previous.has_value())
    {
        for (const auto& event : previous->authoredImpacts)
            priorImpacts[{event.id.high, event.id.low}] = MixImpact(event);
        for (const auto& event : previous->resurfacingEvents)
            priorFlows[{event.id.high, event.id.low}] = MixResurfacing(event);
    }
    if (next.has_value())
    {
        for (const auto& event : next->authoredImpacts)
            nextImpacts[{event.id.high, event.id.low}] = MixImpact(event);
        for (const auto& event : next->resurfacingEvents)
            nextFlows[{event.id.high, event.id.low}] = MixResurfacing(event);
    }

    std::vector<terrain_dependency::TerrainInvalidationRequest> result;
    std::set<Id> processedImpacts;
    std::set<Id> processedFlows;
    const auto changed = [](const auto& a, const auto& b, const Id& id)
    {
        const auto first = a.find(id);
        const auto second = b.find(id);
        return first == a.end() || second == b.end() || first->second != second->second;
    };
    const auto appendRegion = [&](const std::span<const math::Double3> points, const f64 influence)
    {
        auto local = BuildTerrainAuthoringInvalidations(
            planet, points, influence, physicalTileLevel, downstreamRadiusTiles,
            terrain_dependency::TerrainChangeKind::TerrainAuthoring);
        result.insert(result.end(), local.begin(), local.end());
    };
    for (const auto* recipe : {previous.has_value() ? &*previous : nullptr,
                                next.has_value() ? &*next : nullptr})
    {
        if (recipe == nullptr) continue;
        for (const auto& event : recipe->authoredImpacts)
        {
            const Id id{event.id.high, event.id.low};
            if (!changed(priorImpacts, nextImpacts, id) || !processedImpacts.insert(id).second) continue;
            const math::Double3 point = event.centerUnitDirection;
            const f64 influence = event.radiusMeters * std::max(
                1.0, event.ejectaExtentRadii + event.binarySeparationRadii + 1.0);
            appendRegion(std::span<const math::Double3>(&point, 1U), influence);
        }
        for (const auto& event : recipe->resurfacingEvents)
        {
            const Id id{event.id.high, event.id.low};
            if (!changed(priorFlows, nextFlows, id) || !processedFlows.insert(id).second) continue;
            appendRegion(event.centerlineUnitDirections, event.widthMeters * 0.5);
        }
    }
    return result;
}
} // namespace orbit::studio_session
