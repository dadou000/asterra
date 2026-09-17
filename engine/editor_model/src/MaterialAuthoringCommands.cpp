#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/editor_model/BuiltinSchemas.hpp>

#include <stdexcept>
#include <string>

namespace orbit::editor_model::authoring_commands
{
namespace
{
[[nodiscard]] commands::CommandEnablement
BodySelectionEnablement(
    const scene::ObjectStore& objects,
    const selection::SelectionService& selection)
{
    if (selection.Ordered().size() != 1)
    {
        return {
            .enabled = false,
            .reason = "Select exactly one celestial body."
        };
    }

    const auto object =
        objects.Find(selection.Ordered().front());

    if (!object.has_value())
    {
        return {
            .enabled = false,
            .reason = "The selected object no longer exists."
        };
    }

    if (object->type != builtin::kCelestialBodyType)
    {
        return {
            .enabled = false,
            .reason = "Select a celestial body."
        };
    }

    return {};
}

[[nodiscard]] const std::string& RequireString(
    const commands::CommandArguments& arguments,
    const std::string& name)
{
    const auto found = arguments.find(name);

    if (found == arguments.end())
    {
        throw std::invalid_argument(
            name + " is required.");
    }

    const auto* value =
        std::get_if<std::string>(&found->second);

    if (value == nullptr || value->empty())
    {
        throw std::invalid_argument(
            name + " must be a non-empty string.");
    }

    return *value;
}

[[nodiscard]] f64 RequireFloat(
    const commands::CommandArguments& arguments,
    const std::string& name)
{
    const auto found = arguments.find(name);

    if (found == arguments.end())
    {
        throw std::invalid_argument(
            name + " is required.");
    }

    const auto* value =
        std::get_if<f64>(&found->second);

    if (value == nullptr)
    {
        throw std::invalid_argument(
            name + " must be a float.");
    }

    return *value;
}
} // namespace

void RegisterMaterialCommands(
    commands::CommandRegistry& registry,
    commands::CommandService& commandService,
    scene::ObjectStore& objects,
    selection::SelectionService& selection)
{
    registry.Register({
        .id = kAttachDecal,
        .name = "Attach Decal",
        .category = "Material",
        .description =
            "Attach a persistent surface decal to the selected celestial body.",
        .parameters = {
            {
                .name = "decal",
                .kind = commands::CommandValueKind::String,
                .required = true
            },
            {
                .name = "latitude",
                .kind = commands::CommandValueKind::Float,
                .required = true
            },
            {
                .name = "longitude",
                .kind = commands::CommandValueKind::Float,
                .required = true
            },
            {
                .name = "width",
                .kind = commands::CommandValueKind::Float,
                .required = true
            },
            {
                .name = "height",
                .kind = commands::CommandValueKind::Float,
                .required = true
            },
            {
                .name = "rotation",
                .kind = commands::CommandValueKind::Float,
                .required = true
            },
            {
                .name = "opacity",
                .kind = commands::CommandValueKind::Float,
                .required = true
            }
        },
        .presentationSurfaces = {
            "viewport.context",
            "properties.toolbar"
        },
        .enablement =
            [&objects, &selection]
            {
                return BodySelectionEnablement(
                    objects,
                    selection);
            },
        .invoke =
            [&commandService,
             &objects,
             &selection](
                const commands::CommandArguments& arguments)
            {
                const std::string& decalAsset =
                    RequireString(arguments, "decal");
                const f64 latitude =
                    RequireFloat(arguments, "latitude");
                const f64 longitude =
                    RequireFloat(arguments, "longitude");
                const f64 width =
                    RequireFloat(arguments, "width");
                const f64 height =
                    RequireFloat(arguments, "height");
                const f64 rotation =
                    RequireFloat(arguments, "rotation");
                const f64 opacity =
                    RequireFloat(arguments, "opacity");

                if (!(width > 0.0) ||
                    !(height > 0.0))
                {
                    throw std::invalid_argument(
                        "Attach Decal width and height must be positive.");
                }

                if (opacity < 0.0 || opacity > 1.0)
                {
                    throw std::invalid_argument(
                        "Attach Decal opacity must be in [0, 1].");
                }

                const scene::ObjectId body =
                    selection.Ordered().front();

                const bool ownsTransaction =
                    !commandService.HasActiveTransaction();

                if (ownsTransaction)
                {
                    commandService.BeginTransaction(
                        "Attach Decal");
                }

                try
                {
                    u32 decalCount = 0;

                    for (const auto& child :
                         objects.Children(body))
                    {
                        if (child.type ==
                            builtin::kSurfaceDecalType)
                        {
                            ++decalCount;
                        }
                    }

                    const scene::ObjectId decal =
                        commandService.CreateObject(
                            builtin::kSurfaceDecalType,
                            "Surface Decal " +
                                std::to_string(
                                    decalCount + 1U),
                            body);

                    commandService.SetProperty(
                        decal,
                        builtin::kDecalAsset,
                        decalAsset);
                    commandService.SetProperty(
                        decal,
                        builtin::kDecalLatitudeRadians,
                        latitude);
                    commandService.SetProperty(
                        decal,
                        builtin::kDecalLongitudeRadians,
                        longitude);
                    commandService.SetProperty(
                        decal,
                        builtin::kDecalWidthMeters,
                        width);
                    commandService.SetProperty(
                        decal,
                        builtin::kDecalHeightMeters,
                        height);
                    commandService.SetProperty(
                        decal,
                        builtin::kDecalRotationDegrees,
                        rotation);
                    commandService.SetProperty(
                        decal,
                        builtin::kDecalOpacity,
                        opacity);

                    if (ownsTransaction)
                    {
                        commandService.CommitTransaction();
                    }

                    const scene::ObjectId selected[] = {
                        decal
                    };
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
