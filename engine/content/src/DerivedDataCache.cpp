#include <orbit/content/DerivedDataCache.hpp>

#include <atomic>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <system_error>

namespace orbit::content
{
namespace
{
std::atomic<u64> gTemporarySequence{0};

[[nodiscard]] std::filesystem::path
TemporaryPath(
    const std::filesystem::path& destination)
{
    const u64 sequence =
        gTemporarySequence.fetch_add(
            1,
            std::memory_order_relaxed);

    const auto ticks =
        std::chrono::steady_clock::now().
            time_since_epoch().
            count();

    std::filesystem::path result =
        destination;

    result +=
        ".tmp-" +
        std::to_string(ticks) +
        "-" +
        std::to_string(sequence);

    return result;
}
} // namespace

DerivedDataCache::DerivedDataCache(
    std::filesystem::path root)
    : root_(
          std::filesystem::absolute(
              std::move(root)))
{
    std::filesystem::create_directories(
        root_);
}

const std::filesystem::path&
DerivedDataCache::Root() const noexcept
{
    return root_;
}

std::string
DerivedDataCache::ValidateArtifactName(
    const std::string_view artifactName)
{
    if (artifactName.empty())
    {
        throw std::invalid_argument(
            "DDC artifact name must not be empty.");
    }

    const std::filesystem::path path(
        artifactName);

    if (path.has_parent_path() ||
        path.is_absolute() ||
        path.filename() != path ||
        artifactName == "." ||
        artifactName == "..")
    {
        throw std::invalid_argument(
            "DDC artifact name must be a single file name.");
    }

    return path.string();
}

std::filesystem::path
DerivedDataCache::KeyDirectory(
    const ContentHash& key) const
{
    const std::string hex =
        key.ToHex();

    return root_ /
        hex.substr(0, 2) /
        hex;
}

std::filesystem::path
DerivedDataCache::ArtifactPath(
    const ContentHash& key,
    const std::string_view artifactName) const
{
    return KeyDirectory(key) /
        ValidateArtifactName(
            artifactName);
}

bool DerivedDataCache::Contains(
    const ContentHash& key,
    const std::string_view artifactName) const
{
    std::error_code error;

    return std::filesystem::is_regular_file(
        ArtifactPath(
            key,
            artifactName),
        error) &&
        !error;
}

std::optional<std::vector<std::byte>>
DerivedDataCache::Read(
    const ContentHash& key,
    const std::string_view artifactName) const
{
    const auto path =
        ArtifactPath(
            key,
            artifactName);

    std::ifstream input(
        path,
        std::ios::binary);

    if (!input)
    {
        return std::nullopt;
    }

    input.seekg(
        0,
        std::ios::end);

    const auto size =
        input.tellg();

    if (size < 0)
    {
        throw std::runtime_error(
            "Unable to determine DDC artifact size: " +
            path.string());
    }

    input.seekg(
        0,
        std::ios::beg);

    std::vector<std::byte> result(
        static_cast<std::size_t>(
            size));

    if (!result.empty())
    {
        input.read(
            reinterpret_cast<char*>(
                result.data()),
            static_cast<std::streamsize>(
                result.size()));

        if (!input)
        {
            throw std::runtime_error(
                "Unable to read DDC artifact: " +
                path.string());
        }
    }

    return result;
}

std::filesystem::path
DerivedDataCache::Store(
    const ContentHash& key,
    const std::string_view artifactName,
    const std::span<const std::byte> bytes)
{
    const auto destination =
        ArtifactPath(
            key,
            artifactName);

    if (Contains(
            key,
            artifactName))
    {
        return destination;
    }

    std::filesystem::create_directories(
        destination.parent_path());

    const auto temporary =
        TemporaryPath(
            destination);

    {
        std::ofstream output(
            temporary,
            std::ios::binary |
            std::ios::trunc);

        if (!output)
        {
            throw std::runtime_error(
                "Unable to create DDC temporary artifact: " +
                temporary.string());
        }

        if (!bytes.empty())
        {
            output.write(
                reinterpret_cast<const char*>(
                    bytes.data()),
                static_cast<std::streamsize>(
                    bytes.size()));
        }

        output.flush();

        if (!output)
        {
            std::filesystem::remove(
                temporary);
            throw std::runtime_error(
                "Unable to write DDC artifact: " +
                temporary.string());
        }
    }

    std::error_code renameError;
    std::filesystem::rename(
        temporary,
        destination,
        renameError);

    if (renameError)
    {
        if (std::filesystem::is_regular_file(
                destination))
        {
            std::filesystem::remove(
                temporary);
        }
        else
        {
            std::filesystem::remove(
                temporary);

            throw std::runtime_error(
                "Unable to publish DDC artifact: " +
                renameError.message());
        }
    }

    return destination;
}

void DerivedDataCache::Remove(
    const ContentHash& key)
{
    std::error_code error;
    std::filesystem::remove_all(
        KeyDirectory(key),
        error);

    if (error)
    {
        throw std::runtime_error(
            "Unable to remove DDC entry: " +
            error.message());
    }
}
} // namespace orbit::content
