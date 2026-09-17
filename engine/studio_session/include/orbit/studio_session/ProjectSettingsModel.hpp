#pragma once

#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/editor_session/WorldDocumentsModel.hpp>
#include <orbit/studio_session/StudioWorkspace.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::studio_session
{
struct ProjectSettingsSnapshot
{
    documents::ProjectId projectId{};
    std::string displayName;
    std::filesystem::path rootDirectory;
    std::filesystem::path manifestPath;
    std::string engineCompatibilityVersion;
    std::filesystem::path startupWorld;
    std::vector<editor_session::WorldDocumentItem> worlds;
};

// Project-settings presentation model. ProjectDocument remains authority;
// setters here are only application-facing adapters over validated persistent
// document/session operations. World entries come from the diagnostic Studio
// catalog so one invalid world document cannot make settings unavailable.
class ProjectSettingsModel
{
public:
    explicit ProjectSettingsModel(
        StudioWorkspace& workspace) noexcept;

    [[nodiscard]] ProjectSettingsSnapshot Snapshot() const;

    void SetDisplayName(
        std::string_view displayName);

    [[nodiscard]] documents::WorldDescriptor SetStartupWorld(
        const std::filesystem::path& relativePath);

private:
    StudioWorkspace& workspace_;
};
} // namespace orbit::studio_session
