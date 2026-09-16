#pragma once

#include <orbit/commands/CommandRegistry.hpp>
#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectManifest.hpp>
#include <orbit/editor_model/CommandSurfaces.hpp>
#include <orbit/editor_ui/EditorUi.hpp>
#include <orbit/plugins/PluginManifest.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/selection/SelectionService.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace orbit::plugins
{
struct PluginPanelDescriptor
{
    editor_ui::PanelId id{};
    std::string pluginId;
    std::string title;
};

struct PluginStatus
{
    std::string id;
    std::string version;
    bool loaded{false};
    std::string error;
    PluginPermissionSet requestedPermissions{};
    PluginPermissionSet grantedPermissions{};
    u64 revision{0};
};

class PluginManager
{
public:
    PluginManager(
        std::filesystem::path projectRoot,
        commands::CommandRegistry& commandRegistry,
        commands::CommandService& commandService,
        editor_model::CommandSurfaceRegistry& commandSurfaces,
        scene::ObjectStore& objects,
        selection::SelectionService& selection);

    ~PluginManager();

    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    void SetGrantedPermissions(
        std::string pluginId,
        PluginPermissionSet permissions);

    // ProjectManifest::plugins is the authoritative enabled-plugin list
    // for this project. Packages live beneath <project>/Plugins/<id>/.
    void LoadEnabled(
        const documents::ProjectManifest& project);

    // Source-content based hot reload; no timestamp resolution assumptions.
    // Returns the number of plugins whose VM was reloaded.
    [[nodiscard]] u32 PollHotReload();

    [[nodiscard]] bool Reload(
        std::string_view pluginId);

    [[nodiscard]] std::vector<PluginStatus>
    Statuses() const;

    [[nodiscard]] std::vector<PluginPanelDescriptor>
    PanelCatalog() const;

    [[nodiscard]] u64 PanelCatalogRevision() const noexcept;

    // Draws one registered Luau panel through Orbit's restricted
    // PanelContext bridge. Returns false when the panel no longer exists.
    [[nodiscard]] bool DrawPanel(
        editor_ui::PanelId panel,
        editor_ui::PanelContext& context);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbit::plugins
