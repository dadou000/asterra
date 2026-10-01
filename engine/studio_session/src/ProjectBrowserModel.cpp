#include <orbit/studio_session/ProjectBrowserModel.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace orbit::studio_session
{
namespace
{
constexpr std::string_view kRecentProjectsHeader =
    "ORBIT_RECENT_PROJECTS_V1";

[[nodiscard]] bool SameManifestPath(
    const std::filesystem::path& first,
    const std::filesystem::path& second)
{
    std::error_code error;

    if (std::filesystem::exists(first, error) && !error)
    {
        error.clear();
        if (std::filesystem::exists(second, error) && !error)
        {
            error.clear();
            const bool equivalent =
                std::filesystem::equivalent(first, second, error);

            if (!error)
            {
                return equivalent;
            }
        }
    }

    return first.lexically_normal() == second.lexically_normal();
}
} // namespace

ProjectBrowserModel::ProjectBrowserModel(
    StudioWorkspace& workspace,
    std::filesystem::path recentProjectsFile,
    const std::size_t maximumRecentProjects)
    : workspace_(&workspace),
      recentProjectsFile_(std::move(recentProjectsFile)),
      maximumRecentProjects_(maximumRecentProjects)
{
    if (recentProjectsFile_.empty())
    {
        throw std::invalid_argument(
            "Recent-project storage path must not be empty.");
    }

    if (maximumRecentProjects_ == 0U)
    {
        throw std::invalid_argument(
            "Project browser must retain at least one recent project.");
    }

    recentProjectsFile_ =
        std::filesystem::absolute(recentProjectsFile_).
            lexically_normal();
    Load();
}

void ProjectBrowserModel::CreateProject(
    const std::filesystem::path& rootDirectory,
    const std::string_view displayName)
{
    workspace_->CreateProject(rootDirectory, displayName);
    RecordCurrentProject();
}

void ProjectBrowserModel::OpenProject(
    const std::filesystem::path& path)
{
    // StudioWorkspace constructs and validates the complete candidate session
    // before replacing the current project. A failed open therefore leaves
    // both the active workspace and MRU ordering unchanged.
    workspace_->OpenProject(path);
    RecordCurrentProject();
}

void ProjectBrowserModel::CloseProject()
{
    workspace_->CloseProject();
}

bool ProjectBrowserModel::HasProject() const noexcept
{
    return workspace_->HasProject();
}

std::vector<RecentProjectItem>
ProjectBrowserModel::RecentProjects() const
{
    std::vector<RecentProjectItem> result;
    result.reserve(recentManifestPaths_.size());

    for (const auto& manifestPath : recentManifestPaths_)
    {
        RecentProjectItem item;
        item.manifestPath = manifestPath;

        try
        {
            if (!std::filesystem::is_regular_file(manifestPath))
            {
                item.error = "Project manifest is missing.";
                result.push_back(std::move(item));
                continue;
            }

            const auto manifest =
                documents::LoadProjectManifest(manifestPath);

            item.displayName = manifest.displayName;
            item.projectId = manifest.projectId;
            item.startupWorld = manifest.startupWorld;

            const auto startupWorld =
                manifestPath.parent_path() /
                manifest.startupWorld;

            if (!std::filesystem::is_regular_file(startupWorld))
            {
                item.error = "Project startup world is missing.";
                result.push_back(std::move(item));
                continue;
            }

            item.available = true;
        }
        catch (const std::exception& exception)
        {
            item.error = exception.what();
        }

        result.push_back(std::move(item));
    }

    return result;
}

std::vector<DiscoveredProject> ProjectBrowserModel::DiscoverProjects(
    const std::vector<std::filesystem::path>& roots,
    const ProjectDiscoveryOptions& options)
{
    namespace fs = std::filesystem;

    static constexpr std::string_view kSkipped[] = {
        ".git", ".vs", ".svn", "node_modules", "build", "Build", "bin", "obj",
        "vcpkg", "vcpkg_installed", "dist", "AppData", "$Recycle.Bin",
        "System Volume Information", "__pycache__", ".cache"};

    const auto deadline =
        std::chrono::steady_clock::now() + options.timeBudget;

    std::vector<DiscoveredProject> found;

    const auto known = [&found](const fs::path& manifest)
    {
        return std::ranges::any_of(
            found,
            [&manifest](const DiscoveredProject& item)
            {
                return item.manifestPath == manifest;
            });
    };

    for (const auto& root : roots)
    {
        std::error_code error;
        if (!fs::is_directory(root, error) || error)
        {
            continue;
        }

        fs::recursive_directory_iterator iterator(
            root,
            fs::directory_options::skip_permission_denied,
            error);

        if (error)
        {
            continue;
        }

        for (; iterator != fs::recursive_directory_iterator{};
             iterator.increment(error))
        {
            if (error)
            {
                error.clear();
                continue;
            }

            if (std::chrono::steady_clock::now() > deadline ||
                found.size() >= options.maximumResults)
            {
                return found;
            }

            const fs::directory_entry& entry = *iterator;
            std::error_code statusError;

            if (entry.is_directory(statusError))
            {
                const auto u8name = entry.path().filename().generic_u8string();
                const std::string name(u8name.begin(), u8name.end());
                const bool skipped = std::ranges::any_of(
                    kSkipped,
                    [&name](const std::string_view candidate)
                    {
                        return name == candidate;
                    });

                if (skipped ||
                    static_cast<u32>(iterator.depth()) + 1U >=
                        options.maximumDepth)
                {
                    iterator.disable_recursion_pending();
                }
                continue;
            }

            if (entry.path().filename() != "Project.orbit.toml")
            {
                continue;
            }

            const fs::path manifest =
                fs::weakly_canonical(entry.path(), statusError);
            const fs::path resolved =
                statusError ? entry.path() : manifest;

            if (known(resolved))
            {
                continue;
            }

            try
            {
                const auto loaded = documents::LoadProjectManifest(resolved);
                found.push_back({
                    .manifestPath = resolved,
                    .displayName = loaded.displayName,
                    .modified = fs::last_write_time(resolved, statusError)
                });
            }
            catch (const std::exception&)
            {
                // Not a usable Orbit project manifest; ignore it.
            }
        }
    }

    std::ranges::sort(
        found,
        [](const DiscoveredProject& a, const DiscoveredProject& b)
        {
            return a.modified > b.modified;
        });

    return found;
}

void ProjectBrowserModel::ForgetRecentProject(
    const std::filesystem::path& path)
{
    const auto manifestPath = NormalizeManifestPath(path);
    const auto oldSize = recentManifestPaths_.size();

    std::erase_if(
        recentManifestPaths_,
        [&manifestPath](const auto& recent)
        {
            return SameManifestPath(recent, manifestPath);
        });

    if (recentManifestPaths_.size() != oldSize)
    {
        Persist();
    }
}

void ProjectBrowserModel::ClearRecentProjects()
{
    if (recentManifestPaths_.empty())
    {
        return;
    }

    recentManifestPaths_.clear();
    Persist();
}

const std::filesystem::path&
ProjectBrowserModel::RecentProjectsFile() const noexcept
{
    return recentProjectsFile_;
}

std::filesystem::path
ProjectBrowserModel::NormalizeManifestPath(
    const std::filesystem::path& path)
{
    if (path.empty())
    {
        throw std::invalid_argument(
            "Project path must not be empty.");
    }

    std::filesystem::path manifestPath = path;

    if (std::filesystem::is_directory(manifestPath))
    {
        manifestPath /= "Project.orbit.toml";
    }

    manifestPath =
        std::filesystem::absolute(manifestPath).
            lexically_normal();

    std::error_code error;
    const auto canonical =
        std::filesystem::weakly_canonical(manifestPath, error);

    return error
        ? manifestPath
        : canonical;
}

void ProjectBrowserModel::Load()
{
    recentManifestPaths_.clear();

    if (!std::filesystem::is_regular_file(recentProjectsFile_))
    {
        return;
    }

    std::ifstream input(recentProjectsFile_, std::ios::binary);

    if (!input)
    {
        throw std::runtime_error(
            "Could not open recent-project storage.");
    }

    std::string line;

    if (!std::getline(input, line) ||
        line != kRecentProjectsHeader)
    {
        // Browser navigation history is non-authoritative. Unknown/corrupt
        // versions are ignored instead of risking project state.
        return;
    }

    while (recentManifestPaths_.size() < maximumRecentProjects_ &&
           std::getline(input, line))
    {
        if (line.empty())
        {
            continue;
        }

        const auto manifest =
            NormalizeManifestPath(
                std::filesystem::path(line));

        const bool duplicate =
            std::ranges::any_of(
                recentManifestPaths_,
                [&manifest](const auto& existing)
                {
                    return SameManifestPath(existing, manifest);
                });

        if (!duplicate)
        {
            recentManifestPaths_.push_back(manifest);
        }
    }
}

void ProjectBrowserModel::Persist() const
{
    const auto parent = recentProjectsFile_.parent_path();

    if (!parent.empty())
    {
        std::filesystem::create_directories(parent);
    }

    std::ofstream output(
        recentProjectsFile_,
        std::ios::binary | std::ios::trunc);

    if (!output)
    {
        throw std::runtime_error(
            "Could not write recent-project storage.");
    }

    output << kRecentProjectsHeader << '\n';

    for (const auto& manifest : recentManifestPaths_)
    {
        output << manifest.generic_string() << '\n';
    }

    output.flush();

    if (!output)
    {
        throw std::runtime_error(
            "Failed while writing recent-project storage.");
    }
}

void ProjectBrowserModel::RecordCurrentProject()
{
    if (!workspace_->HasProject())
    {
        throw std::logic_error(
            "Cannot record a recent project when no project is open.");
    }

    RecordManifest(
        workspace_->Project().ManifestPath());
}

void ProjectBrowserModel::RecordManifest(
    const std::filesystem::path& manifestPath)
{
    const auto normalized =
        NormalizeManifestPath(manifestPath);

    std::erase_if(
        recentManifestPaths_,
        [&normalized](const auto& recent)
        {
            return SameManifestPath(recent, normalized);
        });

    recentManifestPaths_.insert(
        recentManifestPaths_.begin(),
        normalized);

    if (recentManifestPaths_.size() > maximumRecentProjects_)
    {
        recentManifestPaths_.resize(maximumRecentProjects_);
    }

    Persist();
}
} // namespace orbit::studio_session
