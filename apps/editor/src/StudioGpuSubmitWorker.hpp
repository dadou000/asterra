#pragma once

#include <orbit/core/Types.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace orbit::editor_app
{
// The producer owns command lists, swapchain images and render-graph resources.
// A SubmissionPacket is an immutable, versioned handoff; the producer may not
// touch these objects until Poll() acknowledges the submission. This is the
// graphics submission stage, NOT a snapshot of StudioSession/ImGui.
struct SubmissionPacket
{
    std::function<void()> execute;
    u64 fenceValue{0U};
    u32 frameSlot{0U};
};

struct SubmissionResult
{
    u64 fenceValue{0U};
    u32 frameSlot{0U};
    double workerMilliseconds{0.0};
};

class StudioGpuSubmitWorker final
{
public:
    explicit StudioGpuSubmitWorker(bool enabled);
    ~StudioGpuSubmitWorker() noexcept;

    StudioGpuSubmitWorker(const StudioGpuSubmitWorker&) = delete;
    StudioGpuSubmitWorker& operator=(const StudioGpuSubmitWorker&) = delete;

    // Nonblocking single-slot dispatch. Throws if a previous packet has not
    // been acknowledged; no command allocator/back buffer may be reused then.
    void Dispatch(SubmissionPacket packet);

    // Returns false while a packet remains in flight. Returns true otherwise,
    // delivering any completed result through the pointer and rethrowing
    // worker failures on the owning/main thread.
    [[nodiscard]] bool Poll(std::optional<SubmissionResult>* result = nullptr);

    // Explicit lifecycle barrier for shutdown/resize/capture. Never use on
    // the ordinary frame path.
    void Drain();

    [[nodiscard]] bool Enabled() const noexcept { return enabled_; }

private:
    void Run() noexcept;

    bool enabled_{false};
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<SubmissionPacket> queued_;
    std::optional<SubmissionResult> completed_;
    std::exception_ptr failed_;
    bool inFlight_{false};
    bool stopping_{false};
};
} // namespace orbit::editor_app
