#pragma once

#include <orbit/content/ContentHash.hpp>
#include <orbit/content/DerivedDataCache.hpp>
#include <orbit/core/Types.hpp>

#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::content
{
struct ImportArtifact
{
    std::string name;
    std::vector<std::byte> bytes;
};

struct ImportRequest
{
    std::filesystem::path sourcePath;
    std::filesystem::path projectRelativePath;
    std::string settings;
    std::string targetPlatform;
};

struct ImportOutput
{
    std::vector<ImportArtifact> artifacts;
    std::vector<std::filesystem::path> dependencies;
};

struct ImporterDescriptor
{
    std::string id;
    u32 version{1};
    std::vector<std::string> extensions;
    std::function<ImportOutput(
        const ImportRequest&)> import;
};

struct ImporterCatalogEntry
{
    std::string id;
    u32 version{0};
    std::vector<std::string> extensions;
};

class ImporterRegistry
{
public:
    void Register(
        ImporterDescriptor descriptor);

    [[nodiscard]] const ImporterDescriptor*
    FindFor(
        const std::filesystem::path& source) const noexcept;

    [[nodiscard]] std::vector<ImporterCatalogEntry>
    Catalog() const;

private:
    std::vector<ImporterDescriptor>
        importers_;
};

// Registers Orbit-owned source importers that already have a real derived
// representation. Additional texture/mesh codecs land in later milestones
// without changing the registry contract.
void RegisterBuiltinImporters(
    ImporterRegistry& registry);

struct ImportedArtifact
{
    std::string name;
    std::filesystem::path path;
};

struct ImportResult
{
    ContentHash key;
    ContentHash sourceHash;
    std::string importerId;
    u32 importerVersion{0};
    bool cacheHit{false};
    std::vector<ImportedArtifact> artifacts;
    std::vector<std::filesystem::path> dependencies;
};

class AssetPipeline
{
public:
    AssetPipeline(
        const ImporterRegistry& importers,
        DerivedDataCache& cache);

    [[nodiscard]] ImportResult Import(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& source,
        std::string settings = {},
        std::string targetPlatform = "source");

private:
    const ImporterRegistry& importers_;
    DerivedDataCache& cache_;
};

[[nodiscard]] ContentHash BuildDerivedDataKey(
    const ContentHash& sourceHash,
    std::string_view importerId,
    u32 importerVersion,
    std::string_view settings,
    std::string_view targetPlatform);
} // namespace orbit::content
