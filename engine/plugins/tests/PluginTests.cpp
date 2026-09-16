#include <orbit/commands/CommandRegistry.hpp>
#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/plugins/PluginManager.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/selection/SelectionService.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
void Write(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    assert(output.good());
    output << text;
}

std::string Manifest(const std::string& permissions = "")
{
    return
        "[plugin]\n"
        "id = \"test.plugin\"\n"
        "version = \"1.0.0\"\n"
        "orbit_api = \"0.0.3\"\n"
        "entry = \"main.luau\"\n"
        "scope = \"editor\"\n"
        "permissions = [" + permissions + "]\n";
}
}

int main()
{
    const auto root = std::filesystem::temp_directory_path() /
        ("orbit-plugin-" + orbit::documents::ProjectId::Random().ToString());
    std::filesystem::remove_all(root);

    auto project = orbit::documents::ProjectDocument::Create(root, "Plugin Test");
    project.Manifest().plugins.push_back({.id = "test.plugin", .version = "1.0.0"});
    project.Save();

    const auto package = root / "Plugins" / "test.plugin";
    Write(package / "plugin.toml", Manifest("\"project_mutation\", \"mcp_registration\""));
    Write(package / "main.luau",
        "Orbit.log('loaded')\n"
        "Orbit.registerCommand('Ping', function() Orbit.log('pong') end, 'Test', 'Ping command', false)\n"
        "Orbit.registerPanel('Test Panel', function() Orbit.ui.text('hello') end)\n");

    orbit::documents::WorldDatabase world(project.StartupWorldPath());
    orbit::schema::SchemaRegistry schemas;
    orbit::scene::ObjectStore objects(world);
    orbit::selection::SelectionService selection;
    orbit::commands::CommandService commandService(objects, schemas);
    orbit::commands::CommandRegistry commandRegistry;

    orbit::plugins::PluginManager plugins(
        root, commandRegistry, commandService, objects, selection);

    orbit::plugins::PluginPermissionSet grants;
    grants.Add(orbit::plugins::PluginPermission::ProjectMutation);
    plugins.SetGrantedPermissions("test.plugin", grants);
    plugins.LoadEnabled(project.Manifest());

    auto statuses = plugins.Statuses();
    assert(statuses.size() == 1);
    assert(statuses[0].loaded);
    assert(statuses[0].requestedPermissions.Contains(
        orbit::plugins::PluginPermission::ProjectMutation));
    assert(statuses[0].requestedPermissions.Contains(
        orbit::plugins::PluginPermission::McpRegistration));
    assert(statuses[0].grantedPermissions.Contains(
        orbit::plugins::PluginPermission::ProjectMutation));
    assert(!statuses[0].grantedPermissions.Contains(
        orbit::plugins::PluginPermission::McpRegistration));
    assert(commandRegistry.Catalog().size() == 1);
    assert(plugins.PanelCatalog().size() == 1);

    const auto revision = statuses[0].revision;
    Write(package / "main.luau",
        "Orbit.registerCommand('Pong', function() end, 'Test', 'Reloaded', false)\n");
    assert(plugins.PollHotReload() == 1);
    statuses = plugins.Statuses();
    assert(statuses[0].loaded);
    assert(statuses[0].revision > revision);
    assert(commandRegistry.Catalog().size() == 1);
    assert(commandRegistry.Catalog()[0].name == "Pong");
    assert(plugins.PanelCatalog().empty());

    // A denied privileged API must fail inside the sandbox rather than
    // silently escalating the plugin's capabilities.
    Write(package / "main.luau",
        "Orbit.registerCommand('AgentTool', function() end, 'Test', '', true)\n");
    assert(plugins.PollHotReload() == 1);
    statuses = plugins.Statuses();
    assert(!statuses[0].loaded);
    assert(!statuses[0].error.empty());
    assert(commandRegistry.Catalog().empty());

    // The host intentionally exposes no file/process/network libraries.
    Write(package / "main.luau",
        "assert(io == nil)\nassert(os == nil)\nassert(debug == nil)\n");
    assert(plugins.PollHotReload() == 1);
    statuses = plugins.Statuses();
    assert(statuses[0].loaded);

    // A malformed sibling package must be isolated. Valid enabled plugins
    // continue loading and registering commands in the same editor session.
    project.Manifest().plugins.push_back({
        .id = "broken.plugin",
        .version = "1.0.0"
    });
    project.Save();

    Write(
        root / "Plugins" / "broken.plugin" / "plugin.toml",
        "[plugin\nid =");
    Write(
        package / "main.luau",
        "Orbit.registerCommand('StillLoaded', function() end, 'Test', '', false)\n");

    plugins.LoadEnabled(project.Manifest());

    statuses = plugins.Statuses();
    assert(statuses.size() == 2);

    bool foundHealthy = false;
    bool foundBroken = false;

    for (const auto& status : statuses)
    {
        if (status.id == "test.plugin")
        {
            foundHealthy = status.loaded;
        }
        else if (status.id == "broken.plugin")
        {
            foundBroken =
                !status.loaded &&
                !status.error.empty();
        }
    }

    assert(foundHealthy);
    assert(foundBroken);
    assert(commandRegistry.Catalog().size() == 1);
    assert(commandRegistry.Catalog()[0].name == "StillLoaded");

    std::filesystem::remove_all(root);
    return 0;
}
