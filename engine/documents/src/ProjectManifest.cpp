#include <orbit/documents/ProjectManifest.hpp>

#include <toml++/toml.hpp>

#include <fstream>
#include <stdexcept>
#include <system_error>

#if defined(_WIN32)
#define NOMINMAX
#include <Windows.h>
#endif

namespace orbit::documents
{
namespace
{
[[nodiscard]] std::string RequiredString(
    const toml::table& table,
    const std::string_view key)
{
    const auto value =
        table[key].value<std::string>();

    if (!value.has_value())
    {
        throw std::runtime_error(
            "Project manifest missing string field: " +
            std::string(key));
    }

    return *value;
}

[[nodiscard]] i64 RequiredInteger(
    const toml::table& table,
    const std::string_view key)
{
    const auto value =
        table[key].value<i64>();

    if (!value.has_value())
    {
        throw std::runtime_error(
            "Project manifest missing integer field: " +
            std::string(key));
    }

    return *value;
}

void ReplaceFile(
    const std::filesystem::path& temporary,
    const std::filesystem::path& destination)
{
#if defined(_WIN32)
    if (!MoveFileExW(
            temporary.c_str(),
            destination.c_str(),
            MOVEFILE_REPLACE_EXISTING |
                MOVEFILE_WRITE_THROUGH))
    {
        throw std::system_error(
            static_cast<int>(
                GetLastError()),
            std::system_category(),
            "Failed to atomically replace project manifest");
    }
#else
    std::error_code error;
    std::filesystem::rename(
        temporary,
        destination,
        error);

    if (error)
    {
        throw std::system_error(error);
    }
#endif
}
} // namespace

ProjectManifest LoadProjectManifest(
    const std::filesystem::path& path)
{
    const toml::table root =
        toml::parse_file(
            path.string());

    const toml::table* project =
        root["project"].as_table();

    if (project == nullptr)
    {
        throw std::runtime_error(
            "Project manifest is missing [project].");
    }

    ProjectManifest manifest{};

    const i64 schemaVersion =
        RequiredInteger(
            *project,
            "schema_version");

    if (schemaVersion <= 0 ||
        schemaVersion >
            static_cast<i64>(
                kProjectManifestSchemaVersion))
    {
        throw std::runtime_error(
            "Unsupported project manifest schema version.");
    }

    manifest.schemaVersion =
        static_cast<u32>(
            schemaVersion);

    const std::string idText =
        RequiredString(
            *project,
            "id");

    const auto projectId =
        ProjectId::Parse(idText);

    if (!projectId.has_value())
    {
        throw std::runtime_error(
            "Project manifest contains an invalid project ID.");
    }

    manifest.projectId = *projectId;
    manifest.displayName =
        RequiredString(
            *project,
            "name");
    manifest.engineCompatibilityVersion =
        RequiredString(
            *project,
            "engine_compatibility");
    manifest.startupWorld =
        std::filesystem::path(
            RequiredString(
                *project,
                "startup_world"));

    manifest.plugins.clear();

    if (const toml::array* plugins =
            root["plugins"].as_array())
    {
        for (const toml::node& node :
             *plugins)
        {
            const toml::table* item =
                node.as_table();

            if (item == nullptr)
            {
                throw std::runtime_error(
                    "Each plugins entry must be a table.");
            }

            PluginRequirement requirement{
                .id =
                    RequiredString(
                        *item,
                        "id"),
                .version =
                    RequiredString(
                        *item,
                        "version")
            };

            if (const toml::array* permissions =
                    (*item)["granted_permissions"].
                        as_array())
            {
                for (const toml::node& permission :
                     *permissions)
                {
                    const auto value =
                        permission.
                            value<std::string>();

                    if (!value.has_value())
                    {
                        throw std::runtime_error(
                            "Plugin granted_permissions entries must be strings.");
                    }

                    requirement.
                        grantedPermissions.
                        push_back(*value);
                }
            }

            manifest.plugins.push_back(
                std::move(requirement));
        }
    }

    manifest.scriptEntryPoints.clear();

    if (const toml::array* scripts =
            root["script_entry_points"].
                as_array())
    {
        for (const toml::node& node :
             *scripts)
        {
            const auto value =
                node.value<std::string>();

            if (!value.has_value())
            {
                throw std::runtime_error(
                    "script_entry_points entries must be strings.");
            }

            manifest.scriptEntryPoints.
                emplace_back(*value);
        }
    }

    manifest.buildProfiles.clear();

    if (const toml::array* profiles =
            root["build_profiles"].
                as_array())
    {
        for (const toml::node& node :
             *profiles)
        {
            const toml::table* item =
                node.as_table();

            if (item == nullptr)
            {
                throw std::runtime_error(
                    "Each build_profiles entry must be a table.");
            }

            manifest.buildProfiles.push_back({
                .name =
                    RequiredString(
                        *item,
                        "name"),
                .configuration =
                    RequiredString(
                        *item,
                        "configuration"),
                .platform =
                    RequiredString(
                        *item,
                        "platform"),
                .storefront =
                    RequiredString(
                        *item,
                        "storefront")
            });
        }
    }

    manifest.assetMountPoints.clear();

    if (const toml::array* mounts =
            root["asset_mount_points"].
                as_array())
    {
        for (const toml::node& node :
             *mounts)
        {
            const auto value =
                node.value<std::string>();

            if (!value.has_value())
            {
                throw std::runtime_error(
                    "asset_mount_points entries must be strings.");
            }

            manifest.assetMountPoints.
                emplace_back(*value);
        }
    }

    return manifest;
}

void SaveProjectManifestAtomic(
    const std::filesystem::path& path,
    const ProjectManifest& manifest)
{
    if (!manifest.projectId)
    {
        throw std::invalid_argument(
            "Cannot save project manifest with invalid project ID.");
    }

    if (manifest.displayName.empty())
    {
        throw std::invalid_argument(
            "Cannot save project manifest with empty project name.");
    }

    std::filesystem::create_directories(
        path.parent_path());

    toml::table project;
    project.insert(
        "schema_version",
        static_cast<i64>(
            manifest.schemaVersion));
    project.insert(
        "id",
        manifest.projectId.ToString());
    project.insert(
        "name",
        manifest.displayName);
    project.insert(
        "engine_compatibility",
        manifest.
            engineCompatibilityVersion);
    project.insert(
        "startup_world",
        manifest.startupWorld.
            generic_string());

    toml::table root;
    root.insert(
        "project",
        std::move(project));

    toml::array plugins;
    for (const PluginRequirement& plugin :
         manifest.plugins)
    {
        toml::table item;
        item.insert("id", plugin.id);
        item.insert(
            "version",
            plugin.version);

        toml::array grantedPermissions;
        for (const std::string& permission :
             plugin.grantedPermissions)
        {
            grantedPermissions.push_back(
                permission);
        }
        item.insert(
            "granted_permissions",
            std::move(grantedPermissions));

        plugins.push_back(
            std::move(item));
    }
    root.insert(
        "plugins",
        std::move(plugins));

    toml::array scripts;
    for (const auto& script :
         manifest.scriptEntryPoints)
    {
        scripts.push_back(
            script.generic_string());
    }
    root.insert(
        "script_entry_points",
        std::move(scripts));

    toml::array profiles;
    for (const BuildProfile& profile :
         manifest.buildProfiles)
    {
        toml::table item;
        item.insert(
            "name",
            profile.name);
        item.insert(
            "configuration",
            profile.configuration);
        item.insert(
            "platform",
            profile.platform);
        item.insert(
            "storefront",
            profile.storefront);
        profiles.push_back(
            std::move(item));
    }
    root.insert(
        "build_profiles",
        std::move(profiles));

    toml::array mounts;
    for (const auto& mount :
         manifest.assetMountPoints)
    {
        mounts.push_back(
            mount.generic_string());
    }
    root.insert(
        "asset_mount_points",
        std::move(mounts));

    std::filesystem::path temporary =
        path;
    temporary += ".tmp";

    {
        std::ofstream output(
            temporary,
            std::ios::binary |
                std::ios::trunc);

        if (!output)
        {
            throw std::runtime_error(
                "Failed to create temporary project manifest.");
        }

        output << root;
        output.flush();

        if (!output)
        {
            throw std::runtime_error(
                "Failed while writing project manifest.");
        }
    }

    ReplaceFile(
        temporary,
        path);
}
} // namespace orbit::documents
