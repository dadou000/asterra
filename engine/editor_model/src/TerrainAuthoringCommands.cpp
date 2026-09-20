#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/editor_model/SurfaceAuthoringModel.hpp>

#include <orbit/world_model/WorldSchemas.hpp>

#include <optional>
#include <stdexcept>
#include <variant>

namespace orbit::editor_model::authoring_commands
{
namespace
{
[[nodiscard]] bool BodyUsesEllipsoid(
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

    if (enabled == nullptr)
    {
        throw std::runtime_error(
            "Celestial Body ellipsoid property has an invalid persisted type.");
    }

    return *enabled;
}

[[nodiscard]] std::optional<scene::ObjectId>
TerrainSurfaceObject(
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

[[nodiscard]] commands::CommandEnablement CreateTerrainEnablement(
    const scene::ObjectStore& objects,
    const selection::SelectionService& selection)
{
    if (selection.Ordered().size() != 1U)
    {
        return {
            .enabled = false,
            .reason = "Select exactly one Celestial Body."
        };
    }

    const auto object = objects.Find(selection.Ordered().front());

    if (!object.has_value() ||
        object->type != world_model::kCelestialBodyType)
    {
        return {
            .enabled = false,
            .reason = "Select a Celestial Body."
        };
    }

    if (BodyUsesEllipsoid(objects, object->id))
    {
        return {
            .enabled = false,
            .reason =
                "Analytic terrain currently requires a spherical body."
        };
    }

    if (TerrainSurfaceObject(objects, object->id).has_value())
    {
        return {
            .enabled = false,
            .reason =
                "This Celestial Body already owns a Terrain Surface capability."
        };
    }

    return {};
}

[[nodiscard]] commands::CommandEnablement RemoveTerrainEnablement(
    const scene::ObjectStore& objects,
    const selection::SelectionService& selection)
{
    if (selection.Ordered().size() != 1U)
    {
        return {
            .enabled = false,
            .reason = "Select exactly one Celestial Body."
        };
    }

    const auto object = objects.Find(selection.Ordered().front());

    if (!object.has_value() ||
        object->type != world_model::kCelestialBodyType)
    {
        return {
            .enabled = false,
            .reason = "Select a Celestial Body."
        };
    }

    const auto terrain = TerrainSurfaceObject(objects, object->id);

    if (!terrain.has_value())
    {
        return {
            .enabled = false,
            .reason =
                "This Celestial Body has no Terrain Surface capability."
        };
    }

    u32 processSettings = 0U;

    for (const auto& child :
         objects.Children(*terrain))
    {
        if (child.type ==
            world_model::kTerrainProcessAssetType)
        {
            ++processSettings;
            continue;
        }

        return {
            .enabled = false,
            .reason =
                "The Terrain Surface has authored semantic children and cannot be removed."
        };
    }

    if (processSettings > 1U)
    {
        return {
            .enabled = false,
            .reason =
                "The Terrain Surface contains duplicate process settings and must be repaired before removal."
        };
    }

    return {};
}
} // namespace

void RegisterTerrainCommands(
    commands::CommandRegistry& registry,
    commands::CommandService& commandService,
    scene::ObjectStore& objects,
    selection::SelectionService& selection)
{
    registry.Register({
        .id = kCreateTerrainSurface,
        .name = "Create Terrain Surface",
        .category = "World / Surface",
        .description =
            "Attach an authored analytic terrain capability to the selected spherical Celestial Body.",
        .presentationSurfaces = {
            "explorer.context",
            "properties.toolbar"
        },
        .enablement =
            [&objects, &selection]
            {
                return CreateTerrainEnablement(objects, selection);
            },
        .invoke =
            [&objects,
             &commandService,
             &selection](
                const commands::CommandArguments&)
            {
                const auto enabled =
                    CreateTerrainEnablement(objects, selection);

                if (!enabled.enabled)
                {
                    throw std::invalid_argument(enabled.reason);
                }

                const scene::ObjectId body =
                    selection.Ordered().front();
                const bool ownsTransaction =
                    !commandService.HasActiveTransaction();

                if (ownsTransaction)
                {
                    commandService.BeginTransaction(
                        "Create Terrain Surface");
                }

                try
                {
                    const auto terrain =
                        commandService.CreateObject(
                            world_model::kTerrainSurfaceType,
                            "Terrain Surface",
                            body);

                    commandService.SetProperty(
                        terrain,
                        world_model::kTerrainSeed,
                        i64{0x41535445525241LL});
                    commandService.SetProperty(
                        terrain,
                        world_model::kTerrainMacroAmplitudeMeters,
                        1'200.0);
                    commandService.SetProperty(
                        terrain,
                        world_model::kTerrainMacroWavelengthMeters,
                        800'000.0);
                    commandService.SetProperty(
                        terrain,
                        world_model::kTerrainDetailAmplitudeMeters,
                        320.0);
                    commandService.SetProperty(
                        terrain,
                        world_model::kTerrainDetailWavelengthMeters,
                        40'000.0);
                    commandService.SetProperty(
                        terrain,
                        world_model::kTerrainDetailOctaves,
                        i64{10});
                    commandService.SetProperty(
                        terrain,
                        world_model::kTerrainMaximumElevationMeters,
                        8'000.0);

                    SurfaceAuthoringModel surfaceModel(
                        objects,
                        commandService,
                        selection);

                    static_cast<void>(
                        surfaceModel.EnsureProcessSettings(
                            terrain));

                    if (ownsTransaction)
                    {
                        commandService.CommitTransaction();
                    }

                    const scene::ObjectId selected[] = {terrain};
                    selection.Set(selected);
                }
                catch (...)
                {
                    if (ownsTransaction &&
                        commandService.HasActiveTransaction())
                    {
                        commandService.RollbackTransaction();
                    }
                    throw;
                }
            }
    });

    registry.Register({
        .id = kRemoveTerrainSurface,
        .name = "Remove Terrain Surface",
        .category = "World / Surface",
        .description =
            "Remove the authored terrain capability from the selected Celestial Body.",
        .presentationSurfaces = {
            "explorer.context",
            "properties.toolbar"
        },
        .enablement =
            [&objects, &selection]
            {
                return RemoveTerrainEnablement(objects, selection);
            },
        .invoke =
            [&objects,
             &commandService,
             &selection](
                const commands::CommandArguments&)
            {
                const auto enabled =
                    RemoveTerrainEnablement(objects, selection);

                if (!enabled.enabled)
                {
                    throw std::invalid_argument(enabled.reason);
                }

                const scene::ObjectId body = selection.Ordered().front();
                const auto terrain = TerrainSurfaceObject(objects, body);

                if (!terrain.has_value())
                {
                    throw std::logic_error(
                        "Terrain capability disappeared after command enablement.");
                }

                const bool ownsTransaction =
                    !commandService.HasActiveTransaction();

                if (ownsTransaction)
                {
                    commandService.BeginTransaction(
                        "Remove Terrain Surface");
                }

                try
                {
                    for (const auto& child :
                         objects.Children(*terrain))
                    {
                        if (child.type ==
                            world_model::kTerrainProcessAssetType)
                        {
                            commandService.DeleteObject(
                                child.id);
                        }
                    }

                    commandService.DeleteObject(*terrain);

                    if (ownsTransaction)
                    {
                        commandService.CommitTransaction();
                    }

                    const scene::ObjectId selected[] = {body};
                    selection.Set(selected);
                }
                catch (...)
                {
                    if (ownsTransaction &&
                        commandService.HasActiveTransaction())
                    {
                        commandService.RollbackTransaction();
                    }
                    throw;
                }
            }
    });
}
} // namespace orbit::editor_model::authoring_commands
