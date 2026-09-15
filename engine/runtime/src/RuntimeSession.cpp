#include <orbit/runtime/RuntimeSession.hpp>

#include <orbit/core/Log.hpp>
#include <orbit/platform/CrashHandler.hpp>
#include <orbit/platform/Window.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/rhi/Queue.hpp>
#include <orbit/rhi/Swapchain.hpp>
#include <orbit/rhi/vulkan/VulkanBackend.hpp>

#include <cstdlib>

namespace orbit::runtime
{
namespace
{
[[nodiscard]] bool EnvironmentFlagEnabled(const char* name) noexcept
{
    char* value = nullptr;
    std::size_t valueLength = 0;

    const bool found =
        _dupenv_s(
            &value,
            &valueLength,
            name) == 0 &&
        value != nullptr &&
        valueLength > 1;

    std::free(value);
    return found;
}
} // namespace

RuntimeSession::RuntimeSession(const RuntimeSessionDesc& desc)
{
    const bool crashHandlerInstalled =
        platform::InstallCrashHandler({
            .applicationName = desc.applicationName,
            .writeMiniDump = true
        });

    if (!crashHandlerInstalled)
    {
        log::Warning(
            "Orbit crash handler could not be installed.");
    }

    window_ = platform::MakeWindow({
        .title = desc.windowTitle,
        .width = desc.width,
        .height = desc.height
    });

    window_->SetRelativeMouseMode(
        desc.relativeMouseMode);

#if defined(NDEBUG)
    constexpr bool enableDefaultValidation = false;
#else
    constexpr bool enableDefaultValidation = true;
#endif

    const bool enableBestPractices =
        EnvironmentFlagEnabled(
            "ORBIT_VK_BEST_PRACTICES");
    const bool enableSyncValidation =
        EnvironmentFlagEnabled(
            "ORBIT_VK_SYNC_VALIDATION");
    const bool enableGpuAssisted =
        EnvironmentFlagEnabled(
            "ORBIT_VK_GPU_ASSISTED");
    const bool enableRenderDoc =
        EnvironmentFlagEnabled(
            "ORBIT_RENDERDOC");

    device_ =
        rhi::vulkan::CreateDevice({
            .enableValidation =
                enableDefaultValidation ||
                enableBestPractices ||
                enableSyncValidation ||
                enableGpuAssisted,
            .enableBestPracticesValidation =
                enableBestPractices,
            .enableSynchronizationValidation =
                enableSyncValidation,
            .enableGpuAssistedValidation =
                enableGpuAssisted,
            .enableRenderDoc =
                enableRenderDoc
        });

    if (enableRenderDoc)
    {
        rhi::vulkan::SetRenderDocActiveWindow(
            *device_,
            window_->NativeHandle());
    }

    graphicsQueue_ =
        device_->CreateQueue(
            rhi::QueueType::Graphics);

    swapchain_ =
        device_->CreateSwapchain(
            *graphicsQueue_,
            {
                .nativeWindow =
                    window_->NativeHandle(),
                .width = window_->Width(),
                .height = window_->Height(),
                .bufferCount =
                    desc.swapchainBufferCount,
                .allowTearing =
                    desc.allowTearing
            });
}

RuntimeSession::~RuntimeSession() = default;

platform::Window& RuntimeSession::Window() noexcept
{
    return *window_;
}

const platform::Window& RuntimeSession::Window() const noexcept
{
    return *window_;
}

rhi::Device& RuntimeSession::Device() noexcept
{
    return *device_;
}

const rhi::Device& RuntimeSession::Device() const noexcept
{
    return *device_;
}

rhi::Queue& RuntimeSession::GraphicsQueue() noexcept
{
    return *graphicsQueue_;
}

const rhi::Queue& RuntimeSession::GraphicsQueue() const noexcept
{
    return *graphicsQueue_;
}

rhi::Swapchain& RuntimeSession::Swapchain() noexcept
{
    return *swapchain_;
}

const rhi::Swapchain& RuntimeSession::Swapchain() const noexcept
{
    return *swapchain_;
}

bool RuntimeSession::ResizeSwapchainToWindow()
{
    const u32 width = window_->Width();
    const u32 height = window_->Height();

    if (width == 0 || height == 0)
    {
        return false;
    }

    if (width == swapchain_->Width() &&
        height == swapchain_->Height())
    {
        return false;
    }

    swapchain_->Resize(width, height);
    return true;
}
} // namespace orbit::runtime
