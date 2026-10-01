#pragma once

#include <orbit/core/Types.hpp>

#include <concepts>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace orbit::jobs
{
namespace detail
{
struct JobGroupState;
}

using Job =
    std::move_only_function<void()>;

enum class JobPriority : u8
{
    High,
    Normal,
    Low
};

struct JobSystemTelemetry
{
    u32 workers{0};
    u64 outstanding{0};
    u64 queued{0};
    u64 running{0};
};

class JobGroup
{
public:
    JobGroup();
    ~JobGroup();

    JobGroup(const JobGroup&) = delete;
    JobGroup& operator=(
        const JobGroup&) = delete;

    JobGroup(JobGroup&&) noexcept;
    JobGroup& operator=(
        JobGroup&&) noexcept;

    [[nodiscard]] bool
    IsComplete() const noexcept;

private:
    std::shared_ptr<
        detail::JobGroupState>
        state_;

    friend class JobSystem;
};

// Worker count for a background pool sized as a share of the machine. Several
// pools share one process, so each takes hardware_threads / divisor (at least
// `minimum`) instead of all of them. The environment variable, when set to a
// positive integer, overrides the computed value (for tuning and profiling).
[[nodiscard]] u32 PoolWorkerCount(
    const char* environmentVariable,
    u32 divisor,
    u32 minimum = 2U);

class JobSystem
{
public:
    // workerCount 0 means one worker per hardware thread minus one. `name`
    // labels the pool's threads ("Orbit.<name>.<index>") so they can be told
    // apart in a debugger or profiler.
    explicit JobSystem(
        u32 workerCount = 0,
        std::string name = "Jobs");

    ~JobSystem();

    JobSystem(
        const JobSystem&) = delete;
    JobSystem& operator=(
        const JobSystem&) = delete;
    JobSystem(
        JobSystem&&) = delete;
    JobSystem& operator=(
        JobSystem&&) = delete;

    void Submit(Job job);

    void Submit(
        JobPriority priority,
        Job job);

    void Submit(
        JobGroup& group,
        Job job);

    void Submit(
        JobGroup& group,
        JobPriority priority,
        Job job);

    template <typename Callable>
    requires std::invocable<Callable&>
    void Submit(
        Callable&& callable)
    {
        Submit(
            Job(
                std::forward<
                    Callable>(
                        callable)));
    }

    template <typename Callable>
    requires std::invocable<Callable&>
    void Submit(
        const JobPriority priority,
        Callable&& callable)
    {
        Submit(
            priority,
            Job(
                std::forward<
                    Callable>(
                        callable)));
    }

    template <typename Callable>
    requires std::invocable<Callable&>
    void Submit(
        JobGroup& group,
        Callable&& callable)
    {
        Submit(
            group,
            Job(
                std::forward<
                    Callable>(
                        callable)));
    }

    template <typename Callable>
    requires std::invocable<Callable&>
    void Submit(
        JobGroup& group,
        const JobPriority priority,
        Callable&& callable)
    {
        Submit(
            group,
            priority,
            Job(
                std::forward<
                    Callable>(
                        callable)));
    }

    void Wait(JobGroup& group);
    void WaitIdle();

    [[nodiscard]] u32
    WorkerCount() const noexcept;

    // Lock-free approximate snapshot suitable for UI/profiling. `outstanding`
    // includes queued + running work; queued is derived from those atomics and
    // may differ by one while a worker transitions between states.
    [[nodiscard]] JobSystemTelemetry
    Telemetry() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbit::jobs
