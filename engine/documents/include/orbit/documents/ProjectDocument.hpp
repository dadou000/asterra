#pragma once

#include <orbit/documents/ProjectManifest.hpp>
#include <orbit/documents/WorldDatabase.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::documents
{
struct WorldDescriptor
{
    WorldId id{};
    std::filesystem::path relativePath;
    std::string displayName;
    i32 schemaVersion{0};
    bool startup{false};
};

class ProjectDocument
{
public:
    [[nodiscard]] static ProjectDocument Create(
        const std::filesystem::path& rootDirectory,
        std::string_view displayName);

    [[nodiscard]] static ProjectDocument Open(
        const std::filesystem::path& manifestPath);

    [[nodiscard]] const std::filesystem::path&
    RootDirectory() const noexcept;

    [[nodiscard]] const std::filesystem::path&
    ManifestPath() const noexcept;

    [[nodiscard]] std::filesystem::path
    StartupWorldPath() const;

    // Returns project-relative authoritative world document paths in a stable
    // lexical order. Derived data, runtime saves and symlink targets are
    // intentionally excluded. Nested Worlds/ subdirectories are supported.
    [[nodiscard]] std::vector<std::filesystem::path>
    WorldPaths() const;

    // Returns the authoritative project-owned world catalog. Each descriptor
    // comes from the world document itself rather than a parallel editor cache.
    [[nodiscard]] std::vector<WorldDescriptor>
    Worlds() const;

    // Opens and validates one project-owned world and returns its persistent
    // identity, display metadata, schema version and startup state.
    [[nodiscard]] WorldDescriptor DescribeWorld(
        const std::filesystem::path& relativePath) const;

    // Creates another authoritative .orbitworld document inside Worlds/. The
    // caller may pass either "City.orbitworld" or "Worlds/City.orbitworld".
    // Existing documents are never overwritten.
    [[nodiscard]] std::filesystem::path CreateWorld(
        const std::filesystem::path& relativePath,
        std::string_view displayName);

    // Changes persistent display metadata without renaming or replacing the
    // authoritative world document.
    void SetWorldDisplayName(
        const std::filesystem::path& relativePath,
        std::string_view displayName);

    // Changes project display metadata and persists Project.orbit.toml
    // atomically. ProjectId and project directory identity are unchanged.
    void SetDisplayName(
        std::string_view displayName);

    // Changes the project startup world after validating that the target is an
    // existing authoritative world owned by this project. The manifest update
    // is persisted atomically.
    void SetStartupWorld(
        const std::filesystem::path& relativePath);

    [[nodiscard]] const ProjectManifest&
    Manifest() const noexcept;

    [[nodiscard]] ProjectManifest&
    Manifest() noexcept;

    void Save() const;

private:
    std::filesystem::path rootDirectory_;
    std::filesystem::path manifestPath_;
    ProjectManifest manifest_;
};
} // namespace orbit::documents
