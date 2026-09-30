#include <orbit/editor_model/PlanetSurface.hpp>

#include <orbit/editor_model/SurfaceAuthoringModel.hpp>
#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <variant>
#include <vector>

namespace orbit::editor_model
{
namespace
{
[[nodiscard]] bool UsesEllipsoid(
    const scene::ObjectStore& objects,
    const scene::ObjectId body)
{
    const auto value = objects.GetProperty(
        body,
        world_model::kBodyEllipsoidEnabled);

    if (!value.has_value())
    {
        return false;
    }

    const auto* enabled = std::get_if<bool>(&*value);
    return enabled != nullptr && *enabled;
}

[[nodiscard]] std::optional<scene::ObjectId> ExistingTerrain(
    const scene::ObjectStore& objects,
    const scene::ObjectId body)
{
    for (const auto& child : objects.Children(body))
    {
        if (child.type == world_model::kTerrainSurfaceType)
        {
            return child.id;
        }
    }

    return std::nullopt;
}

void GatherBodies(
    const scene::ObjectStore& objects,
    const scene::ObjectRecord& record,
    std::vector<scene::ObjectId>& result)
{
    if (record.type == world_model::kCelestialBodyType)
    {
        result.push_back(record.id);
    }

    for (const auto& child : objects.Children(record.id))
    {
        GatherBodies(objects, child, result);
    }
}
} // namespace

bool IsSurfaceEligiblePlanet(
    const scene::ObjectStore& objects,
    const scene::ObjectId body)
{
    const auto record = objects.Find(body);
    if (!record.has_value() ||
        record->type != world_model::kCelestialBodyType ||
        UsesEllipsoid(objects, body))
    {
        return false;
    }

    for (const auto& child : objects.Children(body))
    {
        if (child.type == world_model::kRadiativeEmitterCapabilityType ||
            child.type == world_model::kGiantAppearanceCapabilityType ||
            child.type == world_model::kCompactObjectCapabilityType)
        {
            return false;
        }
    }

    return true;
}

std::optional<scene::ObjectId> EnsureTerrainSurface(
    scene::ObjectStore& objects,
    commands::CommandService& commands,
    selection::SelectionService& selection,
    const scene::ObjectId body)
{
    const auto record = objects.Find(body);
    if (!record.has_value() ||
        record->type != world_model::kCelestialBodyType ||
        UsesEllipsoid(objects, body))
    {
        return std::nullopt;
    }

    if (const auto existing = ExistingTerrain(objects, body);
        existing.has_value())
    {
        return existing;
    }

    const bool ownsTransaction = !commands.HasActiveTransaction();
    if (ownsTransaction)
    {
        commands.BeginTransaction("Create Terrain Surface");
    }

    try
    {
        const auto terrain = commands.CreateObject(
            world_model::kTerrainSurfaceType,
            "Terrain Surface",
            body);

        commands.SetProperty(
            terrain,
            world_model::kTerrainSeed,
            i64{0x41535445525241LL});
        commands.SetProperty(
            terrain,
            world_model::kTerrainMacroAmplitudeMeters,
            1'200.0);
        commands.SetProperty(
            terrain,
            world_model::kTerrainMacroWavelengthMeters,
            800'000.0);
        commands.SetProperty(
            terrain,
            world_model::kTerrainDetailAmplitudeMeters,
            320.0);
        commands.SetProperty(
            terrain,
            world_model::kTerrainDetailWavelengthMeters,
            40'000.0);
        commands.SetProperty(
            terrain,
            world_model::kTerrainDetailOctaves,
            i64{10});
        commands.SetProperty(
            terrain,
            world_model::kTerrainMaximumElevationMeters,
            8'000.0);

        SurfaceAuthoringModel surfaceModel(
            objects,
            commands,
            selection);
        static_cast<void>(surfaceModel.EnsureProcessSettings(terrain));

        if (ownsTransaction)
        {
            commands.CommitTransaction();
        }

        return terrain;
    }
    catch (...)
    {
        if (ownsTransaction && commands.HasActiveTransaction())
        {
            commands.RollbackTransaction();
        }
        throw;
    }
}

bool EnsurePlanetSurface(
    scene::ObjectStore& objects,
    commands::CommandService& commands,
    selection::SelectionService& selection,
    const scene::ObjectId body)
{
    if (!IsSurfaceEligiblePlanet(objects, body) ||
        ExistingTerrain(objects, body).has_value())
    {
        return false;
    }

    return EnsureTerrainSurface(objects, commands, selection, body)
        .has_value();
}

u32 EnsureAllPlanetSurfaces(
    scene::ObjectStore& objects,
    commands::CommandService& commands,
    selection::SelectionService& selection)
{
    std::vector<scene::ObjectId> bodies;
    for (const auto& root : objects.Roots())
    {
        GatherBodies(objects, root, bodies);
    }

    std::vector<scene::ObjectId> missing;
    for (const auto body : bodies)
    {
        if (IsSurfaceEligiblePlanet(objects, body) &&
            !ExistingTerrain(objects, body).has_value())
        {
            missing.push_back(body);
        }
    }

    if (missing.empty())
    {
        return 0U;
    }

    const bool ownsTransaction = !commands.HasActiveTransaction();
    if (ownsTransaction)
    {
        commands.BeginTransaction("Give Planets a Surface");
    }

    try
    {
        u32 created = 0U;
        for (const auto body : missing)
        {
            if (EnsureTerrainSurface(objects, commands, selection, body)
                    .has_value())
            {
                ++created;
            }
        }

        if (ownsTransaction)
        {
            commands.CommitTransaction();
        }

        return created;
    }
    catch (...)
    {
        if (ownsTransaction && commands.HasActiveTransaction())
        {
            commands.RollbackTransaction();
        }
        throw;
    }
}
} // namespace orbit::editor_model
