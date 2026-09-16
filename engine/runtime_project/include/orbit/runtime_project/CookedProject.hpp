#pragma once

#include <orbit/documents/ProjectManifest.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::runtime_project
{
struct CookedAssetRecord
{
    std::string id;
    std::string kind;
    std::filesystem::path sourcePath;
    std::string sourceSha256;
    std::optional<std::string> derivedKey;
    std::vector<std::filesystem::path> artifacts;
};

struct CookedScriptRecord
{
    std::filesystem::path sourcePath;
    std::string sourceSha256;
    std::string bytecodeSha256;
    std::filesystem::path bytecodePath;
};

struct CookedBuildProfile
{
    std::string name;
    std::string configuration;
    std::string platform;
    std::string storefront;
};

struct CookedProjectManifest
{
    u32 formatVersion{0};
    documents::ProjectId projectId{};
    std::string projectName;
    std::string engineCompatibilityVersion;
    std::filesystem::path startupWorld;
    std::vector<std::filesystem::path>
        scriptEntryPoints;
    CookedBuildProfile build;
    std::vector<CookedAssetRecord> assets;
    std::vector<CookedScriptRecord> scripts;
};

class CookedProject
{
public:
    [[nodiscard]] static CookedProject Open(
        const std::filesystem::path&
            packageOrManifest);

    [[nodiscard]] const std::filesystem::path&
    RootDirectory() const noexcept;

    [[nodiscard]] const std::filesystem::path&
    ManifestPath() const noexcept;

    [[nodiscard]] const CookedProjectManifest&
    Manifest() const noexcept;

    [[nodiscard]] std::filesystem::path
    StartupWorldPath() const;

    [[nodiscard]] std::filesystem::path
    ResolvePackagedFile(
        const std::filesystem::path& relative) const;

    [[nodiscard]] const CookedScriptRecord*
    FindScript(
        const std::filesystem::path& sourcePath) const noexcept;

private:
    std::filesystem::path rootDirectory_;
    std::filesystem::path manifestPath_;
    CookedProjectManifest manifest_;
};

class ScriptRuntime
{
public:
    explicit ScriptRuntime(
        const CookedProject& project);
    ~ScriptRuntime();

    ScriptRuntime(const ScriptRuntime&) = delete;
    ScriptRuntime& operator=(const ScriptRuntime&) = delete;
    ScriptRuntime(ScriptRuntime&&) noexcept;
    ScriptRuntime& operator=(
        ScriptRuntime&&) noexcept;

    // Loads and executes the cooked bytecode corresponding to every
    // project script entry point. Source compilation never occurs here.
    void ExecuteEntryPoints();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbit::runtime_project
