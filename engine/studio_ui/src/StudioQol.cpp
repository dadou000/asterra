#include <orbit/studio_ui/StudioViewportPanels.hpp>

#include <orbit/content/ContentService.hpp>
#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/editor_model/ShortcutRegistry.hpp>
#include <orbit/studio_ui/InspectorProviderRegistry.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <span>
#include <string>
#include <utility>

namespace orbit::studio_ui
{
namespace
{
[[nodiscard]] std::string Lower(std::string value)
{
    std::ranges::transform(
        value,
        value.begin(),
        [](const unsigned char c)
        {
            return static_cast<char>(std::tolower(c));
        });
    return value;
}

[[nodiscard]] bool IsPinned(
    const std::vector<schema::PropertyId>& pinned,
    const schema::PropertyId property) noexcept
{
    return std::ranges::find(pinned, property) != pinned.end();
}
} // namespace

void StudioViewportPanels::Notify(
    std::string title,
    std::string detail,
    const bool error) noexcept
{
    status_ = error ? detail : std::string{};
    if (ui_ == nullptr)
    {
        return;
    }

    try
    {
        ui_->PushNotification({
            .title = std::move(title),
            .detail = std::move(detail),
            .severity = error
                ? editor_ui::EditorUi::NotificationSeverity::Error
                : editor_ui::EditorUi::NotificationSeverity::Info,
            .seconds = error ? 6.0F : 3.0F
        });
    }
    catch (...)
    {
        // Presentation feedback must never make a completed authoring command
        // fail after its semantic transaction has committed.
    }
}

void StudioViewportPanels::InstallQol(
    editor_ui::EditorUi& ui) noexcept
{
    ui_ = &ui;

    try
    {
        GlobalInspectorProviders().Upsert({
            .id = "orbit.selection.qol",
            .owner = "orbit",
            .title = "Property Tools",
            .order = 10,
            .defaultOpen = true,
            .relevant =
                [this]
                {
                    return session_ != nullptr &&
                        session_->World().HasWorld() &&
                        !session_->World().Selection().Ordered().empty();
                },
            .draw =
                [this](editor_ui::PanelContext& context)
                {
                    DrawQolProperties(context);
                }
        });
    }
    catch (...)
    {
        // Headless presentation tests may not own a live UI extension context.
    }

    if (qolInstalled_)
    {
        return;
    }

    auto* shortcuts =
        editor_model::ShortcutRegistry::Active();
    if (shortcuts == nullptr)
    {
        return;
    }

    try
    {
        shortcuts->RegisterCallback(
            {.key = platform::Key::P, .control = true},
            [this]
            {
                RequestCommandPaletteOpen();
            });
        shortcuts->RegisterCallback(
            {.key = platform::Key::F2},
            [this]
            {
                BeginRenameSelection();
            });
        shortcuts->RegisterCallback(
            {.key = platform::Key::Delete},
            [this]
            {
                DeleteSelection();
            });
        shortcuts->RegisterCallback(
            {.key = platform::Key::D, .control = true},
            [this]
            {
                DuplicateSelection();
            });
        shortcuts->RegisterCallback(
            {.key = platform::Key::G},
            [this]
            {
                RevealSelectionInWorld();
            });
        shortcuts->RegisterCallback(
            {.key = platform::Key::ArrowLeft, .alt = true},
            [this]
            {
                NavigateSelectionHistory(-1);
            });
        shortcuts->RegisterCallback(
            {.key = platform::Key::ArrowRight, .alt = true},
            [this]
            {
                NavigateSelectionHistory(1);
            });
        shortcuts->RegisterCallback(
            {.key = platform::Key::ArrowUp, .alt = true},
            [this]
            {
                SelectParent();
            });
        shortcuts->RegisterCallback(
            {.key = platform::Key::ArrowDown, .alt = true},
            [this]
            {
                SelectFirstChild();
            });
        shortcuts->RegisterCallback(
            {.key = platform::Key::Q},
            [this]
            {
                expansion_.ViewportState().gizmo.tool = GizmoTool::Select;
            });
        shortcuts->RegisterCallback(
            {.key = platform::Key::W},
            [this]
            {
                expansion_.ViewportState().gizmo.tool = GizmoTool::Translate;
            });
        shortcuts->RegisterCallback(
            {.key = platform::Key::E},
            [this]
            {
                expansion_.ViewportState().gizmo.tool = GizmoTool::Rotate;
            });
        shortcuts->RegisterCallback(
            {.key = platform::Key::F4},
            [this]
            {
                expansion_.ViewportState().gizmo.tool = GizmoTool::Scale;
            });

        qolInstalled_ = true;
    }
    catch (...)
    {
        // A host may already own one of these bindings. Keep the editor usable
        // and leave the remaining workflows accessible through visible UI.
    }
}

void StudioViewportPanels::EnsureQolCommands()
{
    if (session_ == nullptr ||
        !session_->World().HasWorld())
    {
        return;
    }

    auto& world = session_->World();
    auto& registry = world.CommandRegistry();

    if (registry.Find(
            editor_model::authoring_commands::kDuplicateSelection) == nullptr)
    {
        registry.Register({
            .id = editor_model::authoring_commands::kDuplicateSelection,
            .name = "Duplicate Selection",
            .category = "Edit",
            .description =
                "Duplicate selected leaf objects as one undoable transaction.",
            .presentationSurfaces = {
                "explorer.context",
                "viewport.context",
                "properties.toolbar"
            },
            .enablement =
                [this]() -> commands::CommandEnablement
                {
                    if (session_ == nullptr ||
                        !session_->World().HasWorld())
                    {
                        return {.enabled = false, .reason = "No world is open."};
                    }
                    auto& world = session_->World();
                    if (world.Selection().Ordered().empty())
                    {
                        return {.enabled = false, .reason = "Nothing is selected."};
                    }
                    for (const auto id : world.Selection().Ordered())
                    {
                        if (!world.Objects().Children(id).empty())
                        {
                            return {
                                .enabled = false,
                                .reason = "Generic duplicate is limited to leaf objects."
                            };
                        }
                    }
                    return {};
                },
            .invoke =
                [this](const commands::CommandArguments&)
                {
                    DuplicateSelection();
                }
        });
    }

    if (registry.Find(
            editor_model::authoring_commands::kDeleteSelection) == nullptr)
    {
        registry.Register({
            .id = editor_model::authoring_commands::kDeleteSelection,
            .name = "Delete Selection",
            .category = "Edit",
            .description =
                "Delete selected leaf objects. The operation is undoable.",
            .presentationSurfaces = {
                "explorer.context",
                "viewport.context",
                "properties.toolbar"
            },
            .enablement =
                [this]() -> commands::CommandEnablement
                {
                    if (session_ == nullptr ||
                        !session_->World().HasWorld())
                    {
                        return {.enabled = false, .reason = "No world is open."};
                    }
                    auto& world = session_->World();
                    if (world.Selection().Ordered().empty())
                    {
                        return {.enabled = false, .reason = "Nothing is selected."};
                    }
                    for (const auto id : world.Selection().Ordered())
                    {
                        if (!world.Objects().Children(id).empty())
                        {
                            return {
                                .enabled = false,
                                .reason = "Objects with children are protected from generic Delete."
                            };
                        }
                    }
                    return {};
                },
            .invoke =
                [this](const commands::CommandArguments&)
                {
                    DeleteSelection();
                }
        });
    }
}

void StudioViewportPanels::TrackSelectionHistory()
{
    if (session_ == nullptr || !session_->World().HasWorld())
    {
        return;
    }

    auto& selection = session_->World().Selection();
    if (selection.Revision() == observedSelectionRevision_)
    {
        return;
    }
    observedSelectionRevision_ = selection.Revision();

    if (applyingSelectionHistory_)
    {
        applyingSelectionHistory_ = false;
        return;
    }

    const auto current = selection.Ordered();
    if (!selectionHistory_.empty() &&
        selectionHistory_[selectionHistoryCursor_] == current)
    {
        return;
    }

    if (!selectionHistory_.empty() &&
        selectionHistoryCursor_ + 1U < selectionHistory_.size())
    {
        selectionHistory_.erase(
            selectionHistory_.begin() +
                static_cast<std::ptrdiff_t>(selectionHistoryCursor_ + 1U),
            selectionHistory_.end());
    }

    selectionHistory_.push_back(current);
    if (selectionHistory_.size() > 64U)
    {
        selectionHistory_.erase(selectionHistory_.begin());
    }
    selectionHistoryCursor_ = selectionHistory_.empty()
        ? 0U
        : selectionHistory_.size() - 1U;
}

void StudioViewportPanels::NavigateSelectionHistory(const i32 delta)
{
    TrackSelectionHistory();
    if (session_ == nullptr || selectionHistory_.empty())
    {
        return;
    }

    const i64 current = static_cast<i64>(selectionHistoryCursor_);
    const i64 next = std::clamp<i64>(
        current + delta,
        0,
        static_cast<i64>(selectionHistory_.size() - 1U));
    if (next == current)
    {
        return;
    }

    selectionHistoryCursor_ = static_cast<std::size_t>(next);
    applyingSelectionHistory_ = true;
    session_->World().Selection().Set(
        std::span<const scene::ObjectId>(
            selectionHistory_[selectionHistoryCursor_]));
}

void StudioViewportPanels::SelectParent()
{
    if (session_ == nullptr || !session_->World().HasWorld()) return;
    const auto& selected = session_->World().Selection().Ordered();
    if (selected.size() != 1U) return;
    const auto object = session_->World().Objects().Find(selected.front());
    if (!object || !object->parent) return;
    const std::array ids{*object->parent};
    session_->World().Selection().Set(ids);
}

void StudioViewportPanels::SelectFirstChild()
{
    if (session_ == nullptr || !session_->World().HasWorld()) return;
    const auto& selected = session_->World().Selection().Ordered();
    if (selected.size() != 1U) return;
    const auto children = session_->World().Objects().Children(selected.front());
    if (children.empty()) return;
    const std::array ids{children.front().id};
    session_->World().Selection().Set(ids);
}

void StudioViewportPanels::DuplicateSelection()
{
    if (session_ == nullptr || !session_->World().HasWorld()) return;
    auto& world = session_->World();
    const auto selected = world.Selection().Ordered();
    if (selected.empty()) return;

    auto& commands = world.Commands();
    commands.BeginTransaction("Duplicate Selection");
    std::vector<scene::ObjectId> duplicates;
    duplicates.reserve(selected.size());

    try
    {
        for (const auto id : selected)
        {
            duplicates.push_back(commands.DuplicateObject(id));
        }
        commands.CommitTransaction();
        world.Selection().Set(std::span<const scene::ObjectId>(duplicates));
        Notify(
            "Duplicated",
            std::format("{} object{} duplicated.",
                duplicates.size(), duplicates.size() == 1U ? "" : "s"));
    }
    catch (const std::exception& exception)
    {
        if (commands.HasActiveTransaction()) commands.RollbackTransaction();
        Notify("Duplicate failed", exception.what(), true);
    }
}

void StudioViewportPanels::DeleteSelection()
{
    if (session_ == nullptr || !session_->World().HasWorld()) return;
    auto& world = session_->World();
    const auto selected = world.Selection().Ordered();
    if (selected.empty()) return;

    auto& commands = world.Commands();
    commands.BeginTransaction("Delete Selection");
    try
    {
        for (const auto id : selected)
        {
            commands.DeleteObject(id);
        }
        commands.CommitTransaction();
        world.Selection().Clear();
        Notify(
            "Deleted",
            std::format("{} object{} removed. Ctrl+Z restores them.",
                selected.size(), selected.size() == 1U ? "" : "s"));
    }
    catch (const std::exception& exception)
    {
        if (commands.HasActiveTransaction()) commands.RollbackTransaction();
        Notify("Delete blocked", exception.what(), true);
    }
}

void StudioViewportPanels::BeginRenameSelection()
{
    if (session_ == nullptr || !session_->World().HasWorld()) return;
    const auto& selected = session_->World().Selection().Ordered();
    if (selected.size() != 1U) return;
    const auto object = session_->World().Objects().Find(selected.front());
    if (!object) return;
    renameText_ = object->name;
    renameSelection_ = true;
    renameFocusRequested_ = true;
}

void StudioViewportPanels::CommitRenameSelection()
{
    if (!renameSelection_ || session_ == nullptr || renameText_.empty()) return;
    const auto selected = session_->World().Selection().Ordered();
    if (selected.size() != 1U) return;
    try
    {
        session_->World().Commands().RenameObject(selected.front(), renameText_);
        renameSelection_ = false;
        Notify("Renamed", renameText_);
    }
    catch (const std::exception& exception)
    {
        Notify("Rename failed", exception.what(), true);
    }
}

void StudioViewportPanels::RevealSelectionInWorld()
{
    if (ui_ == nullptr || session_ == nullptr ||
        session_->World().Selection().Ordered().empty())
    {
        return;
    }
    static_cast<void>(ui_->FocusPanelByTitle("World / Assets"));
    Notify("Selection", "Focused the World / Assets browser. Alt+Up/Down walks hierarchy.");
}

void StudioViewportPanels::ApplyViewportPreset(const i32 preset)
{
    auto& state = expansion_.ViewportState();
    switch (preset)
    {
    case 0: // Focus
        state.SetLayout(ViewportLayout::Single);
        state.modes[0] = studio_session::ViewportMode::Perspective;
        break;
    case 1: // Authoring
        state.SetLayout(ViewportLayout::VerticalSplit);
        state.modes[0] = studio_session::ViewportMode::Perspective;
        state.modes[1] = studio_session::ViewportMode::BodyMap;
        break;
    case 2: // Terrain debug
        state.SetLayout(ViewportLayout::VerticalSplit);
        state.modes[0] = studio_session::ViewportMode::Perspective;
        state.modes[1] = studio_session::ViewportMode::Debug;
        break;
    default: // Full diagnostic
        state.SetLayout(ViewportLayout::Quad);
        state.modes = {
            studio_session::ViewportMode::Perspective,
            studio_session::ViewportMode::BodyMap,
            studio_session::ViewportMode::Debug,
            studio_session::ViewportMode::System
        };
        break;
    }

    if (session_ != nullptr)
    {
        if (session_->Viewports().Find("studio.primary") != nullptr)
            session_->Viewports().SetMode("studio.primary", state.modes[0]);
        if (session_->Viewports().Find("studio.map") != nullptr)
            session_->Viewports().SetMode("studio.map", state.modes[1]);
    }
}

void StudioViewportPanels::DrawQolNavigation(
    editor_ui::PanelContext& context)
{
    TrackSelectionHistory();
    EnsureQolCommands();

    const bool canBack = !selectionHistory_.empty() && selectionHistoryCursor_ > 0U;
    const bool canForward = !selectionHistory_.empty() &&
        selectionHistoryCursor_ + 1U < selectionHistory_.size();

    context.SameLine();
    if (context.Button(canBack ? "<##selection-back" : "-##selection-back"))
        NavigateSelectionHistory(-1);
    context.SameLine();
    if (context.Button(canForward ? ">##selection-forward" : "-##selection-forward"))
        NavigateSelectionHistory(1);

    if (session_ == nullptr || !session_->World().HasWorld()) return;
    const auto& selected = session_->World().Selection().Ordered();
    if (selected.size() == 1U)
    {
        context.SameLine();
        if (renameSelection_)
        {
            if (renameFocusRequested_)
            {
                context.FocusNextItem();
                renameFocusRequested_ = false;
            }
            static_cast<void>(context.InputText("##inline-selection-rename", renameText_));
            if (context.KeyPressed(editor_ui::UiKey::Enter)) CommitRenameSelection();
            if (context.KeyPressed(editor_ui::UiKey::Escape)) renameSelection_ = false;
        }
        else if (context.Button("Rename##selection-rename"))
        {
            BeginRenameSelection();
        }

        context.SameLine();
        if (context.Button("Up##selection-parent")) SelectParent();
        context.SameLine();
        if (context.Button("Down##selection-child")) SelectFirstChild();
    }
}

void StudioViewportPanels::DrawQolHistory(
    editor_ui::PanelContext& context)
{
    if (session_ == nullptr || !session_->World().HasWorld()) return;
    auto& commands = session_->World().Commands();
    const auto undo = commands.UndoLabels(12U);
    const auto redo = commands.RedoLabels(12U);

    context.SameLine();
    const bool open = context.Button("History##studio-history");
    if (!context.BeginPopup("studio-history-popup", open, {.width = 360.0F, .height = 0.0F}))
        return;

    context.Heading("Undo history");
    if (undo.empty())
    {
        context.MutedText("No committed edits.");
    }
    else
    {
        for (std::size_t index = 0U; index < undo.size(); ++index)
        {
            std::string label = std::format("Undo to {}##undo-history-{}", undo[index], index);
            if (context.Selectable(label, false))
            {
                try
                {
                    for (std::size_t step = 0U; step <= index; ++step) commands.Undo();
                    context.CloseCurrentPopup();
                    Notify("History restored", undo[index]);
                }
                catch (const std::exception& exception)
                {
                    Notify("Undo failed", exception.what(), true);
                }
                break;
            }
        }
    }

    if (!redo.empty())
    {
        context.Separator();
        context.MutedText("Redo");
        for (std::size_t index = 0U; index < redo.size(); ++index)
        {
            std::string label = std::format("{}##redo-history-{}", redo[index], index);
            if (context.Selectable(label, false))
            {
                try
                {
                    commands.Redo();
                    context.CloseCurrentPopup();
                    Notify("Redone", redo[index]);
                }
                catch (const std::exception& exception)
                {
                    Notify("Redo failed", exception.what(), true);
                }
                break;
            }
        }
    }
    context.EndPopup();
}

void StudioViewportPanels::DrawQolProperties(
    editor_ui::PanelContext& context)
{
    if (session_ == nullptr || !session_->World().HasWorld()) return;
    auto& world = session_->World();
    auto& inspector = world.Inspector();
    const auto selected = inspector.SelectedObjects();
    if (selected.empty()) return;

    static constexpr std::array<std::string_view, 4> kPresets{
        "Focus", "Authoring", "Terrain", "Diagnostic"
    };
    i32 preset = -1;
    context.MutedText("Viewport preset");
    if (context.SegmentedControl("qol-viewport-preset", kPresets, preset) && preset >= 0)
    {
        ApplyViewportPreset(preset);
    }

    context.Separator();
    context.MutedText("Property filter");
    static_cast<void>(context.InputText("##qol-property-filter", propertyFilter_));
    static_cast<void>(context.Checkbox("Modified only##qol-modified-only", propertyModifiedOnly_));

    auto properties = inspector.CommonProperties();
    const std::string filter = Lower(propertyFilter_);
    std::ranges::stable_sort(
        properties,
        [this](const auto& left, const auto& right)
        {
            const bool lp = IsPinned(pinnedProperties_, left.schema.id);
            const bool rp = IsPinned(pinnedProperties_, right.schema.id);
            if (lp != rp) return lp;
            return left.schema.name < right.schema.name;
        });

    for (const auto& property : properties)
    {
        if (!filter.empty() &&
            Lower(property.schema.name + " " + property.schema.unit).find(filter) == std::string::npos)
        {
            continue;
        }

        bool modified = false;
        for (const auto& object : selected)
        {
            if (world.Objects().GetProperty(object.id, property.schema.id).has_value())
            {
                modified = true;
                break;
            }
        }
        if (propertyModifiedOnly_ && !modified) continue;

        const bool pinned = IsPinned(pinnedProperties_, property.schema.id);
        context.Text(std::format(
            "{}{}{}",
            pinned ? "* " : "",
            property.schema.name,
            modified ? " · modified" : ""));

        if (!property.schema.readOnly)
        {
            std::string pinLabel = pinned ? "Unpin##qol-pin-" : "Pin##qol-pin-";
            pinLabel += property.schema.id.ToString();
            if (context.Button(pinLabel))
            {
                const auto found = std::ranges::find(pinnedProperties_, property.schema.id);
                if (found == pinnedProperties_.end()) pinnedProperties_.push_back(property.schema.id);
                else pinnedProperties_.erase(found);
            }

            context.SameLine();
            std::string resetLabel = "Reset##qol-reset-" + property.schema.id.ToString();
            if (context.Button(resetLabel) && modified)
            {
                auto& commands = world.Commands();
                commands.BeginTransaction("Reset Property");
                try
                {
                    for (const auto& object : selected)
                        commands.ResetProperty(object.id, property.schema.id);
                    commands.CommitTransaction();
                    Notify("Property reset", property.schema.name);
                }
                catch (const std::exception& exception)
                {
                    if (commands.HasActiveTransaction()) commands.RollbackTransaction();
                    Notify("Reset failed", exception.what(), true);
                }
            }

            context.SameLine();
            std::string copyLabel = "Copy##qol-copy-" + property.schema.id.ToString();
            if (context.Button(copyLabel) && !property.mixed)
            {
                copiedPropertyValue_ = property.value;
                Notify("Value copied", property.schema.name);
            }

            context.SameLine();
            std::string pasteLabel = "Paste##qol-paste-" + property.schema.id.ToString();
            if (context.Button(pasteLabel) && copiedPropertyValue_.has_value())
            {
                try
                {
                    inspector.SetForSelection(property.schema.id, *copiedPropertyValue_);
                    Notify("Value pasted", property.schema.name);
                }
                catch (const std::exception& exception)
                {
                    Notify("Paste failed", exception.what(), true);
                }
            }
        }
        context.Separator();
    }

    if (content_ != nullptr)
    {
        if (context.Button("Drop asset here##qol-asset-drop-target"))
        {
            // The button is intentionally inert; it provides a predictable
            // target for the generic drag payload below.
        }
        if (const auto payload = context.AcceptDragPayload("ORBIT_ASSET_PATH"))
        {
            const std::string path(
                reinterpret_cast<const char*>(payload->data()),
                payload->size());
            if (const auto* asset = content_->FindByPath(path); asset != nullptr)
            {
                Notify(
                    "Asset received",
                    std::string(content::AssetKindName(asset->kind)) + " · " +
                        asset->sourcePath.generic_string());
            }
        }
    }
}
} // namespace orbit::studio_ui
