#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <vulkan/vulkan.h>

#include <orbit/core/Types.hpp>

#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

namespace orbit::rhi::vulkan::detail
{
// Tracks how far the single graphics queue has progressed so the CPU can run
// ahead of the GPU without a blanket vkQueueWaitIdle after every submit.
//
// Every VulkanQueue::Submit signals one timeline value (its "serial"). Anything
// the CPU later touches that the GPU may still be using is protected by a
// precise per-object wait on that serial:
//   - host-visible Buffer::Map waits for the last submit that used the buffer
//     (VulkanBuffer::lastUse_), so a ring-buffered upload never stalls and a
//     single-buffered one stalls exactly as long as it has to;
//   - CommandAllocator::Reset waits for the last submit recorded from it;
//   - object destruction is deferred (Retire) until the submit that could still
//     reference the object has completed, without blocking the CPU.
// Setting ORBIT_RHI_SYNC_SUBMIT=1 restores the old idle-after-submit behaviour
// for debugging a suspected CPU/GPU lifetime bug.
class GpuProgress
{
public:
    explicit GpuProgress(VkDevice device);
    ~GpuProgress();

    GpuProgress(const GpuProgress&) = delete;
    GpuProgress& operator=(const GpuProgress&) = delete;

    [[nodiscard]] VkSemaphore Timeline() const noexcept;

    // Allocates the serial the submit about to be issued will signal.
    [[nodiscard]] u64 BeginSubmit() noexcept;

    // Serial of the next submit; the conservative "could still reference it"
    // bound for anything destroyed while commands are being recorded.
    [[nodiscard]] u64 NextSerial() const noexcept;

    [[nodiscard]] bool IsComplete(u64 serial) noexcept;

    // Blocks until the timeline reaches serial. Counts toward hazard stats.
    void Wait(u64 serial);

    // Signals a serial from the host so waiters never hang on a submit that
    // failed after its serial was allocated.
    void SignalFromHost(u64 serial) noexcept;

    // Runs destroy now if serial is complete, otherwise when it completes.
    void Retire(u64 serial, std::function<void()> destroy);

    // Runs the deferred destroys whose serial has completed.
    void Collect();

    // Device must be idle: runs every deferred destroy.
    void Flush();

    [[nodiscard]] u64 HazardWaitCount() const noexcept;
    [[nodiscard]] f64 HazardWaitMilliseconds() const noexcept;

private:
    struct Retired
    {
        u64 serial{0};
        std::function<void()> destroy;
    };

    void RefreshCompleted() noexcept;

    VkDevice device_{VK_NULL_HANDLE};
    VkSemaphore timeline_{VK_NULL_HANDLE};
    std::atomic<u64> submitted_{0};
    std::atomic<u64> completed_{0};

    std::mutex retireMutex_;
    std::vector<Retired> retired_;
    std::atomic<std::size_t> retiredCount_{0};

    std::atomic<u64> hazardWaits_{0};
    std::atomic<u64> hazardWaitMicroseconds_{0};
};

// Raises value to at least minimum without ever lowering it.
inline void RaiseTo(std::atomic<u64>& value, const u64 minimum) noexcept
{
    u64 current = value.load(std::memory_order_relaxed);
    while (current < minimum &&
           !value.compare_exchange_weak(
               current, minimum, std::memory_order_release))
    {
    }
}
} // namespace orbit::rhi::vulkan::detail
