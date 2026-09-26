#include <orbit/content/ContentService.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace orbit::content
{
namespace
{
[[nodiscard]] bool IsInside(
    const std::filesystem::path& path,
    const std::filesystem::path& root)
{
    const auto relative = path.lexically_relative(root);
    if (relative.empty() || relative.is_absolute())
    {
        return false;
    }
    return *relative.begin() != std::filesystem::path("..");
}

// A single file or folder name: no separators, no reserved characters, not
// "." / "..", and no trailing dot or space (Windows silently strips those).
void ValidateEntryName(const std::string_view name)
{
    if (name.empty() || name == "." || name == "..")
    {
        throw std::invalid_argument("Name must not be empty, '.' or '..'.");
    }

    constexpr std::string_view kReserved = "\\/:*?\"<>|";
    if (name.find_first_of(kReserved) != std::string_view::npos)
    {
        throw std::invalid_argument(
            "Name must not contain any of \\ / : * ? \" < > |.");
    }

    for (const unsigned char c : name)
    {
        if (c < 0x20U)
        {
            throw std::invalid_argument(
                "Name must not contain control characters.");
        }
    }

    if (name.back() == '.' || name.back() == ' ')
    {
        throw std::invalid_argument(
            "Name must not end with a dot or a space.");
    }
}

// Sidecar metadata that travels with a source file.
void MoveSidecars(
    const std::filesystem::path& from,
    const std::filesystem::path& to)
{
    for (const std::string_view suffix :
         {std::string_view(".orbitshader.toml"),
          std::string_view(".orbitimport.toml")})
    {
        auto sidecarFrom = from;
        sidecarFrom += suffix;

        if (!std::filesystem::is_regular_file(sidecarFrom))
        {
            continue;
        }

        auto sidecarTo = to;
        sidecarTo += suffix;
        std::filesystem::create_directories(sidecarTo.parent_path());
        std::filesystem::rename(sidecarFrom, sidecarTo);
    }
}
} // namespace

std::filesystem::path ContentService::ResolveContentEntry(
    const std::filesystem::path& entry,
    const bool mustExist) const
{
    if (entry.empty())
    {
        throw std::invalid_argument("Content path must not be empty.");
    }

    const auto absolute = std::filesystem::weakly_canonical(
        entry.is_absolute() ? entry : projectRoot_ / entry);

    // The Content root itself is the mount, never an entry: refusing it here
    // keeps rename/move/trash from ever operating on the mount.
    if (!IsInside(
            absolute,
            std::filesystem::weakly_canonical(contentRoot_)))
    {
        throw std::invalid_argument(
            "Path is outside the project's Content mount: " +
            entry.generic_string());
    }

    if (mustExist && !std::filesystem::exists(absolute))
    {
        throw std::invalid_argument(
            "Content entry does not exist: " + entry.generic_string());
    }

    return absolute;
}

std::vector<std::filesystem::path> ContentService::Folders() const
{
    std::vector<std::filesystem::path> result;

    if (!std::filesystem::is_directory(contentRoot_))
    {
        return result;
    }

    for (const auto& item : std::filesystem::recursive_directory_iterator(
             contentRoot_,
             std::filesystem::directory_options::skip_permission_denied))
    {
        if (item.is_directory())
        {
            result.push_back(
                std::filesystem::relative(item.path(), projectRoot_));
        }
    }

    std::ranges::sort(
        result,
        [](const auto& a, const auto& b)
        {
            return a.generic_string() < b.generic_string();
        });
    return result;
}

void ContentService::CreateFolder(const std::filesystem::path& folder)
{
    const auto absolute = ResolveContentEntry(folder, false);

    if (std::filesystem::exists(absolute))
    {
        throw std::invalid_argument(
            "Content entry already exists: " + folder.generic_string());
    }

    ValidateEntryName(absolute.filename().string());
    std::filesystem::create_directories(absolute);
    Scan();
}

std::filesystem::path ContentService::RenameEntry(
    const std::filesystem::path& entry,
    const std::string_view newName)
{
    ValidateEntryName(newName);

    const auto source = ResolveContentEntry(entry, true);
    const auto target = source.parent_path() / std::string(newName);

    if (std::filesystem::exists(target))
    {
        throw std::invalid_argument(
            "A Content entry named '" + std::string(newName) +
            "' already exists there.");
    }

    std::filesystem::rename(source, target);
    MoveSidecars(source, target);
    Scan();
    return std::filesystem::relative(target, projectRoot_);
}

std::filesystem::path ContentService::MoveEntry(
    const std::filesystem::path& entry,
    const std::filesystem::path& destinationFolder)
{
    const auto source = ResolveContentEntry(entry, true);

    // The destination may be the Content root (moving something to the top
    // level), which ResolveContentEntry deliberately refuses as an entry.
    const auto contentRoot = std::filesystem::weakly_canonical(contentRoot_);
    const auto requested = std::filesystem::weakly_canonical(
        destinationFolder.is_absolute()
            ? destinationFolder
            : projectRoot_ / destinationFolder);

    if (requested != contentRoot &&
        !IsInside(requested, contentRoot))
    {
        throw std::invalid_argument(
            "Destination is outside the project's Content mount.");
    }

    if (!std::filesystem::is_directory(requested))
    {
        throw std::invalid_argument(
            "Destination folder does not exist: " +
            destinationFolder.generic_string());
    }

    if (std::filesystem::is_directory(source) &&
        (requested == source || IsInside(requested, source)))
    {
        throw std::invalid_argument(
            "A folder cannot be moved into itself.");
    }

    const auto target = requested / source.filename();

    if (target == source)
    {
        return std::filesystem::relative(source, projectRoot_);
    }

    if (std::filesystem::exists(target))
    {
        throw std::invalid_argument(
            "Destination already contains '" +
            source.filename().string() + "'.");
    }

    std::filesystem::rename(source, target);
    MoveSidecars(source, target);
    Scan();
    return std::filesystem::relative(target, projectRoot_);
}

std::filesystem::path ContentService::TrashEntry(
    const std::filesystem::path& entry)
{
    const auto source = ResolveContentEntry(entry, true);

    const auto stamp = std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    const auto relative = std::filesystem::relative(source, projectRoot_);
    const auto target =
        projectRoot_ / ".orbit" / "Trash" / stamp / relative;

    std::filesystem::create_directories(target.parent_path());
    std::filesystem::rename(source, target);
    MoveSidecars(source, target);
    Scan();
    return std::filesystem::relative(target, projectRoot_);
}

std::string ContentService::ReadText(
    const std::filesystem::path& file) const
{
    const auto absolute = ResolveContentEntry(file, true);

    if (!std::filesystem::is_regular_file(absolute))
    {
        throw std::invalid_argument(
            "Not a file: " + file.generic_string());
    }

    std::ifstream input(absolute, std::ios::binary);
    if (!input)
    {
        throw std::runtime_error(
            "Could not open " + file.generic_string());
    }

    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

void ContentService::WriteText(
    const std::filesystem::path& file,
    const std::string_view text)
{
    const auto absolute = ResolveContentEntry(file, false);
    ValidateEntryName(absolute.filename().string());

    if (std::filesystem::is_directory(absolute))
    {
        throw std::invalid_argument(
            "Path is a folder: " + file.generic_string());
    }

    std::filesystem::create_directories(absolute.parent_path());

    // Write beside the target and rename over it: a watcher sees the finished
    // file (the temp name is classified as a transient editor file and
    // ignored), never a truncated one.
    auto temporary = absolute;
    temporary += ".orbit-write.tmp";

    {
        std::ofstream output(
            temporary, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw std::runtime_error(
                "Could not write " + file.generic_string());
        }
        output.write(
            text.data(),
            static_cast<std::streamsize>(text.size()));
        output.flush();
        if (!output)
        {
            throw std::runtime_error(
                "Could not write " + file.generic_string());
        }
    }

    std::error_code error;
    std::filesystem::rename(temporary, absolute, error);
    if (error)
    {
        // rename() over an existing file is not atomic-replace on every
        // platform/filesystem; fall back to remove + rename.
        std::filesystem::remove(absolute, error);
        std::filesystem::rename(temporary, absolute);
    }

    Scan();
}
} // namespace orbit::content
