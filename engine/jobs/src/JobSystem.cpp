#include <orbit/jobs/JobSystem.hpp>

#include <orbit/core/Log.hpp>
#include <orbit/core/ThreadName.hpp>
#include <orbit/profiler/Profiler.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <exception>
#include <format>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace orbit::jobs
{
namespace detail
{
struct JobGroupState
{
    std::atomic<u64> remaining{0};
    std::mutex mutex;
    std::condition_variable condition;
    std::exception_ptr firstException;
};
} // namespace detail

namespace
{
[[nodiscard]] constexpr std::size_t
PriorityIndex(
    const JobPriority priority) noexcept
{
    switch (priority)
    {
    case JobPriority::High:
        return 0;
    case JobPriority::Normal:
        return 1;
    case JobPriority::Low:
        return 2;
    }

    return 1;
}

constexpr std::array<JobPriority, 3> kPriorityOrder{
    JobPriority::High,
    JobPriority::Normal,
    JobPriority::Low
};
} // namespace

JobGroup::JobGroup()
    : state_(std::make_shared<detail::JobGroupState>())
{
}

JobGroup::~JobGroup() = default;

JobGroup::JobGroup(JobGroup&&) noexcept = default;

JobGroup& JobGroup::operator=(JobGroup&&) noexcept = default;

bool JobGroup::IsComplete() const noexcept
{
    return !state_ ||
        state_->remaining.load(std::memory_order_acquire) == 0;
}

u32 PoolWorkerCount(
    const char* const environmentVariable,
    const u32 divisor,
    const u32 minimum)
{
    if (environmentVariable != nullptr)
    {
        std::string value;
#if defined(_WIN32)
        char* buffer = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&buffer, &length, environmentVariable) == 0 &&
            buffer != nullptr)
        {
            value = buffer;
            std::free(buffer);
        }
#else
        if (const char* found = std::getenv(environmentVariable))
        {
            value = found;
        }
#endif
        if (!value.empty())
        {
            u32 parsed = 0;
            const auto [end, error] = std::from_chars(
                value.data(),
                value.data() + value.size(),
                parsed);
            if (error == std::errc{} && end != value.data() && parsed > 0U)
            {
                return parsed;
            }
        }
    }

    const u32 hardware = std::max(std::thread::hardware_concurrency(), 1U);
    return std::max(hardware / std::max(divisor, 1U), minimum);
}

class JobSystem::Impl
{
public:
    Impl(u32 workerCount, std::string name)
        : name_(std::move(name))
        , jobScopeName_(profiler::Intern(name_ + ".job"))
    {
        if (workerCount == 0)
        {
            const u32 hardwareThreads =
                std::thread::hardware_concurrency();

            workerCount = hardwareThreads > 1
                ? hardwareThreads - 1
                : 1;
        }

        workers_.reserve(workerCount);
        for (u32 index = 0; index < workerCount; ++index)
        {
            workers_.push_back(std::make_unique<Worker>());
        }

        try
        {
            for (u32 index = 0; index < workerCount; ++index)
            {
                workers_[index]->thread =
                    std::thread([this, index]
                    {
                        WorkerLoop(index);
                    });
            }
        }
        catch (...)
        {
            StopWorkers();
            throw;
        }
    }

    ~Impl()
    {
        WaitIdle();
        StopWorkers();
    }

    void Submit(
        const JobPriority priority,
        Job job)
    {
        if (!job)
        {
            throw std::invalid_argument(
                "Orbit cannot submit an empty job.");
        }

        Enqueue({
            .function = std::move(job),
            .group = {},
            .priority = priority
        });
    }

    void Submit(
        const std::shared_ptr<detail::JobGroupState>& group,
        const JobPriority priority,
        Job job)
    {
        if (!group)
        {
            throw std::invalid_argument(
                "Orbit cannot submit to a moved-from job group.");
        }

        if (!job)
        {
            throw std::invalid_argument(
                "Orbit cannot submit an empty job.");
        }

        group->remaining.fetch_add(1, std::memory_order_acq_rel);

        try
        {
            Enqueue({
                .function = std::move(job),
                .group = group,
                .priority = priority
            });
        }
        catch (...)
        {
            group->remaining.fetch_sub(1, std::memory_order_acq_rel);
            throw;
        }
    }

    void Wait(
        const std::shared_ptr<detail::JobGroupState>& group)
    {
        if (!group)
        {
            return;
        }

        while (group->remaining.load(std::memory_order_acquire) != 0)
        {
            if (TryExecuteOne(kExternalThread))
            {
                continue;
            }

            std::unique_lock lock(group->mutex);
            group->condition.wait_for(
                lock,
                std::chrono::milliseconds(1),
                [&group]
                {
                    return group->remaining.load(
                        std::memory_order_acquire) == 0;
                });
        }

        std::exception_ptr exception;
        {
            std::scoped_lock lock(group->mutex);
            exception = group->firstException;
            group->firstException = nullptr;
        }

        if (exception)
        {
            std::rethrow_exception(exception);
        }
    }

    void WaitIdle()
    {
        while (outstandingJobs_.load(std::memory_order_acquire) != 0)
        {
            if (TryExecuteOne(kExternalThread))
            {
                continue;
            }

            std::unique_lock lock(wakeMutex_);
            wakeCondition_.wait_for(
                lock,
                std::chrono::milliseconds(1),
                [this]
                {
                    return
                        outstandingJobs_.load(
                            std::memory_order_acquire) == 0 ||
                        stopping_.load(
                            std::memory_order_acquire);
                });
        }
    }

    [[nodiscard]] u32 WorkerCount() const noexcept
    {
        return static_cast<u32>(workers_.size());
    }

    [[nodiscard]] JobSystemTelemetry Telemetry() const noexcept
    {
        const u64 outstanding =
            outstandingJobs_.load(std::memory_order_acquire);
        const u64 running =
            runningJobs_.load(std::memory_order_acquire);

        return {
            .workers = static_cast<u32>(workers_.size()),
            .outstanding = outstanding,
            .queued = outstanding > running
                ? outstanding - running
                : 0,
            .running = running
        };
    }

private:
    struct JobItem
    {
        Job function;
        std::shared_ptr<detail::JobGroupState> group;
        JobPriority priority{JobPriority::Normal};
    };

    struct Worker
    {
        std::mutex mutex;
        std::array<std::deque<JobItem>, 3> queues;
        std::thread thread;
    };

    static constexpr u32 kExternalThread =
        std::numeric_limits<u32>::max();

    void StopWorkers()
    {
        {
            std::scoped_lock lock(wakeMutex_);
            stopping_.store(true, std::memory_order_release);
        }

        wakeCondition_.notify_all();
        for (auto& worker : workers_)
        {
            if (worker->thread.joinable())
            {
                worker->thread.join();
            }
        }
    }

    void Enqueue(JobItem item)
    {
        const u64 ticket =
            nextWorker_.fetch_add(1, std::memory_order_relaxed);
        const u32 workerIndex = static_cast<u32>(
            ticket % static_cast<u64>(workers_.size()));

        {
            Worker& worker = *workers_[workerIndex];
            std::scoped_lock lock(worker.mutex);
            worker.queues[PriorityIndex(item.priority)].push_back(
                std::move(item));
            outstandingJobs_.fetch_add(
                1,
                std::memory_order_release);
        }

        {
            // Publish the wake generation under the condition-variable mutex
            // so an enqueue between an empty scan and sleep cannot be lost.
            std::scoped_lock lock(wakeMutex_);
            ++workGeneration_;
        }
        wakeCondition_.notify_one();
    }

    bool TryTakeOwn(
        const u32 workerIndex,
        const JobPriority priority,
        JobItem& item)
    {
        if (workerIndex >= workers_.size())
        {
            return false;
        }

        Worker& worker = *workers_[workerIndex];
        std::scoped_lock lock(worker.mutex);
        auto& queue = worker.queues[PriorityIndex(priority)];
        if (queue.empty())
        {
            return false;
        }

        item = std::move(queue.back());
        queue.pop_back();
        return true;
    }

    bool TrySteal(
        const u32 victimIndex,
        const JobPriority priority,
        JobItem& item)
    {
        Worker& worker = *workers_[victimIndex];
        std::scoped_lock lock(worker.mutex);
        auto& queue = worker.queues[PriorityIndex(priority)];
        if (queue.empty())
        {
            return false;
        }

        item = std::move(queue.front());
        queue.pop_front();
        return true;
    }

    bool TryExecuteOne(const u32 workerIndex)
    {
        JobItem item{};
        for (const JobPriority priority : kPriorityOrder)
        {
            if (workerIndex != kExternalThread &&
                TryTakeOwn(workerIndex, priority, item))
            {
                Execute(std::move(item));
                return true;
            }

            for (u32 victim = 0;
                 victim < static_cast<u32>(workers_.size());
                 ++victim)
            {
                if (victim == workerIndex)
                {
                    continue;
                }

                if (TrySteal(victim, priority, item))
                {
                    Execute(std::move(item));
                    return true;
                }
            }
        }

        return false;
    }

    void Execute(JobItem item)
    {
        runningJobs_.fetch_add(1, std::memory_order_acq_rel);

        try
        {
            // One scope per job, named after the pool, so a capture shows every
            // job on every worker. Jobs that want a finer label nest their own.
            ORBIT_PROFILE_SCOPE(jobScopeName_);
            item.function();
        }
        catch (...)
        {
            if (item.group)
            {
                std::scoped_lock lock(item.group->mutex);
                if (!item.group->firstException)
                {
                    item.group->firstException =
                        std::current_exception();
                }
            }
            else
            {
                log::Error(
                    "Unhandled exception in a fire-and-forget Orbit job.");
            }
        }

        if (item.group)
        {
            const u64 previous =
                item.group->remaining.fetch_sub(
                    1,
                    std::memory_order_acq_rel);
            if (previous == 1)
            {
                item.group->condition.notify_all();
            }
        }

        runningJobs_.fetch_sub(1, std::memory_order_acq_rel);
        const u64 previousOutstanding =
            outstandingJobs_.fetch_sub(
                1,
                std::memory_order_acq_rel);

        if (previousOutstanding == 1)
        {
            wakeCondition_.notify_all();
        }
    }

    void WorkerLoop(const u32 workerIndex)
    {
        core::SetCurrentThreadName(
            std::format("Orbit.{}.{}", name_, workerIndex));

        while (true)
        {
            u64 observedGeneration;
            {
                std::scoped_lock lock(wakeMutex_);
                observedGeneration = workGeneration_;
            }

            if (TryExecuteOne(workerIndex))
            {
                continue;
            }

            if (stopping_.load(std::memory_order_acquire) &&
                outstandingJobs_.load(std::memory_order_acquire) == 0)
            {
                return;
            }

            std::unique_lock lock(wakeMutex_);
            wakeCondition_.wait(
                lock,
                [this, observedGeneration]
                {
                    return stopping_.load(std::memory_order_acquire) ||
                        workGeneration_ != observedGeneration;
                });
        }
    }

    std::string name_;
    const char* jobScopeName_{nullptr};
    std::vector<std::unique_ptr<Worker>> workers_;
    std::atomic<u64> outstandingJobs_{0};
    std::atomic<u64> runningJobs_{0};
    std::atomic<u64> nextWorker_{0};
    std::atomic<bool> stopping_{false};
    std::mutex wakeMutex_;
    u64 workGeneration_{0};
    std::condition_variable wakeCondition_;
};

JobSystem::JobSystem(const u32 workerCount, std::string name)
    : impl_(std::make_unique<Impl>(workerCount, std::move(name)))
{
}

JobSystem::~JobSystem() = default;

void JobSystem::Submit(Job job)
{
    impl_->Submit(JobPriority::Normal, std::move(job));
}

void JobSystem::Submit(
    const JobPriority priority,
    Job job)
{
    impl_->Submit(priority, std::move(job));
}

void JobSystem::Submit(
    JobGroup& group,
    Job job)
{
    impl_->Submit(
        group.state_,
        JobPriority::Normal,
        std::move(job));
}

void JobSystem::Submit(
    JobGroup& group,
    const JobPriority priority,
    Job job)
{
    impl_->Submit(
        group.state_,
        priority,
        std::move(job));
}

void JobSystem::Wait(JobGroup& group)
{
    impl_->Wait(group.state_);
}

void JobSystem::WaitIdle()
{
    impl_->WaitIdle();
}

u32 JobSystem::WorkerCount() const noexcept
{
    return impl_->WorkerCount();
}

JobSystemTelemetry JobSystem::Telemetry() const noexcept
{
    return impl_->Telemetry();
}
} // namespace orbit::jobs
