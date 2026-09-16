#include <orbit/commands/CommandRegistry.hpp>
#include <orbit/commands/CommandService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/editor_model/CommandSurfaces.hpp>
#include <orbit/plugins/PluginManager.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/selection/SelectionService.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
#include <string>

namespace
{
void Check(
    const bool condition,
    const std::source_location location =
        std::source_location::current())
{
    if (!condition)
    {
        std::cerr
            << "Plugin test check failed at "
            << location.file_name()
            << ':'
            << location.line()
            << '\n';
        std::exit(1);
    }
}

void Write(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    Check(output.good());
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
    project.Manifest().plugins.push_back({
        .id = "test.plugin",
        .version = "1.0.0",
        .grantedPermissions = {
            "project_mutation"
        }
    });
    project.Save();

    const auto package = root / "Plugins" / "test.plugin";
    Write(package / "plugin.toml", Manifest("\"project_mutation\", \"mcp_registration\""));
    Write(package / "main.luau",
        "Orbit.log('loaded')\n"
        "local ping = Orbit.registerCommand('Ping', function() Orbit.log('pong') end, 'Test', 'Ping command', false)\n"
        "Orbit.registerContextAction(ping, 'explorer', 'context')\n"
        "Orbit.registerValidator(function() return 'test validation issue' end)\n"
        "Orbit.registerPanel('Test Panel', function() Orbit.ui.text('hello') end)\n");

    orbit::documents::WorldDatabase world(project.StartupWorldPath());
    orbit::schema::SchemaRegistry schemas;
    orbit::scene::ObjectStore objects(world);
    orbit::selection::SelectionService selection;
    orbit::commands::CommandService commandService(objects, schemas);
    orbit::commands::CommandRegistry commandRegistry;
    orbit::editor_model::CommandSurfaceRegistry commandSurfaces;

    orbit::plugins::PluginManager plugins(
        root,
        commandRegistry,
        commandService,
        commandSurfaces,
        objects,
        selection);

    plugins.LoadEnabled(project.Manifest());

    auto statuses = plugins.Statuses();
    Check(statuses.size() == 1);
    Check(statuses[0].loaded);
    Check(statuses[0].requestedPermissions.Contains(
        orbit::plugins::PluginPermission::ProjectMutation));
    Check(statuses[0].requestedPermissions.Contains(
        orbit::plugins::PluginPermission::McpRegistration));
    Check(statuses[0].grantedPermissions.Contains(
        orbit::plugins::PluginPermission::ProjectMutation));
    Check(!statuses[0].grantedPermissions.Contains(
        orbit::plugins::PluginPermission::McpRegistration));
    Check(commandRegistry.Catalog().size() == 1);
    Check(!commandRegistry.Catalog()[0].automationVisible);
    Check(plugins.PanelCatalog().size() == 1);
    Check(
        commandSurfaces.Commands(
            "explorer",
            orbit::editor_model::CommandSurfaceKind::ContextMenu).size() == 1);

    const auto validationIssues = plugins.Validate();
    Check(validationIssues.size() == 1);
    Check(validationIssues[0].pluginId == "test.plugin");
    Check(validationIssues[0].message == "test validation issue");

    const auto revision = statuses[0].revision;
    Write(package / "main.luau",
        "Orbit.registerCommand('Pong', function() end, 'Test', 'Reloaded', false)\n");
    Check(plugins.PollHotReload() == 1);
    statuses = plugins.Statuses();
    Check(statuses[0].loaded);
    Check(statuses[0].revision > revision);
    Check(commandRegistry.Catalog().size() == 1);
    Check(commandRegistry.Catalog()[0].name == "Pong");
    Check(plugins.PanelCatalog().empty());
    Check(
        commandSurfaces.Commands(
            "explorer",
            orbit::editor_model::CommandSurfaceKind::ContextMenu).empty());
    Check(plugins.Validate().empty());

    // A denied privileged API must fail inside the sandbox rather than
    // silently escalating the plugin's capabilities.
    Write(package / "main.luau",
        "Orbit.registerCommand('AgentTool', function() end, 'Test', '', true)\n");
    Check(plugins.PollHotReload() == 1);
    statuses = plugins.Statuses();
    Check(!statuses[0].loaded);
    Check(!statuses[0].error.empty());
    Check(commandRegistry.Catalog().empty());

    // Once the project explicitly grants MCP registration, the same
    // package may expose an automation-visible command.
    project.Manifest().plugins[0].grantedPermissions.push_back("mcp_registration");
    project.Save();

    Write(
        package / "main.luau",
        "Orbit.registerCommand('AgentTool', function() end, 'Test', '', true)\n");

    plugins.LoadEnabled(project.Manifest());

    statuses = plugins.Statuses();
    Check(statuses[0].loaded);
    Check(commandRegistry.Catalog().size() == 1);
    Check(commandRegistry.Catalog()[0].automationVisible);

    // The host intentionally exposes no file/process/network libraries.
    // Do not rely on an external test helper inside Luau: encode the result
    // into the registered command name and inspect it from the host.
    Write(package / "main.luau",
        "if io == nil and os == nil and debug == nil then\n"
        "  Orbit.registerCommand('SafeGlobals', function() end, 'Test', '', false)\n"
        "else\n"
        "  Orbit.registerCommand('UnsafeGlobals', function() end, 'Test', '', false)\n"
        "end\n");
    Check(plugins.PollHotReload() == 1);
    statuses = plugins.Statuses();
    Check(statuses[0].loaded);
    Check(commandRegistry.Catalog().size() == 1);
    Check(commandRegistry.Catalog()[0].name == "SafeGlobals");

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
    Check(statuses.size() == 2);

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
            foundBroken = !status.loaded && !status.error.empty();
        }
    }

    Check(foundHealthy);
    Check(foundBroken);
    Check(commandRegistry.Catalog().size() == 1);
    Check(commandRegistry.Catalog()[0].name == "StillLoaded");

    std::filesystem::remove_all(root);
    return 0;
}
