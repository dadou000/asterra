#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/editor_model/BuiltinSchemas.hpp>

#include <orbit/paths/PathNetwork.hpp>

#include <stdexcept>
#include <string>

namespace orbit::editor_model::authoring_commands
{
namespace
{
[[nodiscard]] commands::CommandEnablement
PathPairEnablement(
    const scene::ObjectStore& objects,
    const selection::SelectionService& selection)
{
    if (selection.Ordered().size() != 2)
    {
        return {
            .enabled = false,
            .reason = "Select exactly two path nodes."
        };
    }

    const auto first =
        objects.Find(selection.Ordered()[0]);
    const auto second =
        objects.Find(selection.Ordered()[1]);

    if (!first.has_value() ||
        !second.has_value())
    {
        return {
            .enabled = false,
            .reason = "A selected path node no longer exists."
        };
    }

    if (first->type != paths::kPathNodeType ||
        second->type != paths::kPathNodeType)
    {
        return {
            .enabled = false,
            .reason = "Both selected objects must be path nodes."
        };
    }

    if (!first->parent.has_value() ||
        first->parent != second->parent)
    {
        return {
            .enabled = false,
            .reason = "Selected path nodes must belong to the same network."
        };
    }

    return {};
}

[[nodiscard]] math::Double3 OptionalVector(
    const commands::CommandArguments& arguments,
    const std::string_view name)
{
    const auto found =
        arguments.find(std::string(name));

    if (found == arguments.end())
    {
        return {};
    }

    const auto* value =
        std::get_if<math::Double3>(
            &found->second);

    if (value == nullptr)
    {
        throw std::invalid_argument(
            std::string(name) +
            " must be a Vector3 command argument.");
    }

    return *value;
}
} // namespace

void Register(
    commands::CommandRegistry& registry,
    commands::CommandService& commandService,
    scene::ObjectStore& objects,
    selection::SelectionService& selection)
{
    registry.Register({
        .id = kUndo,
        .name = "Undo",
        .category = "Edit",
        .description =
            "Undo the most recent committed authoring transaction.",
        .enablement =
            [&commandService]
            {
                if (!commandService.CanUndo())
                {
                    return commands::CommandEnablement{
                        .enabled = false,
                        .reason =
                            "There is no committed edit to undo."
                    };
                }

                return commands::CommandEnablement{};
            },
        .invoke =
            [&commandService](
                const commands::CommandArguments&)
            {
                commandService.Undo();
            }
    });

    registry.Register({
        .id = kRedo,
        .name = "Redo",
        .category = "Edit",
        .description =
            "Redo the most recently undone authoring transaction.",
        .enablement =
            [&commandService]
            {
                if (!commandService.CanRedo())
                {
                    return commands::CommandEnablement{
                        .enabled = false,
                        .reason =
                            "There is no undone edit to redo."
                    };
                }

                return commands::CommandEnablement{};
            },
        .invoke =
            [&commandService](
                const commands::CommandArguments&)
            {
                commandService.Redo();
            }
    });

    registry.Register({
        .id = kClearSelection,
        .name = "Clear Selection",
        .category = "Selection",
        .description =
            "Clear the shared editor selection.",
        .enablement =
            [&selection]
            {
                if (selection.Ordered().empty())
                {
                    return commands::CommandEnablement{
                        .enabled = false,
                        .reason =
                            "Nothing is selected."
                    };
                }

                return commands::CommandEnablement{};
            },
        .invoke =
            [&selection](
                const commands::CommandArguments&)
            {
                selection.Clear();
            }
    });

    registry.Register({
        .id = kAssignMaterial,
        .name = "Assign Material",
        .category = "Material",
        .description =
            "Assign a project material asset to the selected compatible object.",
        .parameters = {
            commands::CommandParameter{
                .name = "material",
                .kind = commands::CommandValueKind::String,
                .required = true
            }
        },
        .enablement =
            [&selection, &objects]
            {
                if (selection.Ordered().size() != 1)
                {
                    return commands::CommandEnablement{
                        .enabled = false,
                        .reason =
                            "Select exactly one material-compatible object."
                    };
                }

                const auto object =
                    objects.Find(
                        selection.Ordered().front());

                if (!object.has_value())
                {
                    return commands::CommandEnablement{
                        .enabled = false,
                        .reason =
                            "The selected object no longer exists."
                    };
                }

                if (object->type !=
                    builtin::kCelestialBodyType)
                {
                    return commands::CommandEnablement{
                        .enabled = false,
                        .reason =
                            "The selected object does not expose a material assignment property."
                    };
                }

                return commands::CommandEnablement{};
            },
        .invoke =
            [&selection, &commandService](
                const commands::CommandArguments& arguments)
            {
                const auto found =
                    arguments.find("material");

                if (found == arguments.end())
                {
                    throw std::invalid_argument(
                        "Assign Material requires a material argument.");
                }

                const auto* material =
                    std::get_if<std::string>(
                        &found->second);

                if (material == nullptr)
                {
                    throw std::invalid_argument(
                        "Assign Material material argument must be a string.");
                }

                commandService.SetProperty(
                    selection.Ordered().front(),
                    builtin::kBodyMaterialAsset,
                    *material);
            }
    });

    registry.Register({
        .id = kMoveToRoot,
        .name = "Move To Root",
        .category = "Hierarchy",
        .description =
            "Reparent the primary selected object to the world root.",
        .enablement =
            [&selection, &objects]
            {
                if (selection.Ordered().size() != 1)
                {
                    return commands::CommandEnablement{
                        .enabled = false,
                        .reason =
                            "Select exactly one object."
                    };
                }

                const auto object =
                    objects.Find(
                        selection.Ordered().front());

                if (!object.has_value())
                {
                    return commands::CommandEnablement{
                        .enabled = false,
                        .reason =
                            "The selected object no longer exists."
                    };
                }

                if (!object->parent.has_value())
                {
                    return commands::CommandEnablement{
                        .enabled = false,
                        .reason =
                            "The selected object is already at the root."
                    };
                }

                return commands::CommandEnablement{};
            },
        .invoke =
            [&selection, &commandService](
                const commands::CommandArguments&)
            {
                commandService.ReparentObject(
                    selection.Ordered().front(),
                    std::nullopt);
            }
    });

    registry.Register({
        .id = kConnectPathDirect,
        .name = "Connect Direct",
        .category = "Path",
        .description =
            "Connect the two selected path nodes with a direct semantic edge.",
        .enablement =
            [&objects, &selection]
            {
                return PathPairEnablement(
                    objects,
                    selection);
            },
        .invoke =
            [&objects,
             &commandService,
             &selection](
                const commands::CommandArguments&)
            {
                paths::PathNetworkService service(
                    objects,
                    commandService);

                static_cast<void>(
                    service.ConnectDirect(
                        selection.Ordered()[0],
                        selection.Ordered()[1]));
            }
    });

    registry.Register({
        .id = kConnectPathBezier,
        .name = "Connect Bezier",
        .category = "Path",
        .description =
            "Connect the two selected path nodes with an editable cubic Bezier edge.",
        .parameters = {
            {
                .name = "start_handle",
                .kind = commands::CommandValueKind::Vector3,
                .required = false
            },
            {
                .name = "end_handle",
                .kind = commands::CommandValueKind::Vector3,
                .required = false
            }
        },
        .enablement =
            [&objects, &selection]
            {
                return PathPairEnablement(
                    objects,
                    selection);
            },
        .invoke =
            [&objects,
             &commandService,
             &selection](
                const commands::CommandArguments& arguments)
            {
                paths::PathNetworkService service(
                    objects,
                    commandService);

                static_cast<void>(
                    service.ConnectBezier(
                        selection.Ordered()[0],
                        selection.Ordered()[1],
                        OptionalVector(
                            arguments,
                            "start_handle"),
                        OptionalVector(
                            arguments,
                            "end_handle")));
            }
    });
    registry.Register({
        .id = kConnectPathRouted,
        .name = "Connect Routed",
        .category = "Path",
        .description =
            "Connect the two selected path nodes with an asynchronously solved routed edge.",
        .enablement =
            [&objects, &selection]
            {
                return PathPairEnablement(
                    objects,
                    selection);
            },
        .invoke =
            [&objects,
             &commandService,
             &selection](
                const commands::CommandArguments&)
            {
                paths::PathNetworkService service(
                    objects,
                    commandService);

                static_cast<void>(
                    service.ConnectRouted(
                        selection.Ordered()[0],
                        selection.Ordered()[1]));
            }
    });

}
} // namespace orbit::editor_model::authoring_commands
