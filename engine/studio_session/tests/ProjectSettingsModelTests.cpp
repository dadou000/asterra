#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/studio_session/ProjectSettingsModel.hpp>
#include <orbit/studio_session/StudioWorkspace.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
#include <stdexcept>

namespace
{
void Check(
    const bool condition,
    const std::source_location location =
        std::source_location::current())
{
    if (!condition)
    {
        std::cerr
            << "Project settings model test failed at "
            << location.file_name()
            << ':'
            << location.line()
            << '\n';
        std::exit(1);
    }
}
} // namespace

int main()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("orbit-project-settings-" +
         orbit::documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    orbit::documents::ProjectId stableProjectId{};

    {
        orbit::studio_session::StudioWorkspace workspace;
        workspace.CreateProject(root, "Initial Name");
        orbit::studio_session::ProjectSettingsModel settings(workspace);

        auto snapshot = settings.Snapshot();
        stableProjectId = snapshot.projectId;

        Check(snapshot.displayName == "Initial Name");
        Check(snapshot.rootDirectory == std::filesystem::absolute(root));
        Check(snapshot.manifestPath == std::filesystem::absolute(root / "Project.orbit.toml"));
        Check(snapshot.engineCompatibilityVersion == "0.0.3");
        Check(snapshot.startupWorld == std::filesystem::path("Worlds/Main.orbitworld"));
        Check(snapshot.worlds.size() == 1U);
        Check(snapshot.worlds[0].valid);
        Check(snapshot.worlds[0].descriptor.startup);

        settings.SetDisplayName("Renamed Project");
        snapshot = settings.Snapshot();
        Check(snapshot.displayName == "Renamed Project");
        Check(snapshot.projectId == stableProjectId);

        bool emptyRejected = false;
        try
        {
            settings.SetDisplayName("");
        }
        catch (const std::invalid_argument&)
        {
            emptyRejected = true;
        }
        Check(emptyRejected);
        Check(settings.Snapshot().displayName == "Renamed Project");

        const auto secondary =
            workspace.Session().CreateWorld(
                "Secondary",
                "Secondary World");
        Check(!secondary.startup);

        const auto startup =
            settings.SetStartupWorld("Secondary");
        Check(startup.startup);
        Check(startup.displayName == "Secondary World");
        Check(
            settings.Snapshot().startupWorld ==
            std::filesystem::path("Worlds/Secondary.orbitworld"));

        bool missingRejected = false;
        try
        {
            static_cast<void>(
                settings.SetStartupWorld("Missing"));
        }
        catch (const std::runtime_error&)
        {
            missingRejected = true;
        }
        Check(missingRejected);
        Check(
            settings.Snapshot().startupWorld ==
            std::filesystem::path("Worlds/Secondary.orbitworld"));

        workspace.CloseProject();
    }

    {
        orbit::studio_session::StudioWorkspace workspace;
        workspace.OpenProject(root);
        orbit::studio_session::ProjectSettingsModel settings(workspace);
        auto snapshot = settings.Snapshot();

        Check(snapshot.projectId == stableProjectId);
        Check(snapshot.displayName == "Renamed Project");
        Check(
            snapshot.startupWorld ==
            std::filesystem::path("Worlds/Secondary.orbitworld"));
        Check(snapshot.worlds.size() == 2U);

        std::size_t startupCount = 0U;
        for (const auto& world : snapshot.worlds)
        {
            Check(world.valid);

            if (world.descriptor.startup)
            {
                ++startupCount;
                Check(
                    world.descriptor.relativePath ==
                    std::filesystem::path("Worlds/Secondary.orbitworld"));
            }
        }
        Check(startupCount == 1U);

        const auto brokenPath = root / "Worlds/Broken.orbitworld";
        {
            std::ofstream broken(
                brokenPath,
                std::ios::binary | std::ios::trunc);
            broken << "invalid world";
        }

        snapshot = settings.Snapshot();
        Check(snapshot.worlds.size() == 3U);

        bool foundBroken = false;
        for (const auto& world : snapshot.worlds)
        {
            if (world.descriptor.relativePath ==
                std::filesystem::path("Worlds/Broken.orbitworld"))
            {
                Check(!world.valid);
                Check(!world.diagnostic.empty());
                foundBroken = true;
            }
        }
        Check(foundBroken);
    }

    std::filesystem::remove_all(root);
    return 0;
}
