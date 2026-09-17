#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/studio_session/ProjectBrowserModel.hpp>
#include <orbit/studio_session/StudioWorkspace.hpp>

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
            << "Project browser model test failed at "
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
        ("orbit-project-browser-" +
         orbit::documents::ProjectId::Random().ToString());
    const auto recentFile = root / "Ui" / "RecentProjects.txt";
    const auto projectA = root / "ProjectA";
    const auto projectB = root / "ProjectB";
    const auto projectC = root / "ProjectC";

    std::filesystem::remove_all(root);

    {
        orbit::studio_session::StudioWorkspace workspace;
        orbit::studio_session::ProjectBrowserModel browser(
            workspace,
            recentFile,
            2U);

        Check(!browser.HasProject());
        Check(browser.RecentProjects().empty());

        browser.CreateProject(projectA, "Project A");
        Check(browser.HasProject());
        Check(workspace.Project().Manifest().displayName == "Project A");

        auto recent = browser.RecentProjects();
        Check(recent.size() == 1U);
        Check(recent[0].available);
        Check(recent[0].displayName == "Project A");
        Check(
            recent[0].startupWorld ==
            std::filesystem::path("Worlds/Main.orbitworld"));
        const auto projectAId = recent[0].projectId;

        browser.CreateProject(projectB, "Project B");
        recent = browser.RecentProjects();
        Check(recent.size() == 2U);
        Check(recent[0].displayName == "Project B");
        Check(recent[1].displayName == "Project A");
        Check(recent[1].projectId == projectAId);

        const auto generationBeforeFailure = workspace.Generation();
        bool rejectedMissing = false;

        try
        {
            browser.OpenProject(root / "MissingProject.orbit.toml");
        }
        catch (const std::runtime_error&)
        {
            rejectedMissing = true;
        }

        Check(rejectedMissing);
        Check(workspace.Generation() == generationBeforeFailure);
        Check(workspace.Project().Manifest().displayName == "Project B");
        recent = browser.RecentProjects();
        Check(recent.size() == 2U);
        Check(recent[0].displayName == "Project B");

        browser.CreateProject(projectC, "Project C");
        recent = browser.RecentProjects();
        Check(recent.size() == 2U);
        Check(recent[0].displayName == "Project C");
        Check(recent[1].displayName == "Project B");

        browser.OpenProject(projectB);
        recent = browser.RecentProjects();
        Check(recent.size() == 2U);
        Check(recent[0].displayName == "Project B");
        Check(recent[1].displayName == "Project C");

        browser.CloseProject();
        Check(!browser.HasProject());
        Check(std::filesystem::is_regular_file(recentFile));
    }

    {
        orbit::studio_session::StudioWorkspace workspace;
        orbit::studio_session::ProjectBrowserModel browser(
            workspace,
            recentFile,
            2U);

        auto recent = browser.RecentProjects();
        Check(recent.size() == 2U);
        Check(recent[0].displayName == "Project B");
        Check(recent[1].displayName == "Project C");

        browser.OpenProject(recent[0].manifestPath);
        Check(workspace.Project().Manifest().displayName == "Project B");

        browser.ForgetRecentProject(projectC);
        recent = browser.RecentProjects();
        Check(recent.size() == 1U);
        Check(recent[0].displayName == "Project B");

        browser.CloseProject();
        std::filesystem::remove_all(projectB);

        recent = browser.RecentProjects();
        Check(recent.size() == 1U);
        Check(!recent[0].available);
        Check(!recent[0].error.empty());

        browser.ClearRecentProjects();
        Check(browser.RecentProjects().empty());
    }

    {
        orbit::studio_session::StudioWorkspace workspace;
        orbit::studio_session::ProjectBrowserModel browser(
            workspace,
            recentFile,
            2U);
        Check(browser.RecentProjects().empty());
    }

    std::filesystem::remove_all(root);
    return 0;
}
