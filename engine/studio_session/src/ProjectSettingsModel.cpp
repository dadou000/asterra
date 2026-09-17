#include <orbit/studio_session/ProjectSettingsModel.hpp>

namespace orbit::studio_session
{
ProjectSettingsModel::ProjectSettingsModel(
    StudioWorkspace& workspace) noexcept
    : workspace_(workspace)
{
}

ProjectSettingsSnapshot ProjectSettingsModel::Snapshot() const
{
    const auto& project = workspace_.Project();
    const auto& manifest = project.Manifest();

    return {
        .projectId = manifest.projectId,
        .displayName = manifest.displayName,
        .rootDirectory = project.RootDirectory(),
        .manifestPath = project.ManifestPath(),
        .engineCompatibilityVersion =
            manifest.engineCompatibilityVersion,
        .startupWorld = manifest.startupWorld,
        .worlds = project.Worlds()
    };
}

void ProjectSettingsModel::SetDisplayName(
    const std::string_view displayName)
{
    workspace_.Project().SetDisplayName(displayName);
}

documents::WorldDescriptor
ProjectSettingsModel::SetStartupWorld(
    const std::filesystem::path& relativePath)
{
    return workspace_.Session().SetStartupWorld(relativePath);
}
} // namespace orbit::studio_session
