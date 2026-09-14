#include "VulkanObjects.hpp"

#include <stdexcept>

namespace orbit::rhi::vulkan::detail
{
VulkanFence::VulkanFence(
    const VkDevice device, const VkSemaphore timelineSemaphore)
    : device_(device), semaphore_(timelineSemaphore)
{
}

VulkanFence::~VulkanFence()
{
    if (semaphore_ != VK_NULL_HANDLE)
    {
        vkDestroySemaphore(device_, semaphore_, nullptr);
    }
}

u64 VulkanFence::CompletedValue() const noexcept
{
    u64 value = 0;
    vkGetSemaphoreCounterValue(device_, semaphore_, &value);
    return value;
}

void VulkanFence::Wait(const u64 value)
{
    if (CompletedValue() >= value)
    {
        return;
    }

    VkSemaphoreWaitInfo waitInfo{};
    waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    waitInfo.semaphoreCount = 1;
    waitInfo.pSemaphores = &semaphore_;
    waitInfo.pValues = &value;

    if (vkWaitSemaphores(device_, &waitInfo, UINT64_MAX) != VK_SUCCESS)
    {
        throw std::runtime_error(
            "Orbit failed while waiting for a Vulkan timeline "
            "semaphore.");
    }
}

VkSemaphore VulkanFence::Native() const noexcept
{
    return semaphore_;
}

VulkanQueue::VulkanQueue(
    const VkDevice device,
    const VkQueue nativeQueue,
    const u32 familyIndex,
    const QueueType type,
    const DeviceFunctions& functions)
    : device_(device),
      nativeQueue_(nativeQueue),
      familyIndex_(familyIndex),
      type_(type),
      functions_(&functions)
{
}

QueueType VulkanQueue::Type() const noexcept
{
    return type_;
}

VkQueue VulkanQueue::Native() const noexcept
{
    return nativeQueue_;
}

u32 VulkanQueue::FamilyIndex() const noexcept
{
    return familyIndex_;
}

const DeviceFunctions& VulkanQueue::Functions() const noexcept
{
    return *functions_;
}

void VulkanQueue::SetPendingSwapchainSync(
    const VkSemaphore waitForImageAvailable,
    const VkSemaphore signalRenderFinished,
    const VkFence imageAvailableRetired)
{
    pendingImageAvailable_ = waitForImageAvailable;
    pendingRenderFinished_ = signalRenderFinished;
    pendingImageAvailableRetired_ = imageAvailableRetired;
}

void VulkanQueue::Submit(CommandList& commandList)
{
    auto* vulkanCommandList =
        dynamic_cast<VulkanCommandList*>(&commandList);

    if (vulkanCommandList == nullptr ||
        vulkanCommandList->Type() != type_)
    {
        throw std::runtime_error(
            "Orbit cannot submit a command list to an incompatible "
            "queue.");
    }

    VkCommandBufferSubmitInfo commandBufferInfo{};
    commandBufferInfo.sType =
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    commandBufferInfo.commandBuffer = vulkanCommandList->Native();

    VkSemaphoreSubmitInfo waitInfo{};
    waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waitInfo.semaphore = pendingImageAvailable_;
    waitInfo.stageMask =
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSemaphoreSubmitInfo signalInfo{};
    signalInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signalInfo.semaphore = pendingRenderFinished_;
    signalInfo.stageMask =
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSubmitInfo2 submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos = &commandBufferInfo;

    if (pendingImageAvailable_ != VK_NULL_HANDLE)
    {
        submitInfo.waitSemaphoreInfoCount = 1;
        submitInfo.pWaitSemaphoreInfos = &waitInfo;
    }

    if (pendingRenderFinished_ != VK_NULL_HANDLE)
    {
        submitInfo.signalSemaphoreInfoCount = 1;
        submitInfo.pSignalSemaphoreInfos = &signalInfo;
    }

    // Piggyback the "image-available semaphore is now safe to reuse
    // for a future acquire" fence onto this same submission (its
    // completion implies the semaphore's wait operation has retired)
    // instead of a separate no-op submit -- see AcquireIfNeeded's
    // wait on it in VulkanSwapchain.cpp for why this exists: without
    // it, nothing stopped the CPU from racing ahead of the GPU and
    // reusing that semaphore for a new acquire before its previous
    // wait had actually completed, which is undefined behavior Vulkan
    // validation flags but real drivers are free to corrupt state
    // over rather than merely warn about.
    if (vkQueueSubmit2(
            nativeQueue_,
            1,
            &submitInfo,
            pendingImageAvailableRetired_) != VK_SUCCESS)
    {
        throw std::runtime_error(
            "Orbit failed to submit a Vulkan command buffer.");
    }

    pendingImageAvailable_ = VK_NULL_HANDLE;
    pendingRenderFinished_ = VK_NULL_HANDLE;
    pendingImageAvailableRetired_ = VK_NULL_HANDLE;
}

void VulkanQueue::Signal(Fence& fence, const u64 value)
{
    auto* vulkanFence = dynamic_cast<VulkanFence*>(&fence);
    if (vulkanFence == nullptr)
    {
        throw std::runtime_error(
            "Orbit Vulkan received a fence from another backend.");
    }

    VkSemaphoreSubmitInfo signalInfo{};
    signalInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signalInfo.semaphore = vulkanFence->Native();
    signalInfo.value = value;
    signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkSubmitInfo2 submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submitInfo.signalSemaphoreInfoCount = 1;
    submitInfo.pSignalSemaphoreInfos = &signalInfo;

    // Sequential submits to the same VkQueue execute in submission
    // order (Vulkan guarantees no reordering within a single queue),
    // so this pure-signal submit only completes once every command
    // buffer submitted before it -- including the one from Submit()
    // above -- has finished on the GPU. Mirrors ID3D12CommandQueue::
    // Signal's ordering guarantee exactly.
    if (vkQueueSubmit2(nativeQueue_, 1, &submitInfo, VK_NULL_HANDLE) !=
        VK_SUCCESS)
    {
        throw std::runtime_error(
            "Orbit failed to signal a Vulkan timeline semaphore.");
    }
}
} // namespace orbit::rhi::vulkan::detail
