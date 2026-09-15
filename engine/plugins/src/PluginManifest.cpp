#include <orbit/plugins/PluginManifest.hpp>

#include <toml++/toml.hpp>

#include <stdexcept>

namespace orbit::plugins
{
namespace
{
[[nodiscard]] std::string RequiredString(
    const toml::table& table,
    const std::string_view key)
{
    const auto value =
        table[key].value<std::string>();

    if (!value.has_value() ||
        value->empty())
    {
        throw std::runtime_error(
            "Plugin manifest missing string field: " +
            std::string(key));
    }

    return *value;
}
} // namespace

std::optional<PluginPermission>
PermissionFromString(
    const std::string_view value) noexcept
{
    if (value == "filesystem_external")
    {
        return PluginPermission::
            FilesystemExternal;
    }

    if (value == "network")
    {
        return PluginPermission::Network;
    }

    if (value == "process_execution")
    {
        return PluginPermission::
            ProcessExecution;
    }

    if (value == "project_mutation")
    {
        return PluginPermission::
            ProjectMutation;
    }

    if (value == "build_hooks")
    {
        return PluginPermission::
            BuildHooks;
    }

    if (value == "mcp_registration")
    {
        return PluginPermission::
            McpRegistration;
    }

    return std::nullopt;
}

std::string_view PermissionName(
    const PluginPermission permission) noexcept
{
    switch (permission)
    {
    case PluginPermission::FilesystemExternal:
        return "filesystem_external";
    case PluginPermission::Network:
        return "network";
    case PluginPermission::ProcessExecution:
        return "process_execution";
    case PluginPermission::ProjectMutation:
        return "project_mutation";
    case PluginPermission::BuildHooks:
        return "build_hooks";
    case PluginPermission::McpRegistration:
        return "mcp_registration";
    }

    return "unknown";
}

PluginManifest LoadPluginManifest(
    const std::filesystem::path& path)
{
    const toml::table document =
        toml::parse_file(
            path.string());

    const toml::table* plugin =
        document["plugin"].as_table();

    if (plugin == nullptr)
    {
        throw std::runtime_error(
            "Plugin manifest is missing [plugin].");
    }

    PluginManifest result{};
    result.id =
        RequiredString(
            *plugin,
            "id");
    result.version =
        RequiredString(
            *plugin,
            "version");
    result.orbitApiVersion =
        RequiredString(
            *plugin,
            "orbit_api");
    result.entryScript =
        std::filesystem::path(
            RequiredString(
                *plugin,
                "entry"));

    const std::string scope =
        RequiredString(
            *plugin,
            "scope");

    if (scope == "editor")
    {
        result.scope =
            PluginScope::Editor;
    }
    else if (scope == "runtime")
    {
        result.scope =
            PluginScope::Runtime;
    }
    else if (scope == "both")
    {
        result.scope =
            PluginScope::Both;
    }
    else
    {
        throw std::runtime_error(
            "Plugin manifest has invalid scope.");
    }

    if (const toml::array* permissions =
            (*plugin)["permissions"].
                as_array())
    {
        for (const toml::node& node :
             *permissions)
        {
            const auto value =
                node.value<std::string>();

            if (!value.has_value())
            {
                throw std::runtime_error(
                    "Plugin permissions must be strings.");
            }

            const auto permission =
                PermissionFromString(
                    *value);

            if (!permission.has_value())
            {
                throw std::runtime_error(
                    "Unknown plugin permission: " +
                    *value);
            }

            result.requestedPermissions.
                Add(*permission);
        }
    }

    if (const toml::array* dependencies =
            document["dependencies"].
                as_array())
    {
        for (const toml::node& node :
             *dependencies)
        {
            const toml::table* dependency =
                node.as_table();

            if (dependency == nullptr)
            {
                throw std::runtime_error(
                    "Plugin dependencies must be tables.");
            }

            result.dependencies.push_back({
                .id =
                    RequiredString(
                        *dependency,
                        "id"),
                .version =
                    RequiredString(
                        *dependency,
                        "version")
            });
        }
    }

    if (result.entryScript.is_absolute())
    {
        throw std::runtime_error(
            "Plugin entry script must be project-package relative.");
    }

    return result;
}
} // namespace orbit::plugins
