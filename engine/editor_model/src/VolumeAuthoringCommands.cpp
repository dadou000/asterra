#include <orbit/editor_model/AuthoringCommands.hpp>

#include <orbit/world_model/VolumeSchemas.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <variant>

namespace orbit::editor_model::authoring_commands
{
namespace
{
[[nodiscard]] commands::CommandEnablement
VolumeParentEnablement(
    const scene::ObjectStore& objects,
    const selection::SelectionService& selection)
{
    if (selection.Ordered().size() != 1U)
    {
        return {
            .enabled = false,
            .reason = "Select one World, Celestial Body, or existing Volume."
        };
    }

    const auto record =
        objects.Find(
            selection.Ordered().front());

    if (!record.has_value())
    {
        return {
            .enabled = false,
            .reason = "The selected object no longer exists."
        };
    }

    if (record->type != world_model::kWorldType &&
        record->type != world_model::kCelestialBodyType &&
        record->type != world_model::kVolumeType)
    {
        return {
            .enabled = false,
            .reason = "Volumes can be created under a World or Celestial Body."
        };
    }

    return {};
}

struct Preset
{
    std::string name{"Empty"};
    math::Double3 halfExtents{10.0, 10.0, 10.0};
    world_model::VolumeSolverPolicy solver{
        world_model::VolumeSolverPolicy::Auto};
    world_model::VolumeRepresentationMode representation{
        world_model::VolumeRepresentationMode::Auto};
    u64 fields{0U};
};

[[nodiscard]] Preset
PresetFor(
    std::string name)
{
    if (name.empty())
    {
        name = "Empty";
    }

    const auto density =
        static_cast<u64>(
            world_model::VolumeField::Density);
    const auto velocity =
        static_cast<u64>(
            world_model::VolumeField::Velocity);
    const auto temperature =
        static_cast<u64>(
            world_model::VolumeField::Temperature);
    const auto fuel =
        static_cast<u64>(
            world_model::VolumeField::Fuel);
    const auto emission =
        static_cast<u64>(
            world_model::VolumeField::Emission);
    const auto moisture =
        static_cast<u64>(
            world_model::VolumeField::Moisture);
    const auto sediment =
        static_cast<u64>(
            world_model::VolumeField::Sediment);

    if (name == "Smoke")
        return {name,{12.0,12.0,12.0},world_model::VolumeSolverPolicy::Local3D,world_model::VolumeRepresentationMode::Auto,density|velocity|temperature};
    if (name == "Fire")
        return {name,{8.0,12.0,8.0},world_model::VolumeSolverPolicy::Local3D,world_model::VolumeRepresentationMode::Auto,density|velocity|temperature|fuel|emission};
    if (name == "Fog")
        return {name,{100.0,25.0,100.0},world_model::VolumeSolverPolicy::Surface2D5D,world_model::VolumeRepresentationMode::Auto,density|velocity|moisture};
    if (name == "Dust")
        return {name,{80.0,20.0,80.0},world_model::VolumeSolverPolicy::Surface2D5D,world_model::VolumeRepresentationMode::Auto,density|velocity|sediment};
    if (name == "Snow")
        return {name,{100.0,40.0,100.0},world_model::VolumeSolverPolicy::Surface2D5D,world_model::VolumeRepresentationMode::Auto,density|velocity|temperature|moisture};
    if (name == "Surface Flow")
        return {name,{128.0,4.0,128.0},world_model::VolumeSolverPolicy::Surface2D5D,world_model::VolumeRepresentationMode::Auto,velocity|moisture|sediment};
    if (name == "Empty")
        return {};

    throw std::invalid_argument(
        "Unknown Volume preset. Use Empty, Smoke, Fire, Fog, Dust, Snow, or Surface Flow.");
}
} // namespace

void RegisterVolumeCommands(
    commands::CommandRegistry& registry,
    commands::CommandService& commandService,
    scene::ObjectStore& objects,
    selection::SelectionService& selection)
{
    registry.Register({
        .id = kCreateVolume,
        .name = "Create Volume",
        .category = "World / Volumetrics",
        .description = "Create one authored universal Volume domain using Empty or a production preset.",
        .parameters = {
            commands::CommandParameter{
                .name = "preset",
                .kind = commands::CommandValueKind::String,
                .required = false
            }
        },
        .presentationSurfaces = {
            "explorer.context",
            "properties.toolbar"
        },
        .enablement =
            [&objects, &selection]
            {
                return VolumeParentEnablement(
                    objects,
                    selection);
            },
        .invoke =
            [&objects, &commandService, &selection](
                const commands::CommandArguments& arguments)
            {
                const auto enabled =
                    VolumeParentEnablement(
                        objects,
                        selection);

                if (!enabled.enabled)
                    throw std::invalid_argument(enabled.reason);

                std::string presetName{"Empty"};

                if (const auto found =
                        arguments.find("preset");
                    found != arguments.end())
                {
                    const auto* preset =
                        std::get_if<std::string>(
                            &found->second);

                    if (preset == nullptr)
                        throw std::invalid_argument("Volume preset must be a string.");

                    presetName = *preset;
                }

                const auto preset =
                    PresetFor(
                        presetName);

                scene::ObjectId parent =
                    selection.Ordered().front();

                if (const auto selected =
                        objects.Find(parent);
                    selected.has_value() &&
                    selected->type ==
                        world_model::kVolumeType)
                {
                    if (!selected->parent.has_value())
                        throw std::invalid_argument("Selected Volume has no parent.");

                    parent = *selected->parent;
                }

                u32 count = 0U;
                for (const auto& child :
                     objects.Children(parent))
                {
                    if (child.type ==
                        world_model::kVolumeType)
                        ++count;
                }

                const bool owns =
                    !commandService.HasActiveTransaction();

                if (owns)
                    commandService.BeginTransaction("Create Volume");

                try
                {
                    const auto volume =
                        commandService.CreateObject(
                            world_model::kVolumeType,
                            preset.name == "Empty"
                                ? "Volume " + std::to_string(count + 1U)
                                : preset.name + " Volume",
                            parent);

                    commandService.SetProperty(volume, world_model::kVolumeEnabled, true);
                    commandService.SetProperty(volume, world_model::kVolumePreset, preset.name);
                    commandService.SetProperty(volume, world_model::kVolumeCenterMeters, math::Double3{});
                    commandService.SetProperty(volume, world_model::kVolumeHalfExtentsMeters, preset.halfExtents);
                    commandService.SetProperty(volume, world_model::kVolumeSolverPolicy, static_cast<i64>(preset.solver));
                    commandService.SetProperty(volume, world_model::kVolumeRepresentationMode, static_cast<i64>(preset.representation));
                    commandService.SetProperty(volume, world_model::kVolumeFieldMask, static_cast<i64>(preset.fields));
                    commandService.SetProperty(volume, world_model::kVolumeResolution, i64{64});

                    if (owns)
                        commandService.CommitTransaction();

                    const scene::ObjectId selected[]{volume};
                    selection.Set(selected);
                }
                catch (...)
                {
                    if (owns &&
                        commandService.HasActiveTransaction())
                        commandService.RollbackTransaction();
                    throw;
                }
            }
    });

    registry.Register({
        .id = kRemoveVolume,
        .name = "Remove Volume",
        .category = "World / Volumetrics",
        .description = "Remove the selected authored Volume domain and its semantic source/effector children.",
        .presentationSurfaces = {
            "explorer.context",
            "properties.toolbar"
        },
        .enablement =
            [&objects, &selection]
            {
                if (selection.Ordered().size() != 1U)
                    return commands::CommandEnablement{.enabled=false,.reason="Select one Volume."};

                const auto record =
                    objects.Find(
                        selection.Ordered().front());

                if (!record.has_value() ||
                    record->type !=
                        world_model::kVolumeType)
                    return commands::CommandEnablement{.enabled=false,.reason="Select a Volume."};

                return commands::CommandEnablement{};
            },
        .invoke =
            [&objects, &commandService, &selection](
                const commands::CommandArguments&)
            {
                const auto volume =
                    selection.Ordered().front();
                const auto record =
                    objects.Find(volume);

                if (!record.has_value() ||
                    record->type !=
                        world_model::kVolumeType)
                    throw std::invalid_argument("Select a Volume.");

                const auto parent =
                    record->parent;

                const bool owns =
                    !commandService.HasActiveTransaction();

                if (owns)
                    commandService.BeginTransaction("Remove Volume");

                try
                {
                    for (const auto& child :
                         objects.Children(volume))
                    {
                        commandService.DeleteObject(child.id);
                    }

                    commandService.DeleteObject(volume);

                    if (owns)
                        commandService.CommitTransaction();

                    if (parent.has_value())
                    {
                        const scene::ObjectId selected[]{*parent};
                        selection.Set(selected);
                    }
                    else
                    {
                        selection.Clear();
                    }
                }
                catch (...)
                {
                    if (owns &&
                        commandService.HasActiveTransaction())
                        commandService.RollbackTransaction();
                    throw;
                }
            }
    });
}
} // namespace orbit::editor_model::authoring_commands
