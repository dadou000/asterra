#include <orbit/surface_model/SurfaceComposition.hpp>

#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace orbit::surface_model
{
namespace
{
template <typename Value>
[[nodiscard]] Value PropertyOr(
    const scene::ObjectStore& objects,
    const scene::ObjectId object,
    const schema::PropertyId property,
    Value fallback)
{
    const auto stored = objects.GetProperty(object, property);

    if (!stored.has_value())
    {
        return fallback;
    }

    const auto* value = std::get_if<Value>(&*stored);

    if (value == nullptr)
    {
        throw std::runtime_error(
            "Terrain surface property has an unexpected persisted type on object " +
            object.ToString() + ".");
    }

    return *value;
}

[[nodiscard]] terrain::AnalyticTerrainDesc TerrainDescription(
    const scene::ObjectStore& objects,
    const scene::ObjectId object)
{
    terrain::AnalyticTerrainDesc result;

    const i64 seed = PropertyOr<i64>(
        objects,
        object,
        world_model::kTerrainSeed,
        i64{0x41535445525241LL});
    const i64 octaves = PropertyOr<i64>(
        objects,
        object,
        world_model::kTerrainDetailOctaves,
        i64{10});

    if (seed < 0)
    {
        throw std::runtime_error(
            "Terrain seed must be non-negative on object " +
            object.ToString() + ".");
    }

    if (octaves < 1 || octaves > 16)
    {
        throw std::runtime_error(
            "Terrain detail octave count must be between 1 and 16 on object " +
            object.ToString() + ".");
    }

    result.seed = static_cast<u64>(seed);
    result.macroAmplitudeMeters = PropertyOr<f64>(
        objects,
        object,
        world_model::kTerrainMacroAmplitudeMeters,
        1'200.0);
    result.macroWavelengthMeters = PropertyOr<f64>(
        objects,
        object,
        world_model::kTerrainMacroWavelengthMeters,
        800'000.0);
    result.detailAmplitudeMeters = PropertyOr<f64>(
        objects,
        object,
        world_model::kTerrainDetailAmplitudeMeters,
        320.0);
    result.detailWavelengthMeters = PropertyOr<f64>(
        objects,
        object,
        world_model::kTerrainDetailWavelengthMeters,
        40'000.0);
    result.detailOctaves = static_cast<u32>(octaves);
    result.maximumElevationAboveSeaLevelMeters = PropertyOr<f64>(
        objects,
        object,
        world_model::kTerrainMaximumElevationMeters,
        8'000.0);

    if (result.macroAmplitudeMeters < 0.0 ||
        result.detailAmplitudeMeters < 0.0 ||
        result.macroWavelengthMeters <= 0.0 ||
        result.detailWavelengthMeters <= 0.0 ||
        result.maximumElevationAboveSeaLevelMeters <= 0.0)
    {
        throw std::runtime_error(
            "Terrain surface recipe contains invalid dimensions on object " +
            object.ToString() + ".");
    }

    return result;
}
} // namespace

SurfaceCompositionStats SurfaceComposition::Rebuild(
    const scene::ObjectStore& objects,
    const world_model::UniverseComposition& universe)
{
    // UniverseComposition replaces BodyRegistry storage during a rebuild.
    // Drop every reference to the previous registry before validating the
    // replacement capability graph; a failed terrain recipe therefore leaves
    // the surface layer unavailable rather than dangling into freed bodies.
    registry_.reset();
    bodyByTerrainObject_.clear();
    terrainObjectByBody_.clear();
    sourceRevision_ = ~u64{0};

    auto candidate =
        std::make_unique<surface::SurfaceRegistry>(
            universe.Bodies());
    std::unordered_map<scene::ObjectId, universe::BodyId>
        candidateBodyByObject;
    std::unordered_map<universe::BodyId, scene::ObjectId>
        candidateObjectByBody;

    std::vector<scene::ObjectRecord> pending = objects.Roots();
    u32 terrainCount = 0;

    while (!pending.empty())
    {
        const auto object = pending.back();
        pending.pop_back();

        auto children = objects.Children(object.id);
        pending.insert(
            pending.end(),
            children.begin(),
            children.end());

        if (object.type != world_model::kTerrainSurfaceType)
        {
            continue;
        }

        if (!object.parent.has_value())
        {
            throw std::runtime_error(
                "Terrain Surface must be parented to a Celestial Body.");
        }

        const auto parent = objects.Find(*object.parent);

        if (!parent.has_value() ||
            parent->type != world_model::kCelestialBodyType)
        {
            throw std::runtime_error(
                "Terrain Surface parent must be a Celestial Body.");
        }

        const auto bodyId = universe.BodyForObject(parent->id);

        if (!bodyId.has_value())
        {
            throw std::runtime_error(
                "Terrain Surface parent is not present in UniverseComposition.");
        }

        if (candidateObjectByBody.contains(*bodyId))
        {
            throw std::runtime_error(
                "A Celestial Body may own only one Terrain Surface capability.");
        }

        const auto planet = candidate->SphericalPlanetDefinition(*bodyId);

        if (!planet.has_value())
        {
            throw std::runtime_error(
                "Analytic terrain currently requires a spherical Celestial Body; ellipsoid terrain must use a future ellipsoid-aware terrain source.");
        }

        auto source =
            std::make_shared<terrain::AnalyticTerrainSource>(
                *planet,
                TerrainDescription(objects, object.id));

        candidate->AttachTerrain(*bodyId, std::move(source));
        candidateBodyByObject.emplace(object.id, *bodyId);
        candidateObjectByBody.emplace(*bodyId, object.id);
        ++terrainCount;
    }

    registry_ = std::move(candidate);
    bodyByTerrainObject_ = std::move(candidateBodyByObject);
    terrainObjectByBody_ = std::move(candidateObjectByBody);
    sourceRevision_ = objects.Revision();

    return {
        .terrainSurfaces = terrainCount,
        .sourceRevision = sourceRevision_
    };
}

surface::SurfaceRegistry& SurfaceComposition::Registry()
{
    if (registry_ == nullptr)
    {
        throw std::logic_error(
            "SurfaceComposition has not been built for an active universe.");
    }

    return *registry_;
}

const surface::SurfaceRegistry& SurfaceComposition::Registry() const
{
    if (registry_ == nullptr)
    {
        throw std::logic_error(
            "SurfaceComposition has not been built for an active universe.");
    }

    return *registry_;
}

std::optional<universe::BodyId>
SurfaceComposition::BodyForTerrainObject(
    const scene::ObjectId object) const noexcept
{
    const auto found = bodyByTerrainObject_.find(object);
    return found != bodyByTerrainObject_.end()
        ? std::optional<universe::BodyId>(found->second)
        : std::nullopt;
}

std::optional<scene::ObjectId>
SurfaceComposition::TerrainObjectForBody(
    const universe::BodyId body) const noexcept
{
    const auto found = terrainObjectByBody_.find(body);
    return found != terrainObjectByBody_.end()
        ? std::optional<scene::ObjectId>(found->second)
        : std::nullopt;
}

u64 SurfaceComposition::SourceRevision() const noexcept
{
    return sourceRevision_;
}
} // namespace orbit::surface_model
