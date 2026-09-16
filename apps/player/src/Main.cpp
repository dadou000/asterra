#include <orbit/core/BuildInfo.hpp>
#include <orbit/core/Log.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/platform/Window.hpp>
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
#include <vector>

namespace
{
[[nodiscard]] std::filesystem::path
ProjectInput(
    const int argc,
    char** argv)
{
    if (argc >= 2)
    {
        return std::filesystem::path(
            argv[1]);
    }

    return std::filesystem::current_path();
}
} // namespace

int main(
    const int argc,
    char** argv)
{
    try
    {
        const auto project =
            orbit::runtime_project::
                CookedProject::Open(
                    ProjectInput(
                        argc,
                        argv));

        orbit::documents::WorldDatabase
            world(
                project.StartupWorldPath());

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
