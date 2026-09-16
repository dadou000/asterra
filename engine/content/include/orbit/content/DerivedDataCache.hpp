#pragma once

#include <orbit/content/ContentHash.hpp>

#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace orbit::content
{
class DerivedDataCache
{
public:
    explicit DerivedDataCache(
        std::filesystem::path root);

    [[nodiscard]] const std::filesystem::path&
    Root() const noexcept;

    [[nodiscard]] std::filesystem::path ArtifactPath(
        const ContentHash& key,
        std::string_view artifactName) const;

    [[nodiscard]] bool Contains(
        const ContentHash& key,
        std::string_view artifactName) const;

    [[nodiscard]] std::optional<std::vector<std::byte>>
    Read(
        const ContentHash& key,
        std::string_view artifactName) const;

    // Stores an immutable derived artifact. Concurrent writers of the same
    // key/artifact converge on the same path; the first completed rename wins.
    [[nodiscard]] std::filesystem::path Store(
        const ContentHash& key,
        std::string_view artifactName,
        std::span<const std::byte> bytes);

    void Remove(
        const ContentHash& key);

private:
    [[nodiscard]] static std::string
    ValidateArtifactName(
        std::string_view artifactName);

    [[nodiscard]] std::filesystem::path
    KeyDirectory(
        const ContentHash& key) const;

    std::filesystem::path root_;
};
} // namespace orbit::content
