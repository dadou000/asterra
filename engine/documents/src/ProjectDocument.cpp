#include <orbit/documents/ProjectDocument.hpp>

#include <orbit/documents/WorldDatabase.hpp>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace orbit::documents
{
namespace
{
[[nodiscard]] std::filesystem::path NormalizeWorldPath(
    const std::filesystem::path& input)
{
    if (input.empty())
    {
        throw std::invalid_argument(
            "World path must not be empty.");
    }

    if (input.is_absolute())
    {
        throw std::invalid_argument(
            "World path must be project-relative.");
    }

    for (const auto& part : input)
    {
        if (part == "..")
        {
            throw std::invalid_argument(
                "World path must stay inside the project Worlds directory.");
        }
    }

    std::filesystem::path relative =
        input.lexically_normal();

    if (relative.empty() ||
        relative == ".")
    {
        throw std::invalid_argument(
            "World path must stay inside the project Worlds directory.");
    }

    if (relative.begin() == relative.end() ||
        *relative.begin() != "Worlds")
    {
        relative =
            std::filesystem::path("Worlds") /
            relative;
    }

    relative = relative.lexically_normal();

    if (relative.begin() == relative.end() ||
        *relative.begin() != "Worlds")
    {
        throw std::invalid_argument(
            "World path must stay inside the project Worlds directory.");
    }

    if (relative.extension() != ".orbitworld")
    {
        relative.replace_extension(
            ".orbitworld");
    }

    return relative;
}
} // namespace

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

std::vector<std::filesystem::path>
ProjectDocument::WorldPaths() const
{
    std::vector<std::filesystem::path> worlds;
    const auto directory =
        rootDirectory_ /
        "Worlds";

    if (!std::filesystem::exists(directory))
    {
        return worlds;
    }

    for (const auto& entry :
         std::filesystem::directory_iterator(directory))
    {
        if (!entry.is_regular_file() ||
            entry.path().extension() !=
                ".orbitworld")
        {
            continue;
        }

        worlds.push_back(
            std::filesystem::relative(
                entry.path(),
                rootDirectory_));
    }

    std::sort(
        worlds.begin(),
        worlds.end());
    return worlds;
}

std::filesystem::path ProjectDocument::CreateWorld(
    const std::filesystem::path& relativePath,
    const std::string_view displayName)
{
    if (displayName.empty())
    {
        throw std::invalid_argument(
            "World display name must not be empty.");
    }

    const auto normalized =
        NormalizeWorldPath(relativePath);
    const auto absolute =
        rootDirectory_ /
        normalized;

    if (std::filesystem::exists(absolute))
    {
        throw std::runtime_error(
            "World document already exists.");
    }

    std::filesystem::create_directories(
        absolute.parent_path());

    WorldDatabase world(absolute);
    world.SetMetadata(
        "display_name",
        std::string(displayName));
    world.Checkpoint();

    return normalized;
}

void ProjectDocument::SetStartupWorld(
    const std::filesystem::path& relativePath)
{
    const auto normalized =
        NormalizeWorldPath(relativePath);
    const auto absolute =
        rootDirectory_ /
        normalized;

    if (!std::filesystem::is_regular_file(absolute))
    {
        throw std::runtime_error(
            "Startup world must reference an existing project world document.");
    }

    WorldDatabase world(absolute);
    static_cast<void>(world.Id());

    manifest_.startupWorld =
        normalized;
    Save();
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
