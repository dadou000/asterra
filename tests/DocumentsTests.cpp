#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>

#include <SQLiteCpp/SQLiteCpp.h>

#include <cassert>
#include <filesystem>
#include <stdexcept>

int main()
{
    const auto unique =
        orbit::documents::ProjectId::Random().
            ToString();

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbit-documents-" + unique);

    std::filesystem::remove_all(root);

    {
        auto project =
            orbit::documents::
                ProjectDocument::Create(
                    root,
                    "Document Test");

        assert(
            project.Manifest().
                projectId.IsValid());
        assert(
            project.Manifest().
                displayName ==
            "Document Test");
        assert(
            std::filesystem::exists(
                project.StartupWorldPath()));

        const auto initialWorlds =
            project.WorldPaths();
        assert(initialWorlds.size() == 1);
        assert(
            initialWorlds.front() ==
            std::filesystem::path(
                "Worlds/Main.orbitworld"));

        const auto secondWorld =
            project.CreateWorld(
                "Secondary",
                "Secondary World");
        assert(
            secondWorld ==
            std::filesystem::path(
                "Worlds/Secondary.orbitworld"));
        assert(
            std::filesystem::exists(
                root / secondWorld));

        {
            orbit::documents::WorldDatabase secondary(
                root / secondWorld);
            assert(
                secondary.GetMetadata(
                    "display_name") ==
                std::optional<std::string>(
                    "Secondary World"));
        }

        const auto worlds =
            project.WorldPaths();
        assert(worlds.size() == 2);
        assert(
            worlds[0] ==
            std::filesystem::path(
                "Worlds/Main.orbitworld"));
        assert(
            worlds[1] ==
            std::filesystem::path(
                "Worlds/Secondary.orbitworld"));

        bool overwriteRejected = false;
        try
        {
            static_cast<void>(
                project.CreateWorld(
                    secondWorld,
                    "Duplicate"));
        }
        catch (const std::runtime_error&)
        {
            overwriteRejected = true;
        }
        assert(overwriteRejected);

        bool escapeRejected = false;
        try
        {
            static_cast<void>(
                project.CreateWorld(
                    "../Outside.orbitworld",
                    "Outside"));
        }
        catch (const std::invalid_argument&)
        {
            escapeRejected = true;
        }
        assert(escapeRejected);

        project.SetStartupWorld(secondWorld);
        assert(
            project.Manifest().startupWorld ==
            secondWorld);
        assert(
            project.StartupWorldPath() ==
            root / secondWorld);

        project.Manifest().displayName =
            "Renamed Project";
        project.Manifest().
            plugins.push_back({
                .id = "example.plugin",
                .version = "1.2.3",
                .grantedPermissions = {
                    "project_mutation",
                    "mcp_registration"
                }
            });
        project.Save();

        const auto reopened =
            orbit::documents::
                ProjectDocument::Open(
                    project.ManifestPath());

        assert(
            reopened.Manifest().
                projectId ==
            project.Manifest().
                projectId);
        assert(
            reopened.Manifest().
                displayName ==
            "Renamed Project");
        assert(
            reopened.Manifest().startupWorld ==
            secondWorld);
        assert(
            reopened.Manifest().
                plugins.size() == 1);
        assert(
            reopened.Manifest().
                plugins[0].
                grantedPermissions.size() == 2);
        assert(
            reopened.Manifest().
                plugins[0].
                grantedPermissions[0] ==
            "project_mutation");
    }

    const std::filesystem::path
        worldPath =
            root /
            "Worlds/Main.orbitworld";

    {
        orbit::documents::WorldDatabase
            world(worldPath);

        assert(
            world.SchemaVersion() ==
            orbit::documents::
                kCurrentWorldSchemaVersion);
        assert(world.Id().IsValid());

        world.SetMetadata(
            "transaction_test",
            "before");

        bool threw = false;

        try
        {
            world.RunTransaction(
                [](auto& transactionWorld)
                {
                    transactionWorld.
                        SetMetadata(
                            "transaction_test",
                            "inside");
                    throw std::runtime_error(
                        "rollback");
                });
        }
        catch (const std::runtime_error&)
        {
            threw = true;
        }

        assert(threw);
        assert(
            world.GetMetadata(
                "transaction_test") ==
            std::optional<std::string>(
                "before"));
    }

    // Explicit migration fixture: replace the world with a schema-0 DB
    // and verify WorldDatabase upgrades it to the current schema.
    const std::filesystem::path
        migrationPath =
            root /
            "Worlds/Migration.orbitworld";

    std::filesystem::remove(
        migrationPath);

    {
        SQLite::Database legacy(
            migrationPath.string(),
            SQLite::OPEN_READWRITE |
                SQLite::OPEN_CREATE);

        legacy.exec(
            "CREATE TABLE orbit_schema ("
            "version INTEGER NOT NULL"
            ");");
        legacy.exec(
            "INSERT INTO orbit_schema(version) "
            "VALUES (0);");
    }

    {
        orbit::documents::WorldDatabase
            migrated(migrationPath);

        assert(
            migrated.SchemaVersion() ==
            orbit::documents::
                kCurrentWorldSchemaVersion);
        assert(migrated.Id().IsValid());
        assert(
            migrated.GetMetadata(
                "world_id").
                has_value());
    }

    std::filesystem::remove_all(root);
    return 0;
}
