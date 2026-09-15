#pragma once

#include <orbit/core/StrongId.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace orbit::documents
{
struct ProjectIdTag;
using ProjectId = core::StrongId<ProjectIdTag>;

inline constexpr u32 kProjectManifestSchemaVersion = 1;

struct PluginRequirement
{
    std::string id;
    std::string version;
};

struct BuildProfile
{
    std::string name;
    std::string configuration{"Development"};
    std::string platform{"Windows"};
    std::string storefront{"standalone"};
};

struct ProjectManifest
{
    u32 schemaVersion{
        kProjectManifestSchemaVersion};
    ProjectId projectId{};
    std::string displayName;
    std::string engineCompatibilityVersion{
        "0.0.3"};
    std::filesystem::path startupWorld{
        "Worlds/Main.orbitworld"};
    std::vector<PluginRequirement> plugins;
    std::vector<std::filesystem::path>
        scriptEntryPoints;
    std::vector<BuildProfile> buildProfiles;
    std::vector<std::filesystem::path>
        assetMountPoints{
            "Content"};
};

[[nodiscard]] ProjectManifest LoadProjectManifest(
    const std::filesystem::path& path);

void SaveProjectManifestAtomic(
    const std::filesystem::path& path,
    const ProjectManifest& manifest);
} // namespace orbit::documents
