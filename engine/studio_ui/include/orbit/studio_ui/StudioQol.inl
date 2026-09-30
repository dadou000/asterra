#pragma once

#include <orbit/content/ContentService.hpp>
#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/editor_model/ShortcutRegistry.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orbit::studio_ui
{
namespace studio_qol_detail
{
[[nodiscard]] inline std::string Lower(std::string value)
{
    std::ranges::transform(value, value.begin(), [](const unsigned char c)
    {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

[[nodiscard]] inline bool IsPinned(
    const std::vector<schema::PropertyId>& pinned,
    const schema::PropertyId property) noexcept
{
    return std::ranges::find(pinned, property) != pinned.end();
}

[[nodiscard]] inline std::vector<std::string_view> AssetPropertyKeywords(
    const content::AssetKind kind)
{
    using content::AssetKind;
    switch (kind)
    {
    case AssetKind::Texture: return {"texture", "albedo", "normal", "roughness", "metallic", "emissive", "image"};
    case AssetKind::Material:
    case AssetKind::MaterialInstance:
    case AssetKind::ShaderMaterial: return {"material"};
    case AssetKind::Decal: return {"decal"};
    case AssetKind::Component: return {"component"};
    case AssetKind::Mesh: return {"mesh", "model"};
    case AssetKind::PathProfile: return {"profile", "path profile"};
    case AssetKind::Shader:
    case AssetKind::ShadingShader: return {"shader"};
    case AssetKind::ColorLut: return {"lut", "lookup"};
    case AssetKind::Unknown: return {};
    }
    return {};
}
} // namespace studio_qol_detail

inline void StudioViewportPanels::Notify(
    std::string title,
    std::string detail,
    const bool error) noexcept
{
    if (error)
        status_ = title + (detail.empty() ? std::string{} : ": " + detail);
    else
        status_.clear();

    if (ui_ == nullptr) return;
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
    catch (...) {}
}

inline void StudioViewportPanels::InstallQol(editor_ui::EditorUi* ui) noexcept
{
    if (ui != nullptr) ui_ = ui;

    try
    {
        GlobalInspectorProviders().Upsert({
            .id = "orbit.selection.qol",
            .owner = "orbit",
            .title = "Workflow",
            .order = 10,
            .defaultOpen = true,
            .relevant = [this]
            {
                return session_ != nullptr && session_->World().HasWorld();
            },
            .draw = [this](editor_ui::PanelContext& context)
            {
                DrawQolProperties(context);
            }
        });
    }
    catch (...) {}

    if (qolInstalled_) return;
    auto* shortcuts = editor_model::ShortcutRegistry::Active();
    if (shortcuts == nullptr) return;

    try
    {
        shortcuts->RegisterCallback({.key = platform::Key::P, .control = true}, [this] { RequestCommandPaletteOpen(); });
        shortcuts->RegisterCallback({.key = platform::Key::LetterA, .shift = true}, [this] { expansion_.RequestQuickCreateOpen(); });
        shortcuts->RegisterCallback({.key = platform::Key::F2}, [this] { BeginRenameSelection(); });
        shortcuts->RegisterCallback({.key = platform::Key::Delete}, [this] { DeleteSelection(); });
        shortcuts->RegisterCallback({.key = platform::Key::D, .control = true}, [this] { DuplicateSelection(); });
        shortcuts->RegisterCallback({.key = platform::Key::G}, [this] { RevealSelectionInWorld(); });
        shortcuts->RegisterCallback({.key = platform::Key::ArrowLeft, .alt = true}, [this] { NavigateSelectionHistory(-1); });
        shortcuts->RegisterCallback({.key = platform::Key::ArrowRight, .alt = true}, [this] { NavigateSelectionHistory(1); });
        shortcuts->RegisterCallback({.key = platform::Key::ArrowUp, .alt = true}, [this] { SelectParent(); });
        shortcuts->RegisterCallback({.key = platform::Key::ArrowDown, .alt = true}, [this] { SelectFirstChild(); });
        shortcuts->RegisterCallback({.key = platform::Key::Q}, [this] { expansion_.ViewportState().gizmo.tool = GizmoTool::Select; });
        shortcuts->RegisterCallback({.key = platform::Key::W}, [this] { expansion_.ViewportState().gizmo.tool = GizmoTool::Translate; });
        shortcuts->RegisterCallback({.key = platform::Key::E}, [this] { expansion_.ViewportState().gizmo.tool = GizmoTool::Rotate; });
        shortcuts->RegisterCallback({.key = platform::Key::F4}, [this] { expansion_.ViewportState().gizmo.tool = GizmoTool::Scale; });
        qolInstalled_ = true;
    }
    catch (...) {}
}

inline void StudioViewportPanels::EnsureQolCommands()
{
    if (session_ == nullptr || !session_->World().HasWorld()) return;
    auto& registry = session_->World().CommandRegistry();

    if (registry.Find(editor_model::authoring_commands::kDuplicateSelection) == nullptr)
    {
        registry.Register({
            .id = editor_model::authoring_commands::kDuplicateSelection,
            .name = "Duplicate Selection",
            .category = "Edit",
            .description = "Duplicate selected leaf objects as one undoable transaction.",
            .presentationSurfaces = {"explorer.context", "viewport.context", "properties.toolbar"},
            .enablement = [this]() -> commands::CommandEnablement
            {
                if (session_ == nullptr || !session_->World().HasWorld())
                    return {.enabled = false, .reason = "No world is open."};
                auto& world = session_->World();
                if (world.Selection().Ordered().empty())
                    return {.enabled = false, .reason = "Nothing is selected."};
                for (const auto id : world.Selection().Ordered())
                    if (!world.Objects().Children(id).empty())
                        return {.enabled = false, .reason = "Generic duplicate is limited to leaf objects."};
                return {};
            },
            .invoke = [this](const commands::CommandArguments&) { DuplicateSelection(); }
        });
    }

    if (registry.Find(editor_model::authoring_commands::kDeleteSelection) == nullptr)
    {
        registry.Register({
            .id = editor_model::authoring_commands::kDeleteSelection,
            .name = "Delete Selection",
            .category = "Edit",
            .description = "Delete selected leaf objects. The operation is undoable.",
            .presentationSurfaces = {"explorer.context", "viewport.context", "properties.toolbar"},
            .enablement = [this]() -> commands::CommandEnablement
            {
                if (session_ == nullptr || !session_->World().HasWorld())
                    return {.enabled = false, .reason = "No world is open."};
                auto& world = session_->World();
                if (world.Selection().Ordered().empty())
                    return {.enabled = false, .reason = "Nothing is selected."};
                for (const auto id : world.Selection().Ordered())
                    if (!world.Objects().Children(id).empty())
                        return {.enabled = false, .reason = "Objects with children are protected from generic Delete."};
                return {};
            },
            .invoke = [this](const commands::CommandArguments&) { DeleteSelection(); }
        });
    }
}

inline void StudioViewportPanels::TrackSelectionHistory()
{
    if (session_ == nullptr || !session_->World().HasWorld()) return;
    auto& selection = session_->World().Selection();
    if (selection.Revision() == observedSelectionRevision_) return;
    observedSelectionRevision_ = selection.Revision();
    if (applyingSelectionHistory_)
    {
        applyingSelectionHistory_ = false;
        return;
    }

    const auto current = selection.Ordered();
    if (!selectionHistory_.empty() && selectionHistoryCursor_ < selectionHistory_.size() &&
        selectionHistory_[selectionHistoryCursor_] == current) return;
    if (!selectionHistory_.empty() && selectionHistoryCursor_ + 1U < selectionHistory_.size())
        selectionHistory_.erase(
            selectionHistory_.begin() + static_cast<std::ptrdiff_t>(selectionHistoryCursor_ + 1U),
            selectionHistory_.end());
    selectionHistory_.push_back(current);
    if (selectionHistory_.size() > 64U) selectionHistory_.erase(selectionHistory_.begin());
    selectionHistoryCursor_ = selectionHistory_.empty() ? 0U : selectionHistory_.size() - 1U;
}

inline void StudioViewportPanels::NavigateSelectionHistory(const i32 delta)
{
    TrackSelectionHistory();
    if (session_ == nullptr || selectionHistory_.empty()) return;
    const i64 current = static_cast<i64>(selectionHistoryCursor_);
    const i64 next = std::clamp<i64>(current + delta, 0, static_cast<i64>(selectionHistory_.size() - 1U));
    if (next == current) return;
    selectionHistoryCursor_ = static_cast<std::size_t>(next);
    applyingSelectionHistory_ = true;
    session_->World().Selection().Set(std::span<const scene::ObjectId>(selectionHistory_[selectionHistoryCursor_]));
}

inline void StudioViewportPanels::SelectParent()
{
    if (session_ == nullptr || !session_->World().HasWorld()) return;
    const auto& selected = session_->World().Selection().Ordered();
    if (selected.size() != 1U) return;
    const auto object = session_->World().Objects().Find(selected.front());
    if (!object || !object->parent) return;
    const std::array ids{*object->parent};
    session_->World().Selection().Set(ids);
}

inline void StudioViewportPanels::SelectFirstChild()
{
    if (session_ == nullptr || !session_->World().HasWorld()) return;
    const auto& selected = session_->World().Selection().Ordered();
    if (selected.size() != 1U) return;
    const auto children = session_->World().Objects().Children(selected.front());
    if (children.empty()) return;
    const std::array ids{children.front().id};
    session_->World().Selection().Set(ids);
}

inline void StudioViewportPanels::DuplicateSelection()
{
    if (session_ == nullptr || !session_->World().HasWorld()) return;
    auto& world = session_->World();
    const auto selected = world.Selection().Ordered();
    if (selected.empty()) return;
    auto& service = world.Commands();
    if (service.HasActiveTransaction()) return;
    service.BeginTransaction("Duplicate Selection");
    std::vector<scene::ObjectId> duplicates;
    duplicates.reserve(selected.size());
    try
    {
        for (const auto id : selected) duplicates.push_back(service.DuplicateObject(id));
        service.CommitTransaction();
        world.Selection().Set(std::span<const scene::ObjectId>(duplicates));
        Notify("Duplicated", std::format("{} object{} duplicated.", duplicates.size(), duplicates.size() == 1U ? "" : "s"));
    }
    catch (const std::exception& exception)
    {
        if (service.HasActiveTransaction()) service.RollbackTransaction();
        Notify("Duplicate failed", exception.what(), true);
    }
}

inline void StudioViewportPanels::DeleteSelection()
{
    if (session_ == nullptr || !session_->World().HasWorld()) return;
    auto& world = session_->World();
    const auto selected = world.Selection().Ordered();
    if (selected.empty()) return;
    auto& service = world.Commands();
    if (service.HasActiveTransaction()) return;
    service.BeginTransaction("Delete Selection");
    try
    {
        for (const auto id : selected) service.DeleteObject(id);
        service.CommitTransaction();
        world.Selection().Clear();
        Notify("Deleted", std::format("{} object{} removed. Ctrl+Z restores them.", selected.size(), selected.size() == 1U ? "" : "s"));
    }
    catch (const std::exception& exception)
    {
        if (service.HasActiveTransaction()) service.RollbackTransaction();
        Notify("Delete blocked", exception.what(), true);
    }
}

inline void StudioViewportPanels::BeginRenameSelection()
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

inline void StudioViewportPanels::CommitRenameSelection()
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
    catch (const std::exception& exception) { Notify("Rename failed", exception.what(), true); }
}

inline void StudioViewportPanels::RevealSelectionInWorld()
{
    if (session_ == nullptr || session_->World().Selection().Ordered().empty()) return;
    if (ui_ != nullptr) static_cast<void>(ui_->FocusPanelByTitle("World / Assets"));
    Notify("Selection revealed", "Alt+Up/Down walks the semantic hierarchy.");
}

inline void StudioViewportPanels::ApplyViewportPreset(const i32 preset)
{
    auto& state = expansion_.ViewportState();
    switch (preset)
    {
    case 0:
        state.SetLayout(ViewportLayout::Single);
        state.modes[0] = studio_session::ViewportMode::Perspective;
        break;
    case 1:
        state.SetLayout(ViewportLayout::VerticalSplit);
        state.modes[0] = studio_session::ViewportMode::Perspective;
        state.modes[1] = studio_session::ViewportMode::BodyMap;
        break;
    case 2:
        state.SetLayout(ViewportLayout::VerticalSplit);
        state.modes[0] = studio_session::ViewportMode::Perspective;
        state.modes[1] = studio_session::ViewportMode::Debug;
        break;
    default:
        state.SetLayout(ViewportLayout::Quad);
        state.modes = {studio_session::ViewportMode::Perspective, studio_session::ViewportMode::BodyMap,
            studio_session::ViewportMode::Debug, studio_session::ViewportMode::System};
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

inline void StudioViewportPanels::DrawQolNavigation(editor_ui::PanelContext& context)
{
    TrackSelectionHistory();
    EnsureQolCommands();
    const bool canBack = !selectionHistory_.empty() && selectionHistoryCursor_ > 0U;
    const bool canForward = !selectionHistory_.empty() && selectionHistoryCursor_ + 1U < selectionHistory_.size();
    if (context.Button(canBack ? "Back##qol-back" : "Back -##qol-back")) NavigateSelectionHistory(-1);
    context.SameLine();
    if (context.Button(canForward ? "Forward##qol-forward" : "Forward -##qol-forward")) NavigateSelectionHistory(1);

    if (session_ == nullptr || !session_->World().HasWorld()) return;
    const auto& selected = session_->World().Selection().Ordered();
    if (selected.size() != 1U) return;
    context.SameLine();
    if (renameSelection_)
    {
        if (renameFocusRequested_)
        {
            context.FocusNextItem();
            renameFocusRequested_ = false;
        }
        static_cast<void>(context.InputText("##qol-inline-rename", renameText_));
        if (context.KeyPressed(editor_ui::UiKey::Enter)) CommitRenameSelection();
        if (context.KeyPressed(editor_ui::UiKey::Escape)) renameSelection_ = false;
    }
    else if (context.Button("Rename##qol-rename")) BeginRenameSelection();
    context.SameLine();
    if (context.Button("Parent##qol-parent")) SelectParent();
    context.SameLine();
    if (context.Button("Child##qol-child")) SelectFirstChild();
}

inline void StudioViewportPanels::DrawQolHistory(editor_ui::PanelContext& context)
{
    if (session_ == nullptr || !session_->World().HasWorld()) return;
    auto& service = session_->World().Commands();
    const auto undo = service.UndoLabels(12U);
    const auto redo = service.RedoLabels(12U);
    const bool open = context.Button("History##qol-history");
    if (!context.BeginPopup("qol-history-popup", open, {.width = 360.0F, .height = 0.0F})) return;
    context.Heading("Undo history");
    if (undo.empty()) context.MutedText("No committed edits.");
    for (std::size_t index = 0U; index < undo.size(); ++index)
    {
        const std::string label = std::format("Undo to {}##qol-undo-{}", undo[index], index);
        if (!context.Selectable(label, false)) continue;
        try
        {
            for (std::size_t step = 0U; step <= index; ++step) service.Undo();
            context.CloseCurrentPopup();
            Notify("History restored", undo[index]);
        }
        catch (const std::exception& exception) { Notify("Undo failed", exception.what(), true); }
        break;
    }
    if (!redo.empty())
    {
        context.Separator();
        context.MutedText("Redo");
        for (std::size_t index = 0U; index < redo.size(); ++index)
        {
            const std::string label = std::format("{}##qol-redo-{}", redo[index], index);
            if (!context.Selectable(label, false)) continue;
            try
            {
                service.Redo();
                context.CloseCurrentPopup();
                Notify("Redone", redo[index]);
            }
            catch (const std::exception& exception) { Notify("Redo failed", exception.what(), true); }
            break;
        }
    }
    context.EndPopup();
}

inline void StudioViewportPanels::DrawQolProperties(editor_ui::PanelContext& context)
{
    if (session_ == nullptr || !session_->World().HasWorld()) return;
    TrackSelectionHistory();
    EnsureQolCommands();

    context.MutedText("Navigation");
    DrawQolNavigation(context);
    context.SameLine();
    DrawQolHistory(context);

    auto& world = session_->World();
    auto& inspector = world.Inspector();
    const auto selected = inspector.SelectedObjects();

    context.Separator();
    static constexpr std::array<std::string_view, 4> kPresets{"Focus", "Authoring", "Terrain", "Diagnostic"};
    i32 preset = -1;
    context.MutedText("Viewport preset");
    if (context.SegmentedControl("qol-viewport-preset", kPresets, preset) && preset >= 0) ApplyViewportPreset(preset);

    if (selected.empty())
    {
        context.MutedText("Select an object for property and hierarchy actions.");
        return;
    }

    context.Separator();
    context.MutedText("Selection actions");
    if (context.Button("Duplicate##qol-duplicate")) DuplicateSelection();
    context.SameLine();
    if (context.Button("Delete##qol-delete")) DeleteSelection();
    context.SameLine();
    if (context.Button("Reveal##qol-reveal")) RevealSelectionInWorld();

    context.Separator();
    context.MutedText("Property filter");
    static_cast<void>(context.InputText("##qol-property-filter", propertyFilter_));
    static_cast<void>(context.Checkbox("Modified only##qol-modified-only", propertyModifiedOnly_));

    auto properties = inspector.CommonProperties();
    const std::string filter = studio_qol_detail::Lower(propertyFilter_);
    std::ranges::stable_sort(properties, [this](const auto& left, const auto& right)
    {
        const bool lp = studio_qol_detail::IsPinned(pinnedProperties_, left.schema.id);
        const bool rp = studio_qol_detail::IsPinned(pinnedProperties_, right.schema.id);
        if (lp != rp) return lp;
        return left.schema.name < right.schema.name;
    });

    for (const auto& property : properties)
    {
        if (!filter.empty() && studio_qol_detail::Lower(property.schema.name + " " + property.schema.unit).find(filter) == std::string::npos)
            continue;
        bool modified = false;
        for (const auto& object : selected)
            if (world.Objects().GetProperty(object.id, property.schema.id).has_value())
            {
                modified = true;
                break;
            }
        if (propertyModifiedOnly_ && !modified) continue;

        const bool pinned = studio_qol_detail::IsPinned(pinnedProperties_, property.schema.id);
        context.Text(std::format("{}{}{}", pinned ? "* " : "", property.schema.name, modified ? " · modified" : ""));
        if (property.schema.readOnly)
        {
            context.MutedText("Read only");
            context.Separator();
            continue;
        }

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
            auto& service = world.Commands();
            if (!service.HasActiveTransaction())
            {
                service.BeginTransaction("Reset Property");
                try
                {
                    for (const auto& object : selected) service.ResetProperty(object.id, property.schema.id);
                    service.CommitTransaction();
                    Notify("Property reset", property.schema.name);
                }
                catch (const std::exception& exception)
                {
                    if (service.HasActiveTransaction()) service.RollbackTransaction();
                    Notify("Reset failed", exception.what(), true);
                }
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
            catch (const std::exception& exception) { Notify("Paste failed", exception.what(), true); }
        }
        context.Separator();
    }

    if (content_ != nullptr)
    {
        context.MutedText("Smart asset drop");
        static_cast<void>(context.Button("Drop asset on selection##qol-asset-drop"));
        if (const auto payload = context.AcceptDragPayload("ORBIT_ASSET_PATH"))
        {
            const std::string path(reinterpret_cast<const char*>(payload->data()), payload->size());
            const auto* asset = content_->FindByPath(path);
            if (asset == nullptr)
            {
                Notify("Asset drop failed", "The dragged asset is no longer indexed.", true);
            }
            else
            {
                const auto keywords = studio_qol_detail::AssetPropertyKeywords(asset->kind);
                std::vector<const editor_model::InspectedProperty*> candidates;
                for (const auto& property : properties)
                {
                    if (property.schema.readOnly || property.schema.kind != schema::PropertyKind::String) continue;
                    const std::string name = studio_qol_detail::Lower(property.schema.name);
                    if (std::ranges::any_of(keywords, [&name](const std::string_view keyword)
                        {
                            return name.find(keyword) != std::string::npos;
                        }))
                        candidates.push_back(&property);
                }

                if (candidates.size() == 1U)
                {
                    try
                    {
                        inspector.SetForSelection(candidates.front()->schema.id, path);
                        MarkStudioAssetRecent(asset->sourcePath);
                        Notify("Asset assigned", candidates.front()->schema.name + " · " + path);
                    }
                    catch (const std::exception& exception) { Notify("Asset assignment failed", exception.what(), true); }
                }
                else if (candidates.empty())
                    Notify("Asset received", "No unambiguous compatible string property exists on this selection.");
                else
                    Notify("Choose target property", std::format("{} compatible properties match this {} asset.", candidates.size(), content::AssetKindName(asset->kind)));
            }
        }
    }

    context.Separator();
    context.MutedText("Shortcuts: Ctrl+P commands · Shift+A quick create · F2 rename · Ctrl+D duplicate · Delete remove · G reveal · Alt+Left/Right history · Alt+Up/Down hierarchy · Q/W/E/F4 gizmo");
}
} // namespace orbit::studio_ui
