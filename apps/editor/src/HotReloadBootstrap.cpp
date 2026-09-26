#include <orbit/hot_reload/HotReloadHost.hpp>

#include <orbit/core/Log.hpp>

#include <windows.h>

#include <array>
#include <chrono>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

#ifndef ORBIT_SOURCE_ROOT
#error ORBIT_SOURCE_ROOT must be defined for the Studio hot-reload bootstrap.
#endif

#ifndef ORBIT_BINARY_ROOT
#error ORBIT_BINARY_ROOT must be defined for the Studio hot-reload bootstrap.
#endif

#ifndef ORBIT_BUILD_CONFIG
#error ORBIT_BUILD_CONFIG must be defined for the Studio hot-reload bootstrap.
#endif

#ifndef ORBIT_HOT_RELOAD_PROBE_BINARY
#error ORBIT_HOT_RELOAD_PROBE_BINARY must be defined for the Studio hot-reload bootstrap.
#endif

namespace
{
[[nodiscard]] std::filesystem::path CurrentExecutablePath()
{
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetModuleFileNameW(
        nullptr,
        buffer.data(),
        static_cast<DWORD>(buffer.size()));

    if (length == 0U || static_cast<std::size_t>(length) >= buffer.size())
    {
        return {};
    }

    return std::filesystem::path(std::wstring(buffer.data(), length));
}

[[nodiscard]] bool IsInside(
    const std::filesystem::path& child,
    const std::filesystem::path& parent)
{
    std::error_code error;
    const auto relative = std::filesystem::relative(child, parent, error);
    if (error || relative.empty())
    {
        return false;
    }

    const auto first = relative.begin();
    return first != relative.end() &&
        *first != std::filesystem::path("..");
}

class StudioHotReloadBootstrap
{
public:
    StudioHotReloadBootstrap() noexcept
    {
        try
        {
            const std::filesystem::path sourceRoot = ORBIT_SOURCE_ROOT;
            const std::filesystem::path binaryRoot = ORBIT_BINARY_ROOT;
            const auto executable = CurrentExecutablePath();

            if (executable.empty())
            {
                return;
            }

            std::error_code error;
            const auto executableDirectory =
                std::filesystem::weakly_canonical(executable.parent_path(), error);
            error.clear();
            const auto canonicalSource =
                std::filesystem::weakly_canonical(sourceRoot, error);
            error.clear();
            const auto canonicalBinary =
                std::filesystem::weakly_canonical(binaryRoot, error);

            // Hot reload belongs to the root development Orbit.exe and build-tree
            // Studio only. Packaged executables under dist/ stay self-contained.
            if (executableDirectory != canonicalSource &&
                !IsInside(executable, canonicalBinary))
            {
                return;
            }

            if (!std::filesystem::exists(canonicalBinary / "CMakeCache.txt"))
            {
                orbit::log::Warning(
                    "Orbit hot reload is disabled because the development CMake tree is missing.");
                return;
            }

            orbit::hot_reload::HotReloadHostConfig config{
                .sourceRoot = canonicalSource,
                .binaryRoot = canonicalBinary,
                .runtimeCopyRoot = canonicalBinary / "hot_reload_runtime",
                .buildConfiguration = ORBIT_BUILD_CONFIG,
                .pollInterval = std::chrono::milliseconds(200),
                .automaticBuilds = true
            };

            host_ = std::make_unique<orbit::hot_reload::HotReloadHost>(
                std::move(config));

            host_->RegisterModule({
                .name = "OrbitHotReloadProbe",
                .buildTarget = "OrbitHotReloadProbe",
                .binaryPath = std::filesystem::path(ORBIT_HOT_RELOAD_PROBE_BINARY),
                .sourceRoots = {
                    canonicalSource / "engine" / "hot_reload_probe"
                },
                .debounce = std::chrono::milliseconds(250)
            });

            host_->Start();
        }
        catch (const std::exception& exception)
        {
            orbit::log::Error(
                std::string("Orbit hot-reload bootstrap failed: ") + exception.what());
        }
        catch (...)
        {
            orbit::log::Error("Orbit hot-reload bootstrap failed with an unknown error.");
        }
    }

private:
    std::unique_ptr<orbit::hot_reload::HotReloadHost> host_;
};

StudioHotReloadBootstrap gStudioHotReloadBootstrap;
} // namespace
