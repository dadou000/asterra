#pragma once

#include <orbit/documents/ProjectManifest.hpp>

#include <filesystem>
#include <string_view>
#include <vector>

namespace orbit::documents
{
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
    // lexical order. Derived data and runtime saves are intentionally excluded.
    [[nodiscard]] std::vector<std::filesystem::path>
    WorldPaths() const;

    // Creates another authoritative .orbitworld document inside Worlds/. The
    // caller may pass either "City.orbitworld" or "Worlds/City.orbitworld".
    // Existing documents are never overwritten.
    [[nodiscard]] std::filesystem::path CreateWorld(
        const std::filesystem::path& relativePath,
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
