#include <orbit/studio_ui/StudioExpansionShell.hpp>

#include <orbit/content/ContentService.hpp>
#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/editor_model/SurfaceAuthoringModel.hpp>
#include <orbit/editor_ui/FocusState.hpp>
#include <orbit/editor_ui/PanelExtensions.hpp>
#include <orbit/paths/PathNetwork.hpp>
#include <orbit/studio_ui/CommandPaletteModel.hpp>
#include <orbit/studio_ui/SelectionBreadcrumbs.hpp>
#include <orbit/studio_ui/StudioViewportPanels.hpp>
#include <orbit/studio_ui/VolumeAuthoringUi.hpp>
#include <orbit/terrain_debug/TerrainDebugField.hpp>
#include <orbit/terrain_debug/TerrainDebugSeam.hpp>
#include <orbit/world_model/VisibilityProxyBinding.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <format>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "StudioShellInternals.hpp"

namespace orbit::studio_ui
{
using namespace shell_detail;

namespace
{
enum class CreationCategory : u8 { Meshes, Procedurals, Lighting, Vfx, Sfx };

CreationCategory CategoryForTool(std::string_view category, std::string_view label)
{
    const std::string text = PaletteLower(std::string(category) + " " + std::string(label));
    const auto has = [&](std::string_view term) { return text.find(term) != std::string::npos; };
    if (has("sfx") || has("audio") || has("sound")) return CreationCategory::Sfx;
    if (has("light") || has("lamp")) return CreationCategory::Lighting;
    if (has("vfx") || has("volum") || has("particle") || has("smoke") || has("fire") || has("fog")) return CreationCategory::Vfx;
    if (has("procedural") || has("terrain") || has("surface") || has("path") || has("spline")) return CreationCategory::Procedurals;
    if (has("mesh") || has("primitive") || has("proxy") || has("material")) return CreationCategory::Meshes;
    return CreationCategory::Procedurals;
}
}

void StudioExpansionShell::DrawCreationMenus(editor_ui::PanelContext& context, bool compact)
{
    if (owner_ == nullptr || owner_->session_ == nullptr || !owner_->session_->World().HasWorld()) return;
    using editor_ui::ToolbarIcon;
    using editor_ui::ToolbarStyle;
    struct Category { std::string_view label; ToolbarIcon icon; };
    static constexpr std::array<Category,5> categories{{
        {"Meshes",ToolbarIcon::Box}, {"Procedurals",ToolbarIcon::Procedural},
        {"Lighting",ToolbarIcon::PointLight}, {"VFX",ToolbarIcon::Vfx}, {"SFX",ToolbarIcon::Sfx}}};
    auto& world = owner_->session_->World();
    auto& registry = world.CommandRegistry();
    const auto palette = BuildCommandPalette(registry.Catalog());
    const auto contributions = GlobalStudioUiContributions().Catalog(StudioContributionSurface::QuickCreate);
    for (std::size_t i=0; i<categories.size(); ++i)
    {
        if (i != 0) context.SameLine();
        const auto category = static_cast<CreationCategory>(i);
        const std::string popupId = "toolbar-create-" + std::to_string(i);
        const bool open = context.ToolbarButton(std::string(categories[i].label)+"##"+popupId,
            categories[i].icon,false,true,compact,ToolbarStyle::Menu);
        context.AnchorNextPopupBelowItem();
        if (!context.BeginPopup(popupId,open,{320.0F * editor_ui::CurrentUiScale(),0})) continue;
        context.Heading(categories[i].label);
        context.Separator();
        std::vector<editor_ui::ActionPresentation> actions;
        const bool canCreate = owner_->CanCreateAtViewport("studio.primary");
        const auto builtin = [&](std::string label, const auto& action)
        {
            actions.push_back({.label=std::move(label), .enabled=canCreate,
                .disabledReason="Focus a perspective viewport to place this object.",
                .invoke=[this, action, &context]
                {
                    try { action(); owner_->status_.clear(); context.CloseCurrentPopup(); }
                    catch (const std::exception& e) { owner_->status_=e.what(); }
                }});
        };
        if (category == CreationCategory::Meshes)
        {
            builtin("Box",[this] { owner_->CreateVisibilityProxyAtViewport("studio.primary",true); });
            builtin("Sphere",[this] { owner_->CreateVisibilityProxyAtViewport("studio.primary",false); });
            using world_model::PrimitiveShape;
            using world_model::PrimitiveSurface;
            builtin("Primitive Box",[this] { owner_->CreatePrimitiveAtViewport("studio.primary",PrimitiveShape::Box,PrimitiveSurface::Standard); });
            builtin("Primitive Sphere",[this] { owner_->CreatePrimitiveAtViewport("studio.primary",PrimitiveShape::Sphere,PrimitiveSurface::Standard); });
            builtin("Mirror Sphere",[this] { owner_->CreatePrimitiveAtViewport("studio.primary",PrimitiveShape::Sphere,PrimitiveSurface::Mirror); });
            builtin("Glass Sphere",[this] { owner_->CreatePrimitiveAtViewport("studio.primary",PrimitiveShape::Sphere,PrimitiveSurface::Glass); });
            builtin("Emissive Sphere",[this] { owner_->CreatePrimitiveAtViewport("studio.primary",PrimitiveShape::Sphere,PrimitiveSurface::Emissive); });
        }
        if (category == CreationCategory::Lighting)
        {
            builtin("Point Light",[this] { owner_->CreateLocalLightAtViewport("studio.primary",false); });
            builtin("Spot Light",[this] { owner_->CreateLocalLightAtViewport("studio.primary",true); });
        }
        for (const auto& entry : palette)
        {
            const auto contribution = std::ranges::find_if(contributions,[&](const auto& value)
                { return value.kind == StudioContributionKind::Command && value.command == entry.command; });
            if (!IsQuickCreateLabel(entry.label) && contribution == contributions.end()) continue;
            const std::string_view group = contribution != contributions.end() && !contribution->category.empty()
                ? std::string_view(contribution->category) : std::string_view(entry.category);
            if (CategoryForTool(group,entry.label) != category) continue;
            const auto enablement = registry.Enablement(entry.command);
            actions.push_back({.label=entry.label+"##category-"+entry.command.ToString(),
                .enabled=enablement.enabled || entry.requiresArguments,
                .disabledReason=enablement.reason,
                .invoke=[this, &registry, &context, command=entry.command, arguments=entry.requiresArguments]
                {
                    try
                    {
                        if (arguments) toolbarArgumentCommand_=command;
                        else registry.Invoke(command);
                        owner_->status_.clear(); context.CloseCurrentPopup();
                    }
                    catch (const std::exception& e) { owner_->status_=e.what(); }
                }});
        }
        if (actions.empty()) context.MutedText("No creation tools registered in this category.");
        else static_cast<void>(context.ActionList(actions));
        context.EndPopup();
    }
}

void StudioExpansionShell::DrawNavigationBand(editor_ui::PanelContext& context)
{
    // Service palette/argument popups without a permanent breadcrumb row.
    SyncPersistentState();
    if (owner_ == nullptr || owner_->session_ == nullptr || !owner_->session_->World().HasWorld()) return;
    auto& registry = owner_->session_->World().CommandRegistry();
    const auto commandCatalog = registry.Catalog();
    const auto palette = BuildCommandPalette(commandCatalog);
    const bool openCommandPalette = std::exchange(commandPaletteOpenRequested_, false);
    DrawCommandPalettePopup(context,openCommandPalette,registry,palette);
    DrawQuickCreateBrowser(context,toolbarArgumentCommand_.IsValid(),commandCatalog,registry,palette);
    static_cast<void>(DrawContributions(context,StudioContributionSurface::WorkspaceToolbar,true));
}
} // namespace orbit::studio_ui
