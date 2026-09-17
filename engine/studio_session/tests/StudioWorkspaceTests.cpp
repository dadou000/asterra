#include <orbit/studio_session/StudioWorkspace.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace
{
void Check(const bool condition)
{
    if (!condition)
    {
        std::cerr << "Studio workspace test failed.\n";
        std::exit(1);
    }
}

orbit::scene::ObjectId AddWorldRoot(
    orbit::studio_session::StudioWorkspace& workspace,
    const std::string_view name)
{
    return workspace.Session().World().Commands().CreateObject(
        orbit::world_model::kWorldType,
        name);
}
} // namespace

int main()
{
    const auto base =
        std::filesystem::temp_directory_path() /
        ("orbit-studio-workspace-" +
         orbit::documents::ProjectId::Random().ToString());
    const auto projectA = base / "ProjectA";
    const auto projectB = base / "ProjectB";
    std::filesystem::remove_all(base);

    {
        orbit::studio_session::StudioWorkspace workspace;
        Check(!workspace.HasProject());
        Check(workspace.Generation() == 0U);

        workspace.CreateProject(projectA, "Project A");
        Check(workspace.HasProject());
        const auto generationA = workspace.Generation();
        Check(generationA == 1U);
        Check(workspace.Project().Manifest().displayName == "Project A");

        const auto rootA = AddWorldRoot(workspace, "World A");
        Check(rootA.IsValid());

        workspace.CreateProject(projectB, "Project B");
        Check(workspace.Generation() > generationA);
        Check(workspace.Project().Manifest().displayName == "Project B");
        Check(workspace.Session().World().Objects().Roots().empty());

        const auto rootB = AddWorldRoot(workspace, "World B");
        Check(rootB.IsValid());

        workspace.OpenProject(projectA);
        Check(workspace.Project().Manifest().displayName == "Project A");
        const auto reopenedA =
            workspace.Session().World().Objects().Roots();
        Check(reopenedA.size() == 1U);
        Check(reopenedA.front().id == rootA);
        Check(reopenedA.front().name == "World A");

        const auto stableGeneration = workspace.Generation();
        bool rejectedMissing = false;

        try
        {
            workspace.OpenProject(base / "MissingProject");
        }
        catch (const std::exception&)
        {
            rejectedMissing = true;
        }

        Check(rejectedMissing);
        Check(workspace.HasProject());
        Check(workspace.Generation() == stableGeneration);
        Check(workspace.Project().Manifest().displayName == "Project A");
        Check(
            workspace.Session().World().Objects().Roots().front().id ==
            rootA);

        workspace.CloseProject();
        Check(!workspace.HasProject());
        Check(workspace.Generation() == stableGeneration + 1U);

        workspace.OpenProject(projectB / "Project.orbit.toml");
        Check(workspace.Project().Manifest().displayName == "Project B");
        const auto reopenedB =
            workspace.Session().World().Objects().Roots();
        Check(reopenedB.size() == 1U);
        Check(reopenedB.front().id == rootB);
        Check(reopenedB.front().name == "World B");
    }

    std::filesystem::remove_all(base);
    return 0;
}
