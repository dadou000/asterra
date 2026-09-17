#pragma once

#include <orbit/core/Types.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::plugins
{
inline constexpr std::string_view OrbitApiVersion = "0.0.3";

enum class PluginScope : u8
{
    Editor,
    Runtime,
    Both
};

enum class PluginPermission : u32
{
    FilesystemExternal = 1U << 0U,
    Network = 1U << 1U,
    ProcessExecution = 1U << 2U,
    ProjectMutation = 1U << 3U,
    BuildHooks = 1U << 4U,
    McpRegistration = 1U << 5U
};

struct PluginPermissionSet
{
    u32 bits{0};

    [[nodiscard]] constexpr bool Contains(
        const PluginPermission permission) const noexcept
    {
        return (
            bits &
            static_cast<u32>(permission)) != 0U;
    }

    constexpr void Add(
        const PluginPermission permission) noexcept
    {
        bits |=
            static_cast<u32>(permission);
    }

    [[nodiscard]] constexpr PluginPermissionSet
    Intersection(
        const PluginPermissionSet other) const noexcept
    {
        return {
            .bits = bits & other.bits
        };
    }

    [[nodiscard]] constexpr bool operator==(
        const PluginPermissionSet&) const noexcept = default;
};

struct PluginDependency
{
    std::string id;
    std::string version;
};

struct PluginManifest
{
    std::string id;
    std::string version;
    std::string orbitApiVersion;
    std::filesystem::path entryScript;
    PluginScope scope{PluginScope::Editor};
    PluginPermissionSet requestedPermissions{};
    std::vector<PluginDependency> dependencies;
};

[[nodiscard]] std::optional<PluginPermission>
PermissionFromString(
    std::string_view value) noexcept;

[[nodiscard]] std::string_view PermissionName(
    PluginPermission permission) noexcept;

[[nodiscard]] PluginManifest LoadPluginManifest(
    const std::filesystem::path& path);
} // namespace orbit::plugins
