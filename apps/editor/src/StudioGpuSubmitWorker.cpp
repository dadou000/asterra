#include "StudioGpuSubmitWorker.hpp"

#include <orbit/core/ThreadName.hpp>

#include <stdexcept>
#include <utility>

namespace orbit::editor_app
{
StudioGpuSubmitWorker::StudioGpuSubmitWorker(const bool enabled)
    : enabled_(enabled)
{
    if (enabled_)
        worker_ = std::thread([this] { Run(); });
}

StudioGpuSubmitWorker::~StudioGpuSubmitWorker() noexcept
{
    if (!enabled_) return;
    {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] { return !inFlight_; });
        stopping_ = true;
    }
    cv_.notify_one();
    if (worker_.joinable()) worker_.join();
}

void StudioGpuSubmitWorker::Dispatch(SubmissionPacket packet)
{
    if (!packet.execute)
        throw std::invalid_argument("Studio GPU submission packet has no command.");
    if (!enabled_)
    {
        packet.execute();
        return;
    }
    {
        std::scoped_lock lock(mutex_);
        if (inFlight_ || completed_ || failed_ || stopping_)
            throw std::logic_error(
                "Studio GPU submission packet reused before acknowledgment.");
        inFlight_ = true;
        queued_ = std::move(packet);
    }
    cv_.notify_one();
}

bool StudioGpuSubmitWorker::Poll(std::optional<SubmissionResult>* result)
{
    if (!enabled_)
    {
        if (result) result->reset();
        return true;
    }
    std::exception_ptr error;
    {
        std::scoped_lock lock(mutex_);
        if (inFlight_) return false;
        if (result)
            *result = std::move(completed_);
        completed_.reset();
        error = std::exchange(failed_, {});
    }
    if (error) std::rethrow_exception(error);
    return true;
}

void StudioGpuSubmitWorker::Drain()
{
    if (!enabled_) return;
    {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] { return !inFlight_; });
    }
    static_cast<void>(Poll());
}

void StudioGpuSubmitWorker::Run() noexcept
{
    core::SetCurrentThreadName("Orbit.GraphicsSubmit");
    core::SetForegroundThreadPriority();
    for (;;)
    {
        SubmissionPacket packet;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || queued_.has_value(); });
            if (stopping_ && !queued_) return;
            packet = std::move(*queued_);
            queued_.reset();
        }

        const auto started = std::chrono::steady_clock::now();
        std::exception_ptr error;
        try
        {
            packet.execute();
        }
        catch (...)
        {
            error = std::current_exception();
        }
        const auto finished = std::chrono::steady_clock::now();

        {
            std::scoped_lock lock(mutex_);
            completed_ = SubmissionResult{
                packet.fenceValue,
                packet.frameSlot,
                std::chrono::duration<double, std::milli>(
                    finished - started).count()};
            failed_ = error;
            inFlight_ = false;
        }
        cv_.notify_all();
    }
}
} // namespace orbit::editor_app
