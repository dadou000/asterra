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
    RestartRequired,
    // An engine-owned shader source that its owner reloads in place (today: engine/weather_gpu/shaders), published to handlers like
    // the content kinds instead of triggering the generation refresh.
    EngineShader
};

[[nodiscard]] ChangeKind ClassifyChange(
    const std::filesystem::path& path);

[[nodiscard]] std::string_view ChangeKindName(
    ChangeKind kind) noexcept;
} // namespace orbit::hot_reload
