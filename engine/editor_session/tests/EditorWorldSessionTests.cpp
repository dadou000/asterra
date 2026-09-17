#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/editor_session/EditorWorldSession.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <cstdlib>
#include <filesystem>
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
            << "Editor world session test failed at "
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
        ("orbit-editor-session-" +
         orbit::documents::ProjectId::Random().
             ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            orbit::documents::ProjectDocument::Create(
                root,
                "Editor Session Test");
        orbit::editor_session::EditorWorldSession
            session(project);

        Check(session.HasWorld());
        Check(session.Generation() == 1U);
        Check(
            session.ActiveWorld().relativePath ==
            std::filesystem::path(
                "Worlds/Main.orbitworld"));
        Check(session.UniverseStats().systems == 0U);
        Check(session.UniverseStats().bodies == 0U);

        const auto worldObject =
            session.Commands().CreateObject(
                orbit::world_model::kWorldType,
                "Main World");
        const auto systemObject =
            session.Commands().CreateObject(
                orbit::world_model::kCelestialSystemType,
                "Helion",
                worldObject);
        const auto bodyObject =
            session.Commands().CreateObject(
                orbit::world_model::kCelestialBodyType,
                "Asterra",
                systemObject);
        session.Commands().SetProperty(
            bodyObject,
            orbit::world_model::kBodyRadius,
            6'000'000.0);

        Check(session.RefreshUniverseIfChanged());
        Check(session.UniverseStats().systems == 1U);
        Check(session.UniverseStats().bodies == 1U);

        const auto firstRuntimeBody =
            session.Universe().BodyForObject(
                bodyObject);
        Check(firstRuntimeBody.has_value());
        Check(
            session.Universe().Bodies().FindBody(
                *firstRuntimeBody) != nullptr);
        Check(!session.RefreshUniverseIfChanged());

        const auto secondary =
            project.CreateWorld(
                "Secondary",
                "Secondary World");

        const orbit::u64 generationBeforeFailure =
            session.Generation();
        bool missingRejected = false;
        try
        {
            session.OpenWorld(
                "Missing.orbitworld");
        }
        catch (const std::runtime_error&)
        {
            missingRejected = true;
        }
        Check(missingRejected);
        Check(
            session.Generation() ==
            generationBeforeFailure);
        Check(
            session.ActiveWorld().relativePath ==
            std::filesystem::path(
                "Worlds/Main.orbitworld"));
        Check(
            session.Objects().Find(bodyObject).
                has_value());

        session.Commands().BeginTransaction(
            "Block switch");
        bool transactionBlockedSwitch = false;
        try
        {
            session.OpenWorld(secondary);
        }
        catch (const std::logic_error&)
        {
            transactionBlockedSwitch = true;
        }
        Check(transactionBlockedSwitch);
        Check(
            session.Generation() ==
            generationBeforeFailure);
        session.Commands().RollbackTransaction();

        session.OpenWorld(secondary);
        Check(session.Generation() == 2U);
        Check(
            session.ActiveWorld().relativePath ==
            secondary);
        Check(session.Objects().Roots().empty());
        Check(session.Selection().Ordered().empty());
        Check(session.UniverseStats().systems == 0U);
        Check(session.UniverseStats().bodies == 0U);

        const auto secondaryWorldObject =
            session.Commands().CreateObject(
                orbit::world_model::kWorldType,
                "Secondary World");
        const auto secondarySystem =
            session.Commands().CreateObject(
                orbit::world_model::kCelestialSystemType,
                "Secondary Star",
                secondaryWorldObject);
        const auto secondaryBody =
            session.Commands().CreateObject(
                orbit::world_model::kCelestialBodyType,
                "Secondary Planet",
                secondarySystem);
        Check(session.RefreshUniverseIfChanged());
        Check(
            session.Universe().BodyForObject(
                secondaryBody).has_value());

        session.OpenWorld(
            "Worlds/Main.orbitworld");
        Check(session.Generation() == 3U);
        Check(
            session.Objects().Find(bodyObject).
                has_value());
        Check(
            session.UniverseStats().systems == 1U);
        Check(
            session.UniverseStats().bodies == 1U);

        const auto reopenedRuntimeBody =
            session.Universe().BodyForObject(
                bodyObject);
        Check(reopenedRuntimeBody.has_value());
        Check(
            *reopenedRuntimeBody ==
            *firstRuntimeBody);

        project.SetStartupWorld(secondary);
        session.OpenStartupWorld();
        Check(session.Generation() == 4U);
        Check(
            session.ActiveWorld().relativePath ==
            secondary);
        Check(
            session.Objects().Find(
                secondaryBody).has_value());

        session.CloseWorld();
        Check(!session.HasWorld());
        Check(session.Generation() == 5U);

        bool closedAccessRejected = false;
        try
        {
            static_cast<void>(
                session.Objects());
        }
        catch (const std::logic_error&)
        {
            closedAccessRejected = true;
        }
        Check(closedAccessRejected);

        session.OpenStartupWorld();
        Check(session.HasWorld());
        Check(session.Generation() == 6U);
        Check(
            session.ActiveWorld().relativePath ==
            secondary);
        session.Checkpoint();
    }

    std::filesystem::remove_all(root);
    return 0;
}
