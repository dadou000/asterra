#pragma once

#include <orbit/documents/ProjectManifest.hpp>

#include <filesystem>
#include <string_view>

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
