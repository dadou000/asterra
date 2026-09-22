#include <orbit/editor_model/AuthoringCommands.hpp>

#include <orbit/world_model/VolumeSchemas.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

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

[[nodiscard]] std::optional<scene::ObjectId>
SelectedVolume(
    const scene::ObjectStore& objects,
    const selection::SelectionService& selection)
{
    if (selection.Ordered().size() != 1U)
    {
        return std::nullopt;
    }

    const auto selected =
        objects.Find(
            selection.Ordered().front());

    if (!selected.has_value())
    {
        return std::nullopt;
    }

    if (selected->type ==
        world_model::kVolumeType)
    {
        return selected->id;
    }

    if ((selected->type ==
             world_model::kVolumeSourceType ||
         selected->type ==
             world_model::kVolumeEffectorType) &&
        selected->parent.has_value())
    {
        const auto parent =
            objects.Find(
                *selected->parent);

        if (parent.has_value() &&
            parent->type ==
                world_model::kVolumeType)
        {
            return parent->id;
        }
    }

    return std::nullopt;
}

[[nodiscard]] std::optional<
    world_model::ResolvedVolumeInput>
SelectedVolumeInput(
    const scene::ObjectStore& objects,
    const selection::SelectionService& selection)
{
    if (selection.Ordered().size() != 1U)
    {
        return std::nullopt;
    }

    const auto record =
        objects.Find(
            selection.Ordered().front());

    if (!record.has_value() ||
        !record->parent.has_value() ||
        (record->type !=
             world_model::kVolumeSourceType &&
         record->type !=
             world_model::kVolumeEffectorType))
    {
        return std::nullopt;
    }

    for (const auto& input :
         world_model::ResolveVolumeInputs(
             objects,
             *record->parent))
    {
        if (input.object ==
            record->id)
        {
            return input;
        }
    }

    return std::nullopt;
}

struct Preset
{
    std::string name{"Empty"};
    math::Double3 halfExtents{
        10.0, 10.0, 10.0};
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

[[nodiscard]] world_model::VolumeSourceKind
SourceKindFromName(
    const std::string& name)
{
    using Kind =
        world_model::VolumeSourceKind;

    if (name == "Brush") return Kind::Brush;
    if (name == "Texture / Mask" ||
        name == "TextureMask") return Kind::TextureMask;
    if (name == "Terrain" ||
        name == "Terrain Paint") return Kind::Terrain;
    if (name == "Spline") return Kind::Spline;
    if (name == "Mesh / SDF" ||
        name == "MeshSdf") return Kind::MeshSdf;
    if (name == "Collision Proxy" ||
        name == "CollisionProxy") return Kind::CollisionProxy;
    if (name == "Particles") return Kind::Particles;
    if (name == "Object Motion" ||
        name == "ObjectMotion") return Kind::ObjectMotion;
    if (name == "World Motion" ||
        name == "WorldMotion") return Kind::WorldMotion;

    throw std::invalid_argument(
        "Unknown Volume source kind.");
}

[[nodiscard]] world_model::VolumeEffectorKind
EffectorKindFromName(
    const std::string& name)
{
    using Kind =
        world_model::VolumeEffectorKind;

    if (name == "Obstacle") return Kind::Obstacle;
    if (name == "Drag") return Kind::Drag;
    if (name == "Wind") return Kind::Wind;
    if (name == "Temperature") return Kind::Temperature;
    if (name == "Dissipation") return Kind::Dissipation;

    throw std::invalid_argument(
        "Unknown Volume effector kind.");
}

[[nodiscard]] world_model::VolumeSourceShape
DefaultShape(
    const world_model::VolumeSourceKind kind) noexcept
{
    using Kind =
        world_model::VolumeSourceKind;
    using Shape =
        world_model::VolumeSourceShape;

    switch (kind)
    {
    case Kind::Brush:
    case Kind::Particles:
    case Kind::ObjectMotion:
        return Shape::Sphere;
    case Kind::TextureMask:
    case Kind::WorldMotion:
        return Shape::Box;
    case Kind::Terrain:
        return Shape::TerrainPatch;
    case Kind::Spline:
        return Shape::Spline;
    case Kind::MeshSdf:
    case Kind::CollisionProxy:
        return Shape::Mesh;
    }

    return Shape::Sphere;
}

[[nodiscard]] i64
NextInputOrder(
    const scene::ObjectStore& objects,
    const scene::ObjectId volume,
    const world_model::VolumeInputRole role)
{
    i64 next = 0;

    for (const auto& input :
         world_model::ResolveVolumeInputs(
             objects,
             volume))
    {
        if (input.role == role)
        {
            next =
                std::max(
                    next,
                    input.order + 1);
        }
    }

    return next;
}

[[nodiscard]] scene::ObjectId
CreateSource(
    commands::CommandService& commandService,
    const scene::ObjectStore& objects,
    const scene::ObjectId volume,
    const world_model::VolumeSourceKind kind,
    const math::Double3 position = {},
    const f64 radius = 1.0,
    const f64 scalar = 1.0,
    const bool paintEnabled = false)
{
    const auto domain =
        world_model::ResolveVolumeDomain(
            objects,
            volume);

    if (!domain.has_value())
    {
        throw std::invalid_argument(
            "Volume source requires a valid parent Volume.");
    }

    const i64 order =
        NextInputOrder(
            objects,
            volume,
            world_model::VolumeInputRole::Source);

    const std::string name =
        std::string(
            world_model::VolumeSourceKindName(
                kind)) +
        " Source";

    const auto source =
        commandService.CreateObject(
            world_model::kVolumeSourceType,
            name,
            volume,
            order);

    commandService.SetProperty(source, world_model::kVolumeChildEnabled, true);
    commandService.SetProperty(source, world_model::kVolumeChildKind, static_cast<i64>(kind));
    commandService.SetProperty(source, world_model::kVolumeChildOrder, order);
    commandService.SetProperty(source, world_model::kVolumeChildShape, static_cast<i64>(DefaultShape(kind)));
    commandService.SetProperty(source, world_model::kVolumeChildPositionMeters, position);
    commandService.SetProperty(source, world_model::kVolumeChildRadiusMeters, std::max(radius, 0.0));
    commandService.SetProperty(source, world_model::kVolumeChildHalfExtentsMeters, math::Double3{std::max(radius,1.0),std::max(radius,1.0),std::max(radius,1.0)});
    commandService.SetProperty(source, world_model::kVolumeChildScalar, scalar);
    commandService.SetProperty(source, world_model::kVolumeChildVector, math::Double3{});
    commandService.SetProperty(source, world_model::kVolumeChildFieldMask, static_cast<i64>(domain->fieldMask));
    commandService.SetProperty(source, world_model::kVolumeChildAsset, std::string{});
    commandService.SetProperty(source, world_model::kVolumeChildPaintEnabled, paintEnabled);

    return source;
}

[[nodiscard]] scene::ObjectId
CreateEffector(
    commands::CommandService& commandService,
    const scene::ObjectStore& objects,
    const scene::ObjectId volume,
    const world_model::VolumeEffectorKind kind)
{
    const auto domain =
        world_model::ResolveVolumeDomain(
            objects,
            volume);

    if (!domain.has_value())
    {
        throw std::invalid_argument(
            "Volume effector requires a valid parent Volume.");
    }

    const i64 order =
        NextInputOrder(
            objects,
            volume,
            world_model::VolumeInputRole::Effector);

    const auto effector =
        commandService.CreateObject(
            world_model::kVolumeEffectorType,
            std::string(
                world_model::VolumeEffectorKindName(
                    kind)) +
                " Effector",
            volume,
            order);

    commandService.SetProperty(effector, world_model::kVolumeChildEnabled, true);
    commandService.SetProperty(effector, world_model::kVolumeChildKind, static_cast<i64>(kind));
    commandService.SetProperty(effector, world_model::kVolumeChildOrder, order);
    commandService.SetProperty(effector, world_model::kVolumeChildShape, static_cast<i64>(world_model::VolumeSourceShape::Sphere));
    commandService.SetProperty(effector, world_model::kVolumeChildPositionMeters, math::Double3{});
    commandService.SetProperty(effector, world_model::kVolumeChildRadiusMeters, 1.0);
    commandService.SetProperty(effector, world_model::kVolumeChildHalfExtentsMeters, math::Double3{1.0,1.0,1.0});
    commandService.SetProperty(effector, world_model::kVolumeChildScalar, 1.0);
    commandService.SetProperty(effector, world_model::kVolumeChildVector, math::Double3{});
    commandService.SetProperty(effector, world_model::kVolumeChildFieldMask, static_cast<i64>(domain->fieldMask));
    commandService.SetProperty(effector, world_model::kVolumeChildAsset, std::string{});
    commandService.SetProperty(effector, world_model::kVolumeChildPaintEnabled, false);

    return effector;
}

void MoveSelectedInput(
    scene::ObjectStore& objects,
    commands::CommandService& commandService,
    selection::SelectionService& selection,
    const i32 direction)
{
    const auto selected =
        SelectedVolumeInput(
            objects,
            selection);

    if (!selected.has_value())
    {
        throw std::invalid_argument(
            "Select one Volume Source or Volume Effector.");
    }

    const auto record =
        objects.Find(
            selected->object);

    if (!record.has_value() ||
        !record->parent.has_value())
    {
        throw std::invalid_argument(
            "Selected Volume input has no parent.");
    }

    auto inputs =
        world_model::ResolveVolumeInputs(
            objects,
            *record->parent);

    std::erase_if(
        inputs,
        [selected](
            const world_model::ResolvedVolumeInput& input)
        {
            return input.role != selected->role;
        });

    const auto current =
        std::find_if(
            inputs.begin(),
            inputs.end(),
            [selected](
                const world_model::ResolvedVolumeInput& input)
            {
                return input.object ==
                    selected->object;
            });

    if (current == inputs.end())
    {
        throw std::logic_error(
            "Selected Volume input disappeared.");
    }

    const auto index =
        static_cast<i64>(
            std::distance(
                inputs.begin(),
                current));
    const i64 otherIndex =
        index +
        static_cast<i64>(direction);

    if (otherIndex < 0 ||
        otherIndex >=
            static_cast<i64>(
                inputs.size()))
    {
        return;
    }

    const auto& other =
        inputs[
            static_cast<std::size_t>(
                otherIndex)];

    const bool owns =
        !commandService.HasActiveTransaction();

    if (owns)
    {
        commandService.BeginTransaction(
            "Reorder Volume Input");
    }

    try
    {
        commandService.SetProperty(
            selected->object,
            world_model::kVolumeChildOrder,
            other.order);
        commandService.SetProperty(
            other.object,
            world_model::kVolumeChildOrder,
            selected->order);

        if (owns)
        {
            commandService.CommitTransaction();
        }
    }
    catch (...)
    {
        if (owns &&
            commandService.HasActiveTransaction())
        {
            commandService.RollbackTransaction();
        }
        throw;
    }
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
            {.name="preset",.kind=commands::CommandValueKind::String,.required=false}
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
                    commandService.SetProperty(volume, world_model::kVolumeSurfaceLayers, i64{4});

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
                const auto selected =
                    SelectedVolume(objects, selection);

                if (!selected.has_value() ||
                    selection.Ordered().front() !=
                        *selected)
                    return commands::CommandEnablement{.enabled=false,.reason="Select a Volume."};

                return commands::CommandEnablement{};
            },
        .invoke =
            [&objects, &commandService, &selection](
                const commands::CommandArguments&)
            {
                const auto selected =
                    SelectedVolume(objects, selection);

                if (!selected.has_value() ||
                    selection.Ordered().front() !=
                        *selected)
                    throw std::invalid_argument("Select a Volume.");

                const auto volume = *selected;
                const auto record =
                    objects.Find(volume);
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
                        const scene::ObjectId selectedParent[]{*parent};
                        selection.Set(selectedParent);
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

    registry.Register({
        .id = kAddVolumeSource,
        .name = "Add Volume Source",
        .category = "World / Volumetrics",
        .description = "Add one source adapter to the selected universal Volume.",
        .parameters = {
            {.name="kind",.kind=commands::CommandValueKind::String,.required=true}
        },
        .presentationSurfaces = {
            "properties.toolbar",
            "viewport.radial"
        },
        .enablement =
            [&objects, &selection]
            {
                return SelectedVolume(objects, selection).has_value()
                    ? commands::CommandEnablement{}
                    : commands::CommandEnablement{.enabled=false,.reason="Select a Volume or one of its inputs."};
            },
        .invoke =
            [&objects, &commandService, &selection](
                const commands::CommandArguments& arguments)
            {
                const auto volume =
                    SelectedVolume(objects, selection);

                if (!volume.has_value())
                    throw std::invalid_argument("Select a Volume.");

                const auto found =
                    arguments.find("kind");
                const auto* kindText =
                    found != arguments.end()
                        ? std::get_if<std::string>(&found->second)
                        : nullptr;

                if (kindText == nullptr)
                    throw std::invalid_argument("Add Volume Source requires a string kind.");

                const bool owns =
                    !commandService.HasActiveTransaction();

                if (owns)
                    commandService.BeginTransaction("Add Volume Source");

                try
                {
                    const auto source =
                        CreateSource(
                            commandService,
                            objects,
                            *volume,
                            SourceKindFromName(*kindText));

                    if (owns)
                        commandService.CommitTransaction();

                    const scene::ObjectId selectedSource[]{source};
                    selection.Set(selectedSource);
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
        .id = kAddVolumeEffector,
        .name = "Add Volume Effector",
        .category = "World / Volumetrics",
        .description = "Add one obstacle/force effector to the selected universal Volume.",
        .parameters = {
            {.name="kind",.kind=commands::CommandValueKind::String,.required=true}
        },
        .presentationSurfaces = {
            "properties.toolbar",
            "viewport.radial"
        },
        .enablement =
            [&objects, &selection]
            {
                return SelectedVolume(objects, selection).has_value()
                    ? commands::CommandEnablement{}
                    : commands::CommandEnablement{.enabled=false,.reason="Select a Volume or one of its inputs."};
            },
        .invoke =
            [&objects, &commandService, &selection](
                const commands::CommandArguments& arguments)
            {
                const auto volume =
                    SelectedVolume(objects, selection);

                if (!volume.has_value())
                    throw std::invalid_argument("Select a Volume.");

                const auto found =
                    arguments.find("kind");
                const auto* kindText =
                    found != arguments.end()
                        ? std::get_if<std::string>(&found->second)
                        : nullptr;

                if (kindText == nullptr)
                    throw std::invalid_argument("Add Volume Effector requires a string kind.");

                const bool owns =
                    !commandService.HasActiveTransaction();

                if (owns)
                    commandService.BeginTransaction("Add Volume Effector");

                try
                {
                    const auto effector =
                        CreateEffector(
                            commandService,
                            objects,
                            *volume,
                            EffectorKindFromName(*kindText));

                    if (owns)
                        commandService.CommitTransaction();

                    const scene::ObjectId selectedEffector[]{effector};
                    selection.Set(selectedEffector);
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
        .id = kRemoveVolumeInput,
        .name = "Remove Volume Input",
        .category = "World / Volumetrics",
        .description = "Remove the selected Volume Source or Effector.",
        .presentationSurfaces = {
            "explorer.context",
            "properties.toolbar"
        },
        .enablement =
            [&objects, &selection]
            {
                return SelectedVolumeInput(objects, selection).has_value()
                    ? commands::CommandEnablement{}
                    : commands::CommandEnablement{.enabled=false,.reason="Select one Volume Source or Effector."};
            },
        .invoke =
            [&objects, &commandService, &selection](
                const commands::CommandArguments&)
            {
                const auto input =
                    SelectedVolumeInput(objects, selection);

                if (!input.has_value())
                    throw std::invalid_argument("Select one Volume Source or Effector.");

                const auto record =
                    objects.Find(input->object);
                const auto parent =
                    record->parent;

                commandService.DeleteObject(input->object);

                if (parent.has_value())
                {
                    const scene::ObjectId parentSelection[]{*parent};
                    selection.Set(parentSelection);
                }
                else
                {
                    selection.Clear();
                }
            }
    });

    const auto reorderEnablement =
        [&objects, &selection]
        {
            return SelectedVolumeInput(objects, selection).has_value()
                ? commands::CommandEnablement{}
                : commands::CommandEnablement{.enabled=false,.reason="Select one Volume Source or Effector."};
        };

    registry.Register({
        .id = kMoveVolumeInputUp,
        .name = "Move Volume Input Up",
        .category = "World / Volumetrics",
        .description = "Move the selected source/effector earlier in deterministic evaluation order.",
        .presentationSurfaces = {"properties.toolbar"},
        .enablement = reorderEnablement,
        .invoke =
            [&objects, &commandService, &selection](
                const commands::CommandArguments&)
            {
                MoveSelectedInput(
                    objects,
                    commandService,
                    selection,
                    -1);
            }
    });

    registry.Register({
        .id = kMoveVolumeInputDown,
        .name = "Move Volume Input Down",
        .category = "World / Volumetrics",
        .description = "Move the selected source/effector later in deterministic evaluation order.",
        .presentationSurfaces = {"properties.toolbar"},
        .enablement = reorderEnablement,
        .invoke =
            [&objects, &commandService, &selection](
                const commands::CommandArguments&)
            {
                MoveSelectedInput(
                    objects,
                    commandService,
                    selection,
                    1);
            }
    });

    registry.Register({
        .id = kPaintVolumeTerrainSource,
        .name = "Paint Volume Terrain Source",
        .category = "World / Volumetrics",
        .description = "Author one persistent terrain-paint source stroke at a world-space position.",
        .parameters = {
            {.name="position",.kind=commands::CommandValueKind::Vector3,.required=true},
            {.name="radius",.kind=commands::CommandValueKind::Float,.required=false},
            {.name="strength",.kind=commands::CommandValueKind::Float,.required=false}
        },
        .presentationSurfaces = {
            "viewport.radial",
            "properties.toolbar"
        },
        .enablement =
            [&objects, &selection]
            {
                return SelectedVolume(objects, selection).has_value()
                    ? commands::CommandEnablement{}
                    : commands::CommandEnablement{.enabled=false,.reason="Select a Volume or one of its inputs."};
            },
        .invoke =
            [&objects, &commandService, &selection](
                const commands::CommandArguments& arguments)
            {
                const auto volume =
                    SelectedVolume(objects, selection);

                if (!volume.has_value())
                    throw std::invalid_argument("Select a Volume.");

                const auto positionFound =
                    arguments.find("position");
                const auto* position =
                    positionFound != arguments.end()
                        ? std::get_if<math::Double3>(&positionFound->second)
                        : nullptr;

                if (position == nullptr)
                    throw std::invalid_argument("Terrain source painting requires a world position.");

                f64 radius = 2.0;
                f64 strength = 1.0;

                if (const auto found = arguments.find("radius");
                    found != arguments.end())
                {
                    if (const auto* value =
                            std::get_if<f64>(&found->second);
                        value != nullptr)
                        radius = *value;
                }

                if (const auto found = arguments.find("strength");
                    found != arguments.end())
                {
                    if (const auto* value =
                            std::get_if<f64>(&found->second);
                        value != nullptr)
                        strength = *value;
                }

                const bool owns =
                    !commandService.HasActiveTransaction();

                if (owns)
                    commandService.BeginTransaction("Paint Volume Terrain Source");

                try
                {
                    const auto stroke =
                        CreateSource(
                            commandService,
                            objects,
                            *volume,
                            world_model::VolumeSourceKind::Terrain,
                            *position,
                            std::max(radius, 0.0),
                            strength,
                            true);

                    if (owns)
                        commandService.CommitTransaction();

                    const scene::ObjectId selectedStroke[]{stroke};
                    selection.Set(selectedStroke);
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
