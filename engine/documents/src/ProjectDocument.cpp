#include <orbit/documents/ProjectDocument.hpp>

#include <orbit/documents/WorldDatabase.hpp>

#include <stdexcept>
#include <utility>

namespace orbit::documents
{
ProjectDocument ProjectDocument::Create(
    const std::filesystem::path& rootDirectory,
    const std::string_view displayName)
{
    if (displayName.empty())
    {
        throw std::invalid_argument(
            "Project display name must not be empty.");
    }

    std::filesystem::create_directories(
        rootDirectory);

    const std::filesystem::path
        manifestPath =
            rootDirectory /
            "Project.orbit.toml";

    if (std::filesystem::exists(
            manifestPath))
    {
        throw std::runtime_error(
            "Project manifest already exists.");
    }

    const std::filesystem::path directories[] = {
        "Content",
        "Worlds",
        "Scripts",
        "Plugins",
        "Config",
        "Build",
        ".orbit/DerivedData",
        ".orbit/Intermediate",
        ".orbit/Logs"
    };

    for (const auto& directory :
         directories)
    {
        std::filesystem::create_directories(
            rootDirectory /
            directory);
    }

    ProjectManifest manifest{};
    manifest.projectId =
        ProjectId::Random();
    manifest.displayName =
        std::string(displayName);
    manifest.buildProfiles.push_back({
        .name = "Development Windows",
        .configuration = "Development",
        .platform = "Windows",
        .storefront = "standalone"
    });

    SaveProjectManifestAtomic(
        manifestPath,
        manifest);

    const std::filesystem::path
        worldPath =
            rootDirectory /
            manifest.startupWorld;

    WorldDatabase world(worldPath);
    world.SetMetadata(
        "display_name",
        "Main");
    world.Checkpoint();

    ProjectDocument document;
    document.rootDirectory_ =
        std::filesystem::absolute(
            rootDirectory);
    document.manifestPath_ =
        std::filesystem::absolute(
            manifestPath);
    document.manifest_ =
        std::move(manifest);

    return document;
}

ProjectDocument ProjectDocument::Open(
    const std::filesystem::path& manifestPath)
{
    if (!std::filesystem::exists(
            manifestPath))
    {
        throw std::runtime_error(
            "Project manifest does not exist.");
    }

    ProjectDocument document;
    document.manifestPath_ =
        std::filesystem::absolute(
            manifestPath);
    document.rootDirectory_ =
        document.manifestPath_.
            parent_path();
    document.manifest_ =
        LoadProjectManifest(
            document.manifestPath_);

    const std::filesystem::path
        worldPath =
            document.StartupWorldPath();

    if (!std::filesystem::exists(
            worldPath))
    {
        throw std::runtime_error(
            "Project startup world does not exist.");
    }

    // Opening the world validates and applies any pending schema
    // migration before the project is accepted as open.
    WorldDatabase world(worldPath);
    static_cast<void>(world.Id());

    return document;
}

const std::filesystem::path&
ProjectDocument::RootDirectory() const noexcept
{
    return rootDirectory_;
}

const std::filesystem::path&
ProjectDocument::ManifestPath() const noexcept
{
    return manifestPath_;
}

std::filesystem::path
ProjectDocument::StartupWorldPath() const
{
    return rootDirectory_ /
        manifest_.startupWorld;
}

const ProjectManifest&
ProjectDocument::Manifest() const noexcept
{
    return manifest_;
}

ProjectManifest&
ProjectDocument::Manifest() noexcept
{
    return manifest_;
}

void ProjectDocument::Save() const
{
    SaveProjectManifestAtomic(
        manifestPath_,
        manifest_);
}
} // namespace orbit::documents
