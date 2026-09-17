#include <orbit/editor_model/AuthoringCommands.hpp>

#include <orbit/world_model/WorldSchemas.hpp>

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

[[nodiscard]] bool HasTerrainSurface(
    const scene::ObjectStore& objects,
    const scene::ObjectId body)
{
    for (const auto& child : objects.Children(body))
    {
        if (child.type == world_model::kTerrainSurfaceType)
        {
            return true;
        }
    }

    return false;
}

[[nodiscard]] commands::CommandEnablement TerrainEnablement(
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

    if (HasTerrainSurface(objects, object->id))
    {
        return {
            .enabled = false,
            .reason =
                "This Celestial Body already owns a Terrain Surface capability."
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
                return TerrainEnablement(objects, selection);
            },
        .invoke =
            [&objects,
             &commandService,
             &selection](
                const commands::CommandArguments&)
            {
                const auto enabled =
                    TerrainEnablement(objects, selection);

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
}
} // namespace orbit::editor_model::authoring_commands
