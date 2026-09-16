#include <orbit/build/BuildService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/runtime_project/CookedProject.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <system_error>

#define ORBIT_TEST_CHECK(expression) \
    do \
    { \
        if (!(expression)) \
        { \
            std::cerr \
                << "RuntimeProject test failed: " \
                << #expression \
                << " at line " \
                << __LINE__ \
                << '\n'; \
            return 1; \
        } \
    } while (false)

namespace
{
void WriteText(
    const std::filesystem::path& path,
    const std::string_view text)
{
    std::filesystem::create_directories(
        path.parent_path());

    std::ofstream output(
        path,
        std::ios::binary |
            std::ios::trunc);

    if (!output)
    {
        throw std::runtime_error(
            "Unable to create runtime project test file.");
    }

    output << text;

    if (!output)
    {
        throw std::runtime_error(
            "Unable to write runtime project test file.");
    }
}

class TemporaryDirectory
{
public:
    TemporaryDirectory()
        : path_(
              std::filesystem::
                  temp_directory_path() /
              ("orbit-runtime-project-" +
               orbit::documents::
                   ProjectId::Random().
                   ToString()))
    {
    }

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(
            path_,
            ignored);
    }

    [[nodiscard]] const std::filesystem::path&
    Path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};
} // namespace

int main()
{
    try
    {
        TemporaryDirectory temporary;

        auto project =
            orbit::documents::
                ProjectDocument::Create(
                    temporary.Path(),
                    "Runtime Project Test");

        WriteText(
            temporary.Path() /
                "Scripts/main.luau",
            "assert(Orbit.project_name == \"Runtime Project Test\")\n"
            "assert(type(Orbit.project_id) == \"string\")\n"
            "return true\n");

        project.Manifest().
            scriptEntryPoints = {
                "Scripts/main.luau"
            };
        project.Save();

        orbit::build::BuildService
            buildService;

        const auto build =
            buildService.Cook({
                .manifestPath =
                    project.ManifestPath(),
                .profileName =
                    "Development Windows"
            });

        ORBIT_TEST_CHECK(
            build.Succeeded());

        const auto cooked =
            orbit::runtime_project::
                CookedProject::Open(
                    build.outputDirectory);

        ORBIT_TEST_CHECK(
            cooked.Manifest().
                projectId ==
            project.Manifest().
                projectId);
        ORBIT_TEST_CHECK(
            cooked.Manifest().
                projectName ==
            "Runtime Project Test");
        ORBIT_TEST_CHECK(
            std::filesystem::
                is_regular_file(
                    cooked.
                        StartupWorldPath()));
        ORBIT_TEST_CHECK(
            cooked.Manifest().
                scripts.size() ==
            1U);
        ORBIT_TEST_CHECK(
            cooked.FindScript(
                "Scripts/main.luau") !=
            nullptr);

        orbit::runtime_project::
            ScriptRuntime runtime(
                cooked);

        runtime.ExecuteEntryPoints();

        const auto manifestOnly =
            orbit::runtime_project::
                CookedProject::Open(
                    build.manifestPath);

        ORBIT_TEST_CHECK(
            manifestOnly.RootDirectory() ==
            cooked.RootDirectory());

        WriteText(
            build.outputDirectory /
                "escape.txt",
            "outside test\n");

        bool escapedRejected = false;

        try
        {
            static_cast<void>(
                cooked.
                    ResolvePackagedFile(
                        "../escape.txt"));
        }
        catch (const std::exception&)
        {
            escapedRejected = true;
        }

        ORBIT_TEST_CHECK(
            escapedRejected);

        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr
            << "RuntimeProject test threw: "
            << exception.what()
            << '\n';
        return 1;
    }
}
