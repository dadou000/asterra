#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace orbit::hot_reload
{
enum class ChangeKind : std::uint8_t
{
    Ignored,
    NativeModule,
    Shader,
    Script,
    Content,
    RestartRequired
};

[[nodiscard]] ChangeKind ClassifyChange(
    const std::filesystem::path& path);

[[nodiscard]] std::string_view ChangeKindName(
    ChangeKind kind) noexcept;
} // namespace orbit::hot_reload
