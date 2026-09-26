#include <orbit/hot_reload/HotIterationService.hpp>
#include <orbit/hot_reload/HotReloadHost.hpp>
#include <orbit/post_process/HumanEyeAdaptation.hpp>
#include <orbit/post_process/HumanEyeAdaptationHotReload.hpp>

#include <orbit/core/Log.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifndef ORBIT_SOURCE_ROOT
#error ORBIT_SOURCE_ROOT must be defined for the Studio hot-reload bootstrap.
#endif

#ifndef ORBIT_BINARY_ROOT
#error ORBIT_BINARY_ROOT must be defined for the Studio hot-reload bootstrap.
#endif

#ifndef ORBIT_BUILD_CONFIG
#error ORBIT_BUILD_CONFIG must be defined for the Studio hot-reload bootstrap.
#endif

#ifndef ORBIT_STUDIO_DEVELOPMENT_BINARY
#error ORBIT_STUDIO_DEVELOPMENT_BINARY must be defined for hot iteration.
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
constexpr std::string_view kWaitForPreviousProcessArgument =
    "--orbit-hot-wait-pid";

std::atomic<orbit::hot_reload::HotReloadHost*>
    gEyeAdaptationHost{nullptr};
std::atomic<orbit::hot_reload::HotIterationService*>
    gHotIterationService{nullptr};
UINT_PTR gHotIterationTimerId{0U};

void CALLBACK HotIterationTimerProc(
    HWND,
    UINT,
    UINT_PTR,
    DWORD) noexcept
{
    try
    {
        static_cast<void>(
            orbit::hot_reload::
                PumpHotIterationEvents());
    }
    catch (const std::exception& exception)
    {
        orbit::log::Error(
            std::string(
                "Hot iteration main-thread dispatch failed: ") +
            exception.what());
    }
    catch (...)
    {
        orbit::log::Error(
            "Hot iteration main-thread dispatch failed with an unknown error.");
    }
}

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

struct ForwardedArguments
{
    std::vector<std::string> storage;
    std::vector<char*> pointers;
};

[[nodiscard]] ForwardedArguments PrepareArguments(
    const int argc,
    char** argv)
{
    ForwardedArguments result;
    result.storage.reserve(
        static_cast<std::size_t>(
            std::max(argc, 0)));

    std::optional<DWORD> waitForProcess;

    for (int index = 0;
         index < argc;
         ++index)
    {
        const std::string_view argument =
            argv[index] != nullptr
                ? std::string_view(argv[index])
                : std::string_view{};

        if (index > 0 &&
            argument == kWaitForPreviousProcessArgument &&
            index + 1 < argc &&
            argv[index + 1] != nullptr)
        {
            const std::string_view value(
                argv[index + 1]);
            std::uint64_t parsed = 0U;
            const auto parse =
                std::from_chars(
                    value.data(),
                    value.data() + value.size(),
                    parsed);

            if (parse.ec == std::errc{} &&
                parse.ptr == value.data() + value.size() &&
                parsed <=
                    static_cast<std::uint64_t>(
                        (std::numeric_limits<DWORD>::max)()))
            {
                waitForProcess =
                    static_cast<DWORD>(parsed);
            }

            ++index;
            continue;
        }

        result.storage.emplace_back(
            argument);
    }

    if (waitForProcess.has_value() &&
        *waitForProcess != 0U &&
        *waitForProcess != GetCurrentProcessId())
    {
        const HANDLE previous =
            OpenProcess(
                SYNCHRONIZE,
                FALSE,
                *waitForProcess);

        if (previous != nullptr)
        {
            (void)WaitForSingleObject(
                previous,
                INFINITE);
            CloseHandle(previous);
        }
    }

    result.pointers.reserve(
        result.storage.size() + 1U);
    for (auto& argument : result.storage)
    {
        result.pointers.push_back(
            argument.data());
    }
    result.pointers.push_back(nullptr);
    return result;
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
    explicit StudioHotReloadBootstrap(
        std::vector<std::string> relaunchArguments) noexcept
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

            // Hot iteration belongs to the root development Orbit.exe,
            // build-tree Studio, and generation copies staged under the build
            // tree. Packaged executables remain self-contained.
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
                    "Orbit hot iteration is disabled because the development CMake tree is missing.");
                return;
            }

            const auto probeRoot =
                canonicalSource /
                "engine" /
                "hot_reload_probe";
            const auto eyeSource =
                canonicalSource /
                "engine" /
                "post_process" /
                "src" /
                "HumanEyeAdaptation.cpp";
            const auto eyeHeader =
                canonicalSource /
                "engine" /
                "post_process" /
                "include" /
                "orbit" /
                "post_process" /
                "HumanEyeAdaptation.hpp";
            const auto eyeInterface =
                canonicalSource /
                "engine" /
                "post_process" /
                "include" /
                "orbit" /
                "post_process" /
                "HumanEyeAdaptationHotReload.hpp";
            const auto eyeModuleRoot =
                canonicalSource /
                "engine" /
                "post_process_hot_reload";

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
                    probeRoot
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
                    eyeSource,
                    eyeHeader,
                    eyeInterface,
                    eyeModuleRoot
                },
                .debounce =
                    std::chrono::milliseconds(250)
            });

            orbit::hot_reload::HotIterationConfig
                iterationConfig{
                    .sourceRoot = canonicalSource,
                    .binaryRoot = canonicalBinary,
                    .developmentBinary =
                        std::filesystem::path(
                            ORBIT_STUDIO_DEVELOPMENT_BINARY),
                    .runtimeRoot =
                        canonicalBinary /
                        "hot_reload_runtime",
                    .buildConfiguration =
                        ORBIT_BUILD_CONFIG,
                    .fallbackBuildTarget =
                        "OrbitStudio",
                    .pollInterval =
                        std::chrono::milliseconds(50),
                    .fallbackDebounce =
                        std::chrono::milliseconds(300),
                    .automaticFallbackRelaunch = true,
                    .relaunchArguments =
                        std::move(relaunchArguments)
                };

            iteration_ =
                std::make_unique<
                    orbit::hot_reload::HotIterationService>(
                        std::move(iterationConfig));

            iteration_->AddNativeHotRoot(
                probeRoot);
            iteration_->AddNativeHotRoot(
                eyeSource);
            iteration_->AddNativeHotRoot(
                eyeHeader);
            iteration_->AddNativeHotRoot(
                eyeInterface);
            iteration_->AddNativeHotRoot(
                eyeModuleRoot);

            gEyeAdaptationHost.store(
                host_.get(),
                std::memory_order_release);
            gHotIterationService.store(
                iteration_.get(),
                std::memory_order_release);
            orbit::hot_reload::
                SetActiveHotIterationService(
                    iteration_.get());

            orbit::post_process::
                SetHumanEyeAdaptationUpdateOverride(
                    &UpdateEyeAdaptationThroughHotReload);

            host_->Start();
            iteration_->Start();

            gHotIterationTimerId =
                SetTimer(
                    nullptr,
                    0U,
                    50U,
                    &HotIterationTimerProc);

            if (gHotIterationTimerId == 0U)
            {
                orbit::log::Warning(
                    "Orbit hot iteration could not install the UI-thread event pump; native hot modules remain active.");
            }
        }
        catch (const std::exception& exception)
        {
            orbit::log::Error(
                std::string(
                    "Orbit hot-iteration bootstrap failed: ") +
                exception.what());
        }
        catch (...)
        {
            orbit::log::Error(
                "Orbit hot-iteration bootstrap failed with an unknown error.");
        }
    }

    ~StudioHotReloadBootstrap()
    {
        if (gHotIterationTimerId != 0U)
        {
            (void)KillTimer(
                nullptr,
                gHotIterationTimerId);
            gHotIterationTimerId = 0U;
        }

        orbit::hot_reload::
            SetActiveHotIterationService(
                nullptr);
        gHotIterationService.store(
            nullptr,
            std::memory_order_release);

        if (iteration_ != nullptr)
        {
            iteration_->Stop();
        }

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
    std::unique_ptr<
        orbit::hot_reload::HotIterationService>
        iteration_;
};
} // namespace

namespace orbit::editor_app
{
// Main/editor services can register the active game project once it is open so
// assets and scripts outside the Orbit repository enter the same save-to-reflect
// pipeline. The function is intentionally a no-op in packaged builds.
void RegisterHotIterationWatchRoot(
    const std::filesystem::path& root)
{
    orbit::hot_reload::
        AddHotIterationWatchRoot(root);
}
} // namespace orbit::editor_app

int main(
    const int argc,
    char** argv)
{
    auto forwarded =
        PrepareArguments(
            argc,
            argv);

    std::vector<std::string>
        relaunchArguments;
    if (forwarded.storage.size() > 1U)
    {
        relaunchArguments.assign(
            forwarded.storage.begin() + 1,
            forwarded.storage.end());
    }

    StudioHotReloadBootstrap hotReload(
        std::move(relaunchArguments));

    return OrbitStudioMain(
        static_cast<int>(
            forwarded.storage.size()),
        forwarded.pointers.data());
}
