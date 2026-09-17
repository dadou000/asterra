#include <orbit/build/BuildService.hpp>
#include <orbit/platform/Paths.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
enum class Action
{
    Cook,
    Package,
    Validate
};

struct Options
{
    std::filesystem::path manifestPath;
    std::string profileName;
    std::filesystem::path outputDirectory;
    Action action{Action::Cook};
    bool cleanOutput{true};
};

void PrintUsage()
{
    std::cout
        << "OrbitBuild - headless Orbit project builder\n\n"
        << "Usage:\n"
        << "  OrbitBuild <project-dir|Project.orbit.toml> [options]\n\n"
        << "Options:\n"
        << "  --profile <name>   Select a Project.orbit.toml build profile.\n"
        << "  --output <path>    Override the profile build output directory.\n"
        << "  --validate         Validate only; do not write build products.\n"
        << "  --cook             Validate and cook project products (default).\n"
        << "  --package          Cook and assemble an OrbitPlayer package.\n"
        << "  --no-clean         Refuse to replace an existing output directory.\n"
        << "  --help, -h         Show this help.\n";
}

[[nodiscard]] Options ParseOptions(
    const int argc,
    char** argv)
{
    if (argc < 2)
    {
        throw std::invalid_argument(
            "A project directory or Project.orbit.toml path is required.");
    }

    Options options;
    bool projectSeen = false;

    for (int index = 1;
         index < argc;
         ++index)
    {
        const std::string_view argument =
            argv[index];

        if (argument == "--help" ||
            argument == "-h")
        {
            PrintUsage();
            std::exit(0);
        }

        if (argument == "--profile")
        {
            if (++index >= argc)
            {
                throw std::invalid_argument(
                    "--profile requires a value.");
            }

            options.profileName =
                argv[index];
            continue;
        }

        if (argument == "--output")
        {
            if (++index >= argc)
            {
                throw std::invalid_argument(
                    "--output requires a value.");
            }

            options.outputDirectory =
                argv[index];
            continue;
        }

        if (argument == "--validate")
        {
            options.action =
                Action::Validate;
            continue;
        }

        if (argument == "--cook")
        {
            options.action =
                Action::Cook;
            continue;
        }

        if (argument == "--package")
        {
            options.action =
                Action::Package;
            continue;
        }

        if (argument == "--no-clean")
        {
            options.cleanOutput = false;
            continue;
        }

        if (!argument.empty() &&
            argument.front() == '-')
        {
            throw std::invalid_argument(
                "Unknown option: " +
                std::string(argument));
        }

        if (projectSeen)
        {
            throw std::invalid_argument(
                "Only one project path may be supplied.");
        }

        options.manifestPath =
            std::filesystem::path(
                argument);
        projectSeen = true;
    }

    if (!projectSeen)
    {
        throw std::invalid_argument(
            "A project directory or Project.orbit.toml path is required.");
    }

    if (std::filesystem::is_directory(
            options.manifestPath))
    {
        options.manifestPath /=
            "Project.orbit.toml";
    }

    options.manifestPath =
        std::filesystem::absolute(
            options.manifestPath).
            lexically_normal();

    if (!options.outputDirectory.empty())
    {
        options.outputDirectory =
            std::filesystem::absolute(
                options.outputDirectory).
                lexically_normal();
    }

    return options;
}

[[nodiscard]] std::filesystem::path
FindPlayerExecutable()
{
    const auto executable =
        orbit::platform::
            ExecutablePath();

    const auto sibling =
        executable.parent_path() /
        "OrbitPlayer.exe";

    if (std::filesystem::
            is_regular_file(
                sibling))
    {
        return sibling;
    }

    // Development tree:
    // build/apps/build/<Config>/OrbitBuild.exe
    // build/apps/player/<Config>/OrbitPlayer.exe
    const auto configuration =
        executable.parent_path().
            filename();

    const auto appsRoot =
        executable.parent_path().
            parent_path().
            parent_path();

    const auto development =
        appsRoot /
        "player" /
        configuration /
        "OrbitPlayer.exe";

    if (std::filesystem::
            is_regular_file(
                development))
    {
        return development;
    }

    throw std::runtime_error(
        "OrbitPlayer.exe was not found beside OrbitBuild or in the development build tree.");
}

[[nodiscard]] std::vector<
    std::filesystem::path>
FindPackageRuntimeFiles(
    const std::filesystem::path& manifestPath,
    const std::filesystem::path& playerExecutable)
{
    std::vector<std::filesystem::path>
        runtimeFiles;

    const auto platformConfiguration =
        manifestPath.parent_path() /
        "Config" /
        "PlatformServices.toml";

    if (std::filesystem::is_regular_file(
            platformConfiguration))
    {
        runtimeFiles.push_back(
            platformConfiguration);
    }

    const auto steamRuntime =
        playerExecutable.parent_path() /
        "steam_api64.dll";

    if (std::filesystem::is_regular_file(
            steamRuntime))
    {
        runtimeFiles.push_back(
            steamRuntime);
    }

    return runtimeFiles;
}

void PrintIssues(
    const std::vector<orbit::build::BuildIssue>& issues)
{
    for (const auto& issue :
         issues)
    {
        std::ostream& stream =
            issue.severity ==
                    orbit::build::
                        IssueSeverity::Error
                ? std::cerr
                : std::cout;

        stream
            << (issue.severity ==
                        orbit::build::
                            IssueSeverity::Error
                    ? "error"
                    : "warning")
            << " ["
            << issue.code
            << "] "
            << issue.message;

        if (!issue.path.empty())
        {
            stream
                << " ("
                << issue.path.string()
                << ")";
        }

        stream << '\n';
    }
}
} // namespace

int main(
    const int argc,
    char** argv)
{
    try
    {
        const Options options =
            ParseOptions(
                argc,
                argv);

        orbit::build::BuildService service;

        orbit::build::BuildRequest request{
            .manifestPath =
                options.manifestPath,
            .profileName =
                options.profileName,
            .outputDirectory =
                options.outputDirectory,
            .cleanOutput =
                options.cleanOutput
        };

        if (options.action ==
            Action::Validate)
        {
            const auto result =
                service.Validate(
                    request);

            PrintIssues(
                result.issues);

            if (!result.Succeeded())
            {
                return 2;
            }

            std::cout
                << "Project validation succeeded.\n"
                << "Profile: "
                << result.profile.name
                << "\nTarget: "
                << result.profile.platform
                << " / "
                << result.profile.configuration
                << " / "
                << result.profile.storefront
                << "\nOutput: "
                << result.outputDirectory.string()
                << '\n';

            return 0;
        }

        if (options.action ==
            Action::Package)
        {
            const auto playerExecutable =
                FindPlayerExecutable();

            const auto result =
                service.Package(
                    request,
                    {
                        .playerExecutable =
                            playerExecutable,
                        .runtimeFiles =
                            FindPackageRuntimeFiles(
                                options.manifestPath,
                                playerExecutable)
                    });

            PrintIssues(
                result.issues);

            if (!result.Succeeded())
            {
                return 4;
            }

            std::cout
                << "Package succeeded.\n"
                << "Profile: "
                << result.manifest.profile.name
                << "\nExecutable: "
                << result.executablePath.string()
                << "\nBuild manifest: "
                << result.manifestPath.string()
                << "\nPackage manifest: "
                << result.packageManifestPath.string()
                << '\n';

            return 0;
        }

        const auto result =
            service.Cook(
                request);

        PrintIssues(
            result.issues);

        if (!result.Succeeded())
        {
            return 3;
        }

        std::size_t cacheHits = 0;

        for (const auto& asset :
             result.manifest.assets)
        {
            if (asset.cacheHit)
            {
                ++cacheHits;
            }
        }

        std::cout
            << "Cook succeeded.\n"
            << "Profile: "
            << result.manifest.profile.name
            << "\nAssets: "
            << result.manifest.assets.size()
            << " ("
            << cacheHits
            << " target DDC hits)\n"
            << "Scripts: "
            << result.manifest.scripts.size()
            << "\nManifest: "
            << result.manifestPath.string()
            << '\n';

        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr
            << "OrbitBuild: "
            << exception.what()
            << "\n\n";

        PrintUsage();
        return 1;
    }
}
