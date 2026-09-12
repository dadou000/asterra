#pragma once

#include <filesystem>
#include <string_view>

namespace orbit::platform
{
struct CrashHandlerConfig
{
    std::filesystem::path directory{};
    std::string_view applicationName{"Orbit"};
    bool writeMiniDump{true};
};

[[nodiscard]] bool InstallCrashHandler(
    const CrashHandlerConfig& config = {}) noexcept;

void UninstallCrashHandler() noexcept;
} // namespace orbit::platform
