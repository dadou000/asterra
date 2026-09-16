#pragma once

#include <orbit/core/Types.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace orbit::content
{
class ContentHash
{
public:
    static constexpr std::size_t Size = 32;

    ContentHash() = default;
    explicit ContentHash(
        std::array<std::byte, Size> bytes) noexcept;

    [[nodiscard]] const std::array<std::byte, Size>&
    Bytes() const noexcept;

    [[nodiscard]] std::string ToHex() const;

    [[nodiscard]] bool operator==(
        const ContentHash&) const noexcept = default;

private:
    std::array<std::byte, Size> bytes_{};
};

[[nodiscard]] ContentHash HashBytes(
    std::span<const std::byte> bytes);

[[nodiscard]] ContentHash HashString(
    std::string_view text);

[[nodiscard]] ContentHash HashFile(
    const std::filesystem::path& path);
} // namespace orbit::content
