#include <orbit/editor_model/AuthoringCommands.hpp>

namespace orbit::editor_model::authoring_commands
{
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
}
} // namespace orbit::editor_model::authoring_commands
