#include "VulkanGpuProgress.hpp"

#include <orbit/profiler/Profiler.hpp>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace orbit::rhi::vulkan::detail
{
GpuProgress::GpuProgress(const VkDevice device) : device_(device)
{
    VkSemaphoreTypeCreateInfo typeCreateInfo{};
    typeCreateInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    typeCreateInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    typeCreateInfo.initialValue = 0;

    VkSemaphoreCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    createInfo.pNext = &typeCreateInfo;

    if (vkCreateSemaphore(device_, &createInfo, nullptr, &timeline_) !=
        VK_SUCCESS)
    {
        throw std::runtime_error(
            "Orbit failed to create the Vulkan submit-progress timeline.");
    }
}

GpuProgress::~GpuProgress()
{
    if (timeline_ != VK_NULL_HANDLE)
    {
        vkDestroySemaphore(device_, timeline_, nullptr);
    }
}

VkSemaphore GpuProgress::Timeline() const noexcept
{
    return timeline_;
}

u64 GpuProgress::BeginSubmit() noexcept
{
    return submitted_.fetch_add(1, std::memory_order_acq_rel) + 1U;
}

u64 GpuProgress::NextSerial() const noexcept
{
    return submitted_.load(std::memory_order_acquire) + 1U;
}

void GpuProgress::RefreshCompleted() noexcept
{
    u64 value = 0;
    if (vkGetSemaphoreCounterValue(device_, timeline_, &value) ==
        VK_SUCCESS)
    {
        RaiseTo(completed_, value);
    }
}

bool GpuProgress::IsComplete(const u64 serial) noexcept
{
    if (serial == 0U ||
        serial <= completed_.load(std::memory_order_acquire))
    {
        return true;
    }

    RefreshCompleted();
    return serial <= completed_.load(std::memory_order_acquire);
}

void GpuProgress::Wait(const u64 serial)
{
    if (IsComplete(serial))
    {
        return;
    }

    ORBIT_PROFILE_SCOPE("vk.host_hazard_wait");
    const auto started = std::chrono::steady_clock::now();

    VkSemaphoreWaitInfo waitInfo{};
    waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    waitInfo.semaphoreCount = 1;
    waitInfo.pSemaphores = &timeline_;
    waitInfo.pValues = &serial;

    if (vkWaitSemaphores(device_, &waitInfo, UINT64_MAX) != VK_SUCCESS)
    {
        throw std::runtime_error(
            "Orbit failed while waiting for the Vulkan submit timeline.");
    }

    RaiseTo(completed_, serial);
    hazardWaits_.fetch_add(1, std::memory_order_relaxed);
    hazardWaitMicroseconds_.fetch_add(
        static_cast<u64>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started)
                .count()),
        std::memory_order_relaxed);
}

void GpuProgress::SignalFromHost(const u64 serial) noexcept
{
    VkSemaphoreSignalInfo signalInfo{};
    signalInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
    signalInfo.semaphore = timeline_;
    signalInfo.value = serial;
    vkSignalSemaphore(device_, &signalInfo);
}

void GpuProgress::Retire(const u64 serial, std::function<void()> destroy)
{
    if (IsComplete(serial))
    {
        destroy();
        return;
    }

    {
        const std::lock_guard<std::mutex> lock(retireMutex_);
        retired_.push_back({serial, std::move(destroy)});
        retiredCount_.store(retired_.size(), std::memory_order_release);
    }
}

void GpuProgress::Collect()
{
    if (retiredCount_.load(std::memory_order_acquire) == 0U)
    {
        return;
    }

    RefreshCompleted();
    const u64 completed = completed_.load(std::memory_order_acquire);

    std::vector<Retired> ready;
    {
        const std::lock_guard<std::mutex> lock(retireMutex_);
        const auto split = std::stable_partition(
            retired_.begin(),
            retired_.end(),
            [completed](const Retired& item)
            { return item.serial > completed; });
        ready.assign(
            std::make_move_iterator(split),
            std::make_move_iterator(retired_.end()));
        retired_.erase(split, retired_.end());
        retiredCount_.store(retired_.size(), std::memory_order_release);
    }

    for (auto& item : ready)
    {
        item.destroy();
    }
}

void GpuProgress::Flush()
{
    std::vector<Retired> all;
    {
        const std::lock_guard<std::mutex> lock(retireMutex_);
        all.swap(retired_);
        retiredCount_.store(0, std::memory_order_release);
    }

    for (auto& item : all)
    {
        item.destroy();
    }
}

u64 GpuProgress::HazardWaitCount() const noexcept
{
    return hazardWaits_.load(std::memory_order_relaxed);
}

f64 GpuProgress::HazardWaitMilliseconds() const noexcept
{
    return static_cast<f64>(
               hazardWaitMicroseconds_.load(std::memory_order_relaxed)) /
           1000.0;
}
} // namespace orbit::rhi::vulkan::detail
