#include <orbit/hot_reload/HotReloadHost.hpp>
#include <orbit/post_process/HumanEyeAdaptation.hpp>
#include <orbit/post_process/HumanEyeAdaptationHotReload.hpp>

#include <orbit/core/Log.hpp>

#include <windows.h>

#include <array>
#include <atomic>
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

#ifndef ORBIT_EYE_ADAPTATION_HOT_RELOAD_BINARY
#error ORBIT_EYE_ADAPTATION_HOT_RELOAD_BINARY must be defined for the Studio hot-reload bootstrap.
#endif

int OrbitStudioMain(int argc, char** argv);

namespace
{
std::atomic<orbit::hot_reload::HotReloadHost*>
    gEyeAdaptationHost{nullptr};

[[nodiscard]] std::filesystem::path CurrentExecutablePath()
{
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetModuleFileNameW(
        nullptr,
        buffer.data(),
        static_cast<DWORD>(buffer.size()));

    if (length == 0U ||
        static_cast<std::size_t>(length) >= buffer.size())
    {
        return {};
    }

    return std::filesystem::path(
        std::wstring(
            buffer.data(),
            length));
}

[[nodiscard]] bool IsInside(
    const std::filesystem::path& child,
    const std::filesystem::path& parent)
{
    std::error_code error;
    const auto relative =
        std::filesystem::relative(
            child,
            parent,
            error);

    if (error || relative.empty())
    {
        return false;
    }

    const auto first = relative.begin();
    return first != relative.end() &&
        *first != std::filesystem::path("..");
}

struct EyeAdaptationInvocation
{
    orbit::post_process::HumanEyeAdaptationState state{};
    const orbit::post_process::LuminanceHistogramStatistics* statistics{nullptr};
    orbit::f32 deltaSeconds{0.0F};
    const orbit::post_process::HumanEyeAdaptationConfig* config{nullptr};
    orbit::post_process::HumanEyeAdaptationState result{};
    bool invoked{false};
};

void VisitEyeAdaptationInterface(
    const void* const interfacePointer,
    void* const userData) noexcept
{
    if (interfacePointer == nullptr ||
        userData == nullptr)
    {
        return;
    }

    const auto* api =
        static_cast<const orbit::post_process::
            HumanEyeAdaptationHotReloadInterface*>(
                interfacePointer);
    auto* invocation =
        static_cast<EyeAdaptationInvocation*>(
            userData);

    if (api->abiVersion !=
            orbit::post_process::
                kHumanEyeAdaptationHotReloadInterfaceVersion ||
        api->update == nullptr ||
        invocation->statistics == nullptr ||
        invocation->config == nullptr)
    {
        return;
    }

    invocation->result =
        api->update(
            invocation->state,
            *invocation->statistics,
            invocation->deltaSeconds,
            *invocation->config);
    invocation->invoked = true;
}

orbit::post_process::HumanEyeAdaptationState
UpdateEyeAdaptationThroughHotReload(
    orbit::post_process::HumanEyeAdaptationState state,
    const orbit::post_process::LuminanceHistogramStatistics& statistics,
    const orbit::f32 deltaSeconds,
    const orbit::post_process::HumanEyeAdaptationConfig& config) noexcept
{
    auto* const host =
        gEyeAdaptationHost.load(
            std::memory_order_acquire);

    if (host != nullptr)
    {
        EyeAdaptationInvocation invocation{
            .state = state,
            .statistics = &statistics,
            .deltaSeconds = deltaSeconds,
            .config = &config,
            .result = state,
            .invoked = false
        };

        try
        {
            const bool visited =
                host->VisitInterface(
                    orbit::post_process::
                        kHumanEyeAdaptationHotReloadModuleName,
                    orbit::post_process::
                        kHumanEyeAdaptationHotReloadInterfaceName,
                    orbit::post_process::
                        kHumanEyeAdaptationHotReloadInterfaceVersion,
                    &VisitEyeAdaptationInterface,
                    &invocation);

            if (visited && invocation.invoked)
            {
                return invocation.result;
            }
        }
        catch (...)
        {
            // Hot reload is a development acceleration layer. Any host-side
            // failure falls back to the statically linked production path.
        }
    }

    return orbit::post_process::
        UpdateHumanEyeAdaptationBuiltin(
            state,
            statistics,
            deltaSeconds,
            config);
}

class StudioHotReloadBootstrap
{
public:
    StudioHotReloadBootstrap() noexcept
    {
        try
        {
            const std::filesystem::path sourceRoot =
                ORBIT_SOURCE_ROOT;
            const std::filesystem::path binaryRoot =
                ORBIT_BINARY_ROOT;
            const auto executable =
                CurrentExecutablePath();

            if (executable.empty())
            {
                return;
            }

            std::error_code error;
            const auto executableDirectory =
                std::filesystem::weakly_canonical(
                    executable.parent_path(),
                    error);
            error.clear();
            const auto canonicalSource =
                std::filesystem::weakly_canonical(
                    sourceRoot,
                    error);
            error.clear();
            const auto canonicalBinary =
                std::filesystem::weakly_canonical(
                    binaryRoot,
                    error);

            // Hot reload belongs to the root development Orbit.exe and
            // build-tree Studio only. Packaged executables remain self-contained.
            if (executableDirectory != canonicalSource &&
                !IsInside(
                    executable,
                    canonicalBinary))
            {
                return;
            }

            if (!std::filesystem::exists(
                    canonicalBinary /
                    "CMakeCache.txt"))
            {
                orbit::log::Warning(
                    "Orbit hot reload is disabled because the development CMake tree is missing.");
                return;
            }

            orbit::hot_reload::HotReloadHostConfig config{
                .sourceRoot = canonicalSource,
                .binaryRoot = canonicalBinary,
                .runtimeCopyRoot =
                    canonicalBinary /
                    "hot_reload_runtime",
                .buildConfiguration =
                    ORBIT_BUILD_CONFIG,
                .pollInterval =
                    std::chrono::milliseconds(200),
                .automaticBuilds = true
            };

            host_ =
                std::make_unique<
                    orbit::hot_reload::HotReloadHost>(
                        std::move(config));

            host_->RegisterModule({
                .name = "OrbitHotReloadProbe",
                .buildTarget = "OrbitHotReloadProbe",
                .binaryPath =
                    std::filesystem::path(
                        ORBIT_HOT_RELOAD_PROBE_BINARY),
                .sourceRoots = {
                    canonicalSource /
                        "engine" /
                        "hot_reload_probe"
                },
                .debounce =
                    std::chrono::milliseconds(250)
            });

            host_->RegisterModule({
                .name =
                    orbit::post_process::
                        kHumanEyeAdaptationHotReloadModuleName,
                .buildTarget =
                    "OrbitEyeAdaptationHotReload",
                .binaryPath =
                    std::filesystem::path(
                        ORBIT_EYE_ADAPTATION_HOT_RELOAD_BINARY),
                .sourceRoots = {
                    canonicalSource /
                        "engine" /
                        "post_process" /
                        "src" /
                        "HumanEyeAdaptation.cpp",
                    canonicalSource /
                        "engine" /
                        "post_process" /
                        "include" /
                        "orbit" /
                        "post_process" /
                        "HumanEyeAdaptation.hpp",
                    canonicalSource /
                        "engine" /
                        "post_process" /
                        "include" /
                        "orbit" /
                        "post_process" /
                        "HumanEyeAdaptationHotReload.hpp",
                    canonicalSource /
                        "engine" /
                        "post_process_hot_reload"
                },
                .debounce =
                    std::chrono::milliseconds(250)
            });

            gEyeAdaptationHost.store(
                host_.get(),
                std::memory_order_release);
            orbit::post_process::
                SetHumanEyeAdaptationUpdateOverride(
                    &UpdateEyeAdaptationThroughHotReload);

            host_->Start();
        }
        catch (const std::exception& exception)
        {
            orbit::log::Error(
                std::string(
                    "Orbit hot-reload bootstrap failed: ") +
                exception.what());
        }
        catch (...)
        {
            orbit::log::Error(
                "Orbit hot-reload bootstrap failed with an unknown error.");
        }
    }

    ~StudioHotReloadBootstrap()
    {
        orbit::post_process::
            SetHumanEyeAdaptationUpdateOverride(
                nullptr);
        gEyeAdaptationHost.store(
            nullptr,
            std::memory_order_release);

        if (host_ != nullptr)
        {
            host_->Stop();
        }
    }

private:
    std::unique_ptr<
        orbit::hot_reload::HotReloadHost>
        host_;
};
} // namespace

int main(
    const int argc,
    char** argv)
{
    StudioHotReloadBootstrap hotReload;
    return OrbitStudioMain(
        argc,
        argv);
}
