#pragma once

#include <orbit/core/Types.hpp>

#include <concepts>
#include <functional>
#include <memory>
#include <utility>

namespace orbit::jobs
{
namespace detail
{
struct JobGroupState;
}

using Job = std::move_only_function<void()>;

class JobGroup
{
public:
    JobGroup();
    ~JobGroup();

    JobGroup(const JobGroup&) = delete;
    JobGroup& operator=(const JobGroup&) = delete;
    JobGroup(JobGroup&&) noexcept;
    JobGroup& operator=(JobGroup&&) noexcept;

    [[nodiscard]] bool IsComplete() const noexcept;

private:
    std::shared_ptr<detail::JobGroupState> state_;

    friend class JobSystem;
};

class JobSystem
{
public:
    explicit JobSystem(u32 workerCount = 0);
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;
    JobSystem(JobSystem&&) = delete;
    JobSystem& operator=(JobSystem&&) = delete;

    void Submit(Job job);
    void Submit(JobGroup& group, Job job);

    template <typename Callable>
    requires std::invocable<Callable&>
    void Submit(Callable&& callable)
    {
        Submit(Job(std::forward<Callable>(callable)));
    }

    template <typename Callable>
    requires std::invocable<Callable&>
    void Submit(JobGroup& group, Callable&& callable)
    {
        Submit(group, Job(std::forward<Callable>(callable)));
    }

    void Wait(JobGroup& group);
    void WaitIdle();

    [[nodiscard]] u32 WorkerCount() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbit::jobs
