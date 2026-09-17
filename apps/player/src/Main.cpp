#include <orbit/core/BuildInfo.hpp>
#include <orbit/core/Log.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/platform/Window.hpp>
#include <orbit/platform_services/PlatformConfig.hpp>
#include <orbit/platform_services/PlatformServices.hpp>
#include <orbit/platform_services/steam/SteamProvider.hpp>
#if defined(ORBIT_HAS_STEAMWORKS)
#include <orbit/platform_services/steam/SteamworksBridge.hpp>
#endif
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/rhi/Fence.hpp>
#include <orbit/rhi/Queue.hpp>
#include <orbit/rhi/Resource.hpp>
#include <orbit/rhi/Swapchain.hpp>
#include <orbit/runtime/RuntimeSession.hpp>
#include <orbit/runtime_project/CookedProject.hpp>
#include <orbit/scene/ObjectStore.hpp>

#include <filesystem>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
struct Options
{
    std::filesystem::path projectInput;
    bool validateOnly{false};
};

[[nodiscard]] Options ParseOptions(
    const int argc,
    char** argv)
{
    Options options{
        .projectInput =
            std::filesystem::
                current_path()
    };

    bool projectSeen = false;

    for (int index = 1;
         index < argc;
         ++index)
    {
        const std::string_view argument =
            argv[index];

        if (argument == "--validate-only")
        {
            options.validateOnly = true;
            continue;
        }

        if (!argument.empty() &&
            argument.front() == '-')
        {
            throw std::invalid_argument(
                "Unknown OrbitPlayer option: " +
                std::string(argument));
        }

        if (projectSeen)
        {
            throw std::invalid_argument(
                "OrbitPlayer accepts only one cooked project path.");
        }

        options.projectInput =
            std::filesystem::path(
                argument);
        projectSeen = true;
    }

    return options;
}

struct PlatformRuntime
{
    std::unique_ptr<
        orbit::platform_services::
            PlatformServiceRegistry>
        registry;
    std::unique_ptr<
        orbit::platform_services::
            GameEventService>
        events;
};

[[nodiscard]] PlatformRuntime
CreatePlatformRuntime(
    const orbit::runtime_project::CookedProject& project)
{
    using namespace orbit::platform_services;

    PlatformConfiguration configuration;
    const auto configurationPath =
        project.RootDirectory() /
        "PlatformServices.toml";

    if (std::filesystem::is_regular_file(
            configurationPath))
    {
        configuration =
            LoadPlatformConfiguration(
                configurationPath);

        const auto issues =
            ValidatePlatformConfiguration(
                configuration,
                configuration.steam.enabled);

        if (!issues.empty())
        {
            throw std::runtime_error(
                "Packaged platform service configuration is invalid: " +
                issues.front().message);
        }
    }

    std::unique_ptr<IPlatformProvider>
        provider;

    if (configuration.steam.enabled)
    {
#if defined(ORBIT_HAS_STEAMWORKS)
        provider =
            std::make_unique<
                steam::SteamProvider>(
                    configuration.steam,
                    steam::
                        CreateSteamworksBridge());
#else
        throw std::runtime_error(
            "This package enables Steam, but OrbitPlayer was built without the Steamworks SDK.");
#endif
    }
    else
    {
        provider =
            std::make_unique<
                StandaloneProvider>();
    }

    auto registry =
        std::make_unique<
            PlatformServiceRegistry>(
                std::move(provider));

    std::vector<StatDefinition>
        stats;
    stats.reserve(
        configuration.steam.stats.size());

    for (const auto& mapping :
         configuration.steam.stats)
    {
        stats.push_back({
            .id = mapping.id,
            .kind = mapping.kind
        });
    }

    auto events =
        std::make_unique<
            GameEventService>(
                registry->Provider(),
                std::move(stats),
                configuration.eventRules);

    orbit::log::Info(
        std::format(
            "Platform provider: {}",
            registry->Provider().Name()));

    return {
        .registry = std::move(registry),
        .events = std::move(events)
    };
}
} // namespace

int main(
    const int argc,
    char** argv)
{
    try
    {
        const Options options =
            ParseOptions(
                argc,
                argv);

        const auto project =
            orbit::runtime_project::
                CookedProject::Open(
                    options.
                        projectInput);

        PlatformRuntime platformRuntime =
            CreatePlatformRuntime(
                project);

        orbit::documents::WorldDatabase
            world(
                project.StartupWorldPath(),
                orbit::documents::
                    WorldOpenMode::ReadOnly);

        orbit::scene::ObjectStore objects(
            world);

        const auto roots =
            objects.Roots();

        orbit::log::Info(
            std::format(
                "OrbitPlayer loading '{}' | project {} | world {} | {} root object{} | {}",
                project.Manifest().
                    projectName,
                project.Manifest().
                    projectId.ToString(),
                world.Id().ToString(),
                roots.size(),
                roots.size() == 1U
                    ? ""
                    : "s",
                orbit::build::
                    DisplayVersion));

        orbit::runtime_project::
            ScriptRuntime scriptRuntime(
                project);

        scriptRuntime.ExecuteEntryPoints();

        if (options.validateOnly)
        {
            orbit::log::Info(
                "Cooked project validation completed without creating a graphics device.");
            return 0;
        }

        const std::string title =
            project.Manifest().
                projectName +
            " - Orbit";

        orbit::runtime::RuntimeSession
            runtime({
                .applicationName =
                    project.Manifest().
                        projectName,
                .windowTitle = title,
                .width = 1600,
                .height = 900,
                .swapchainBufferCount = 3,
                .allowTearing = true,
                .relativeMouseMode = false
            });

        auto& window =
            runtime.Window();
        auto& device =
            runtime.Device();
        auto& graphicsQueue =
            runtime.GraphicsQueue();
        auto& swapchain =
            runtime.Swapchain();

        std::vector<
            std::unique_ptr<
                orbit::rhi::
                    CommandAllocator>>
            allocators;

        allocators.reserve(
            swapchain.BufferCount());

        for (orbit::u32 index = 0;
             index <
                 swapchain.BufferCount();
             ++index)
        {
            allocators.push_back(
                device.
                    CreateCommandAllocator(
                        orbit::rhi::
                            QueueType::
                                Graphics));
        }

        auto commandList =
            device.CreateCommandList(
                *allocators.front());

        auto frameFence =
            device.CreateFence(0);

        std::vector<orbit::u64>
            fenceValues(
                swapchain.
                    BufferCount(),
                0);

        orbit::u64 nextFenceValue = 1;

        while (window.PumpEvents())
        {
            if (window.KeyDown(
                    orbit::platform::
                        Key::Escape))
            {
                break;
            }

            if (window.Width() == 0 ||
                window.Height() == 0)
            {
                continue;
            }

            static_cast<void>(
                runtime.
                    ResizeSwapchainToWindow());

            const orbit::u32 frameIndex =
                swapchain.
                    CurrentBackBufferIndex();

            if (fenceValues[frameIndex] !=
                0)
            {
                frameFence->Wait(
                    fenceValues[
                        frameIndex]);
            }

            auto& allocator =
                *allocators[
                    frameIndex];

            allocator.Reset();
            commandList->Reset(
                allocator);

            auto& backBuffer =
                swapchain.
                    CurrentBackBuffer();

            commandList->Transition(
                backBuffer,
                orbit::rhi::
                    ResourceState::
                        Present,
                orbit::rhi::
                    ResourceState::
                        RenderTarget);

            commandList->ClearColorTarget(
                backBuffer,
                {
                    .red = 0.008F,
                    .green = 0.012F,
                    .blue = 0.020F,
                    .alpha = 1.0F
                });

            commandList->Transition(
                backBuffer,
                orbit::rhi::
                    ResourceState::
                        RenderTarget,
                orbit::rhi::
                    ResourceState::
                        Present);

            commandList->Close();

            graphicsQueue.Submit(
                *commandList);

            swapchain.Present(
                true);

            const orbit::u64 signalValue =
                nextFenceValue++;

            graphicsQueue.Signal(
                *frameFence,
                signalValue);

            fenceValues[frameIndex] =
                signalValue;
        }

        if (nextFenceValue > 1)
        {
            frameFence->Wait(
                nextFenceValue - 1);
        }

        static_cast<void>(
            platformRuntime.
                registry->
                Provider().
                Flush());

        return 0;
    }
    catch (const std::exception&
               exception)
    {
        orbit::log::Error(
            exception.what());
        return 1;
    }
}
