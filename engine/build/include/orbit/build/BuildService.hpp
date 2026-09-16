#pragma once

#include <orbit/content/ContentHash.hpp>
#include <orbit/content/ContentService.hpp>
#include <orbit/core/Types.hpp>
#include <orbit/documents/ProjectManifest.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace orbit::build
{
enum class IssueSeverity : u8
{
    Warning,
    Error
};

struct BuildIssue
{
    IssueSeverity severity{IssueSeverity::Error};
    std::string code;
    std::string message;
    std::filesystem::path path;
};

struct BuildRequest
{
    std::filesystem::path manifestPath;
    std::string profileName;
    std::filesystem::path outputDirectory;
    bool cleanOutput{true};
};

struct BuildValidation
{
    documents::BuildProfile profile;
    std::filesystem::path projectRoot;
    std::filesystem::path outputDirectory;
    std::vector<BuildIssue> issues;

    [[nodiscard]] bool Succeeded() const noexcept;
};

struct CookedAsset
{
    content::AssetId id{};
    content::AssetKind kind{content::AssetKind::Unknown};
    std::filesystem::path sourcePath;
    content::ContentHash sourceHash{};
    content::ContentHash derivedKey{};
    std::vector<std::filesystem::path> artifacts;
    bool cacheHit{false};
};

struct CookedScript
{
    std::filesystem::path sourcePath;
    content::ContentHash sourceHash{};
    content::ContentHash bytecodeHash{};
    std::filesystem::path bytecodePath;
};

struct BuildManifest
{
    documents::ProjectId projectId{};
    std::string projectName;
    std::string engineCompatibilityVersion;
    documents::BuildProfile profile;
    std::filesystem::path startupWorld;
    std::vector<std::filesystem::path>
        scriptEntryPoints;
    std::vector<CookedAsset> assets;
    std::vector<CookedScript> scripts;
};

struct BuildResult
{
    BuildManifest manifest;
    std::filesystem::path outputDirectory;
    std::filesystem::path manifestPath;
    std::vector<BuildIssue> issues;

    [[nodiscard]] bool Succeeded() const noexcept;
};

class BuildService
{
public:
    [[nodiscard]] BuildValidation Validate(
        const BuildRequest& request) const;

    // Produces deterministic cooked project products. Runtime executable
    // assembly/package signing are separate stages and are not implied by
    // this operation.
    [[nodiscard]] BuildResult Cook(
        const BuildRequest& request) const;
};
} // namespace orbit::build
