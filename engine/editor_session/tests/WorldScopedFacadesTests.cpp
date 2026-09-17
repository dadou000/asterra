#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/editor_session/EditorWorldSession.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace
{
void Check(const bool condition)
{
    if (!condition)
    {
        std::cerr << "World-scoped editor facade test failed.\n";
        std::exit(1);
    }
}
} // namespace

int main()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("orbit-world-facades-" +
         orbit::documents::ProjectId::Random().ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            orbit::documents::ProjectDocument::Create(
                root,
                "World Facades Test");
        orbit::editor_session::EditorWorldSession session(project);

        const auto mainRoot =
            session.Commands().CreateObject(
                orbit::world_model::kWorldType,
                "Main Root");

        auto mainRoots = session.Explorer().Roots();
        Check(mainRoots.size() == 1U);
        Check(mainRoots.front().id == mainRoot);

        session.Explorer().Select(mainRoot, false);
        auto inspected =
            session.Inspector().SelectedObjects();
        Check(inspected.size() == 1U);
        Check(inspected.front().id == mainRoot);
        Check(session.Plugins().Statuses().empty());

        const auto firstGeneration =
            session.Generation();
        const auto secondary =
            project.CreateWorld(
                "Secondary",
                "Secondary");

        session.OpenWorld(secondary);
        Check(session.Generation() > firstGeneration);
        Check(session.Explorer().Roots().empty());
        Check(
            session.Inspector().
                SelectedObjects().empty());
        Check(session.Plugins().Statuses().empty());

        const auto secondaryRoot =
            session.Commands().CreateObject(
                orbit::world_model::kWorldType,
                "Secondary Root");

        const std::array selected{secondaryRoot};
        session.Selection().Set(selected);

        const auto secondaryRoots =
            session.Explorer().Roots();
        Check(secondaryRoots.size() == 1U);
        Check(secondaryRoots.front().id == secondaryRoot);
        Check(secondaryRoots.front().id != mainRoot);

        inspected =
            session.Inspector().SelectedObjects();
        Check(inspected.size() == 1U);
        Check(inspected.front().id == secondaryRoot);

        session.OpenWorld("Main");
        mainRoots = session.Explorer().Roots();
        Check(mainRoots.size() == 1U);
        Check(mainRoots.front().id == mainRoot);
        Check(
            session.Inspector().
                SelectedObjects().empty());
    }

    std::filesystem::remove_all(root);
    return 0;
}
