#include "VulkanObjects.hpp"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace orbit::rhi::vulkan::detail
{
namespace
{
[[nodiscard]] VkSurfaceFormatKHR ChooseSurfaceFormat(
    const std::vector<VkSurfaceFormatKHR>& formats)
{
    for (const auto& candidate : formats)
    {
        if (candidate.format == VK_FORMAT_R8G8B8A8_UNORM &&
            candidate.colorSpace ==
                VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            return candidate;
        }
    }

    return formats.front();
}

[[nodiscard]] VkSemaphore CreateBinarySemaphore(const VkDevice device)
{
    VkSemaphoreCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkSemaphore semaphore = VK_NULL_HANDLE;
    if (vkCreateSemaphore(
            device, &createInfo, nullptr, &semaphore) != VK_SUCCESS)
    {
        throw std::runtime_error(
            "Orbit failed to create a Vulkan swapchain semaphore.");
    }

    return semaphore;
}

[[nodiscard]] VkFence CreateSignaledFence(const VkDevice device)
{
    VkFenceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    // Signaled so the first-ever acquire of each slot doesn't wait on
    // a submission that never happened.
    createInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    VkFence fence = VK_NULL_HANDLE;
    if (vkCreateFence(device, &createInfo, nullptr, &fence) !=
        VK_SUCCESS)
    {
        throw std::runtime_error(
            "Orbit failed to create a Vulkan swapchain fence.");
    }

    return fence;
}
} // namespace

VulkanSwapchain::VulkanSwapchain(
    const VkInstance instance,
    const VkPhysicalDevice physicalDevice,
    const VkDevice device,
    const VkSurfaceKHR surface,
    const VkSwapchainKHR nativeSwapchain,
    VulkanQueue& presentQueue,
    const SwapchainDesc& desc,
    const VkFormat format,
    const VkColorSpaceKHR colorSpace)
    : instance_(instance),
      physicalDevice_(physicalDevice),
      device_(device),
      surface_(surface),
      nativeSwapchain_(nativeSwapchain),
      format_(format),
      colorSpace_(colorSpace),
      presentQueue_(&presentQueue),
      width_(desc.width),
      height_(desc.height)
{
    CreatePerImageResources();
}

void VulkanSwapchain::CreatePerImageResources()
{
    u32 imageCount = 0;
    vkGetSwapchainImagesKHR(
        device_, nativeSwapchain_, &imageCount, nullptr);

    std::vector<VkImage> images(imageCount);
    vkGetSwapchainImagesKHR(
        device_, nativeSwapchain_, &imageCount, images.data());

    bufferCount_ = imageCount;

    backBuffers_.reserve(imageCount);
    imageAvailableSemaphores_.reserve(imageCount);
    renderFinishedSemaphores_.reserve(imageCount);
    imageAvailableFences_.reserve(imageCount);

    for (const VkImage image : images)
    {
        VkImageViewCreateInfo viewCreateInfo{};
        viewCreateInfo.sType =
            VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewCreateInfo.image = image;
        viewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewCreateInfo.format = format_;
        viewCreateInfo.subresourceRange = {
            VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        VkImageView view = VK_NULL_HANDLE;
        if (vkCreateImageView(
                device_, &viewCreateInfo, nullptr, &view) !=
            VK_SUCCESS)
        {
            throw std::runtime_error(
                "Orbit failed to create a Vulkan swapchain image "
                "view.");
        }

        // Unlike a regular CreateTexture() result, a swapchain image
        // cannot be pre-warmed into its claimed initial layout here --
        // it is owned by the presentation engine until actually
        // acquired, and any use before that (even a layout-transition
        // barrier) is a validation error. VulkanTexture's everUsed_
        // flag (false for a swapchain image; see its constructor)
        // instead makes the first real Transition() call in
        // VulkanCommands.cpp treat this image as coming from
        // UNDEFINED, matching reality.
        backBuffers_.push_back(
            std::make_unique<VulkanTexture>(
                device_,
                nullptr,
                image,
                nullptr,
                view,
                width_,
                height_,
                TextureFormat::RGBA8_UNorm,
                false));

        imageAvailableSemaphores_.push_back(
            CreateBinarySemaphore(device_));
        renderFinishedSemaphores_.push_back(
            CreateBinarySemaphore(device_));
        imageAvailableFences_.push_back(
            CreateSignaledFence(device_));
    }
}

void VulkanSwapchain::DestroyPerImageResources()
{
    backBuffers_.clear();

    for (const VkSemaphore semaphore : imageAvailableSemaphores_)
    {
        vkDestroySemaphore(device_, semaphore, nullptr);
    }

    for (const VkSemaphore semaphore : renderFinishedSemaphores_)
    {
        vkDestroySemaphore(device_, semaphore, nullptr);
    }

    for (const VkFence fence : imageAvailableFences_)
    {
        vkDestroyFence(device_, fence, nullptr);
    }

    imageAvailableSemaphores_.clear();
    renderFinishedSemaphores_.clear();
    imageAvailableFences_.clear();

    currentImageIndex_ = 0;
    acquired_ = false;
    frameSlot_ = 0;
}

VulkanSwapchain::~VulkanSwapchain()
{
    // VulkanDevice::~VulkanDevice also waits for the device to go
    // idle, but Main.cpp (like any normal C++ destruction order)
    // destroys the swapchain *before* the device that owns it, so
    // that later wait cannot protect the semaphores/swapchain this
    // destructor is about to destroy -- without waiting here first,
    // tearing down while a present or submission is still in flight
    // (e.g. on an error/exception unwind mid-frame) hits "in use"
    // validation errors destroying each of them.
    vkDeviceWaitIdle(device_);

    DestroyPerImageResources();

    if (nativeSwapchain_ != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(device_, nativeSwapchain_, nullptr);
    }

    if (surface_ != VK_NULL_HANDLE)
    {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
    }
}

void VulkanSwapchain::Resize(const u32 width, const u32 height)
{
    // Minimized (or a spurious zero-size event) -- nothing to
    // recreate against; the caller (Main.cpp) is expected to skip
    // rendering entirely while the window reports a zero size rather
    // than call this.
    if (width == 0 || height == 0)
    {
        return;
    }

    if (width == width_ && height == height_)
    {
        return;
    }

    // Recreating in place while a previous frame's submission or
    // present could still be using the old images/semaphores would
    // hit "in use" validation errors, same reasoning as the
    // destructor.
    vkDeviceWaitIdle(device_);

    DestroyPerImageResources();

    VkSurfaceCapabilitiesKHR capabilities{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        physicalDevice_, surface_, &capabilities);

    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == 0xFFFFFFFFU)
    {
        extent.width = std::clamp(
            width,
            capabilities.minImageExtent.width,
            capabilities.maxImageExtent.width);
        extent.height = std::clamp(
            height,
            capabilities.minImageExtent.height,
            capabilities.maxImageExtent.height);
    }

    u32 imageCount =
        std::max(bufferCount_, capabilities.minImageCount);

    if (capabilities.maxImageCount > 0)
    {
        imageCount =
            std::min(imageCount, capabilities.maxImageCount);
    }

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = surface_;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = format_;
    createInfo.imageColorSpace = colorSpace_;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.preTransform = capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = nativeSwapchain_;

    VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
    const VkResult createResult = vkCreateSwapchainKHR(
        device_, &createInfo, nullptr, &newSwapchain);

    // The old swapchain must be destroyed once it is no longer in use
    // by the new one regardless of whether creation succeeded (the
    // spec requires retiring it either way).
    vkDestroySwapchainKHR(device_, nativeSwapchain_, nullptr);
    nativeSwapchain_ = VK_NULL_HANDLE;

    if (createResult != VK_SUCCESS)
    {
        throw std::runtime_error(
            "Orbit failed to recreate the Vulkan swapchain.");
    }

    nativeSwapchain_ = newSwapchain;
    width_ = extent.width;
    height_ = extent.height;

    CreatePerImageResources();
}

void VulkanSwapchain::AcquireIfNeeded() const
{
    if (acquired_)
    {
        return;
    }

    const VkSemaphore imageAvailable =
        imageAvailableSemaphores_[frameSlot_];
    const VkFence imageAvailableFence =
        imageAvailableFences_[frameSlot_];

    // Nothing else throttles how far the CPU can race ahead of the
    // GPU with respect to *this specific semaphore*: Main.cpp's own
    // fence-wait (guarding allocator/upload-buffer reuse) happens
    // against a completely different timeline fence, and only after
    // this acquire has already run. Without waiting for the
    // submission that last consumed this semaphore slot to actually
    // finish, a fast-enough CPU can reacquire with it before that
    // wait has retired -- Vulkan validation calls this out as
    // undefined behavior, and unlike most UB it's cheap to just not
    // risk it.
    vkWaitForFences(
        device_, 1, &imageAvailableFence, VK_TRUE, UINT64_MAX);
    vkResetFences(device_, 1, &imageAvailableFence);

    const VkResult result = vkAcquireNextImageKHR(
        device_,
        nativeSwapchain_,
        UINT64_MAX,
        imageAvailable,
        VK_NULL_HANDLE,
        &currentImageIndex_);

    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
    {
        throw std::runtime_error(
            "Orbit failed to acquire a Vulkan swapchain image.");
    }

    // The image-available semaphore/fence pair is correctly indexed
    // by a rotating frame-in-flight slot (chosen before the image
    // index is even known), but the render-finished/present-wait
    // semaphore must be indexed by the *acquired image index* -- the
    // presentation engine's consumption of it is tied to that
    // specific image, and reusing one indexed by frame slot instead
    // can hand vkQueuePresentKHR a semaphore a previous, still-
    // in-flight present of a different image hasn't finished with
    // yet (this is the standard swapchain-semaphore-reuse hazard).
    presentQueue_->SetPendingSwapchainSync(
        imageAvailable,
        renderFinishedSemaphores_[currentImageIndex_],
        imageAvailableFence);

    acquired_ = true;
}

void VulkanSwapchain::Present(const bool /*verticalSync*/)
{
    // The swapchain is created with VK_PRESENT_MODE_FIFO_KHR (see
    // VulkanDevice::CreateSwapchain) and Main.cpp only ever calls
    // Present(true) today, so there is no live "switch to immediate
    // mode" path to serve -- unlike D3D12's per-Present sync-interval
    // flag, Vulkan's present mode is fixed at swapchain creation.
    AcquireIfNeeded();

    const VkSemaphore renderFinished =
        renderFinishedSemaphores_[currentImageIndex_];

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &renderFinished;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &nativeSwapchain_;
    presentInfo.pImageIndices = &currentImageIndex_;

    const VkResult result =
        vkQueuePresentKHR(presentQueue_->Native(), &presentInfo);

    // OUT_OF_DATE here just means the window was resized since this
    // image was acquired -- not a real failure. Main.cpp checks the
    // window's size against Width()/Height() at the start of every
    // frame and calls Resize() before the *next* acquire, so this
    // image simply goes unused rather than crashing the app.
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR &&
        result != VK_ERROR_OUT_OF_DATE_KHR)
    {
        throw std::runtime_error(
            "Orbit failed to present the Vulkan swapchain.");
    }

    acquired_ = false;
    frameSlot_ = (frameSlot_ + 1) % bufferCount_;
}

u32 VulkanSwapchain::Width() const noexcept
{
    return width_;
}

u32 VulkanSwapchain::Height() const noexcept
{
    return height_;
}

u32 VulkanSwapchain::BufferCount() const noexcept
{
    return bufferCount_;
}

u32 VulkanSwapchain::CurrentBackBufferIndex() const noexcept
{
    AcquireIfNeeded();
    return currentImageIndex_;
}

Texture& VulkanSwapchain::CurrentBackBuffer() noexcept
{
    AcquireIfNeeded();
    return *backBuffers_[currentImageIndex_];
}

std::unique_ptr<Swapchain> VulkanDevice::CreateSwapchain(
    Queue& queue,
    const SwapchainDesc& desc)
{
    if (desc.nativeWindow == nullptr)
    {
        throw std::runtime_error(
            "Orbit cannot create a swapchain without a native "
            "window.");
    }

    if (desc.width == 0 || desc.height == 0)
    {
        throw std::runtime_error(
            "Orbit cannot create a zero-sized swapchain.");
    }

    if (desc.bufferCount < 2)
    {
        throw std::runtime_error(
            "Orbit requires at least two swapchain buffers.");
    }

    auto* vulkanQueue = dynamic_cast<VulkanQueue*>(&queue);
    if (vulkanQueue == nullptr ||
        vulkanQueue->Type() != QueueType::Graphics)
    {
        throw std::runtime_error(
            "A Vulkan swapchain requires an Orbit Vulkan graphics "
            "queue.");
    }

    VkWin32SurfaceCreateInfoKHR surfaceCreateInfo{};
    surfaceCreateInfo.sType =
        VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    surfaceCreateInfo.hinstance = GetModuleHandleW(nullptr);
    surfaceCreateInfo.hwnd =
        static_cast<HWND>(desc.nativeWindow);

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (vkCreateWin32SurfaceKHR(
            instance_, &surfaceCreateInfo, nullptr, &surface) !=
        VK_SUCCESS)
    {
        throw std::runtime_error(
            "Orbit failed to create a Vulkan Win32 surface.");
    }

    VkSurfaceCapabilitiesKHR capabilities{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        physicalDevice_, surface, &capabilities);

    u32 formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(
        physicalDevice_, surface, &formatCount, nullptr);

    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(
        physicalDevice_, surface, &formatCount, formats.data());

    if (formats.empty())
    {
        vkDestroySurfaceKHR(instance_, surface, nullptr);
        throw std::runtime_error(
            "Orbit found no surface formats for the Vulkan "
            "swapchain.");
    }

    const VkSurfaceFormatKHR chosenFormat =
        ChooseSurfaceFormat(formats);

    u32 imageCount = desc.bufferCount;
    imageCount =
        std::max(imageCount, capabilities.minImageCount);

    if (capabilities.maxImageCount > 0)
    {
        imageCount =
            std::min(imageCount, capabilities.maxImageCount);
    }

    VkSwapchainCreateInfoKHR swapchainCreateInfo{};
    swapchainCreateInfo.sType =
        VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchainCreateInfo.surface = surface;
    swapchainCreateInfo.minImageCount = imageCount;
    swapchainCreateInfo.imageFormat = chosenFormat.format;
    swapchainCreateInfo.imageColorSpace = chosenFormat.colorSpace;
    swapchainCreateInfo.imageExtent = {desc.width, desc.height};
    swapchainCreateInfo.imageArrayLayers = 1;
    swapchainCreateInfo.imageUsage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swapchainCreateInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchainCreateInfo.preTransform =
        capabilities.currentTransform;
    swapchainCreateInfo.compositeAlpha =
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    // FIFO is the only present mode guaranteed by the spec, and the
    // only one this application actually drives -- see the note in
    // VulkanSwapchain::Present above.
    swapchainCreateInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapchainCreateInfo.clipped = VK_TRUE;

    VkSwapchainKHR nativeSwapchain = VK_NULL_HANDLE;
    if (vkCreateSwapchainKHR(
            nativeDevice_,
            &swapchainCreateInfo,
            nullptr,
            &nativeSwapchain) != VK_SUCCESS)
    {
        vkDestroySurfaceKHR(instance_, surface, nullptr);
        throw std::runtime_error(
            "Orbit failed to create the Vulkan swapchain.");
    }

    return std::make_unique<VulkanSwapchain>(
        instance_,
        physicalDevice_,
        nativeDevice_,
        surface,
        nativeSwapchain,
        *vulkanQueue,
        desc,
        chosenFormat.format,
        chosenFormat.colorSpace);
}
} // namespace orbit::rhi::vulkan::detail
