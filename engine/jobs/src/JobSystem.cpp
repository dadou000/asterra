#include <orbit/jobs/JobSystem.hpp>

#include <orbit/core/Log.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
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

constexpr std::array<
    JobPriority,
    3>
    kPriorityOrder{
        JobPriority::High,
        JobPriority::Normal,
        JobPriority::Low
    };
} // namespace

JobGroup::JobGroup()
    : state_(
        std::make_shared<
            detail::JobGroupState>())
{
}

JobGroup::~JobGroup() = default;

JobGroup::JobGroup(
    JobGroup&&) noexcept = default;

JobGroup& JobGroup::operator=(
    JobGroup&&) noexcept = default;

bool JobGroup::IsComplete() const noexcept
{
    return !state_ ||
        state_->remaining.load(
            std::memory_order_acquire) ==
            0;
}

class JobSystem::Impl
{
public:
    explicit Impl(u32 workerCount)
    {
        if (workerCount == 0)
        {
            const u32 hardwareThreads =
                std::thread::
                    hardware_concurrency();

            workerCount =
                hardwareThreads > 1
                    ? hardwareThreads - 1
                    : 1;
        }

        workers_.reserve(workerCount);

        for (u32 index = 0;
             index < workerCount;
             ++index)
        {
            workers_.push_back(
                std::make_unique<
                    Worker>());
        }

        for (u32 index = 0;
             index < workerCount;
             ++index)
        {
            workers_[index]->thread =
                std::thread(
                    [this, index]
                    {
                        WorkerLoop(
                            index);
                    });
        }
    }

    ~Impl()
    {
        WaitIdle();

        stopping_.store(
            true,
            std::memory_order_release);

        wakeCondition_.
            notify_all();

        for (auto& worker :
             workers_)
        {
            if (worker->thread.
                    joinable())
            {
                worker->thread.join();
            }
        }
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
            .function =
                std::move(job),
            .group = {},
            .priority = priority
        });
    }

    void Submit(
        const std::shared_ptr<
            detail::JobGroupState>&
            group,
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

        group->remaining.
            fetch_add(
                1,
                std::memory_order_acq_rel);

        try
        {
            Enqueue({
                .function =
                    std::move(job),
                .group = group,
                .priority = priority
            });
        }
        catch (...)
        {
            group->remaining.
                fetch_sub(
                    1,
                    std::memory_order_acq_rel);

            throw;
        }
    }

    void Wait(
        const std::shared_ptr<
            detail::JobGroupState>&
            group)
    {
        if (!group)
        {
            return;
        }

        while (group->remaining.load(
                   std::memory_order_acquire) !=
               0)
        {
            if (TryExecuteOne(
                    kExternalThread))
            {
                continue;
            }

            std::unique_lock lock(
                group->mutex);

            group->condition.wait_for(
                lock,
                std::chrono::
                    milliseconds(1),
                [&group]
                {
                    return group->
                               remaining.load(
                                   std::memory_order_acquire) ==
                        0;
                });
        }

        std::exception_ptr exception;

        {
            std::scoped_lock lock(
                group->mutex);

            exception =
                group->firstException;

            group->firstException =
                nullptr;
        }

        if (exception)
        {
            std::rethrow_exception(
                exception);
        }
    }

    void WaitIdle()
    {
        while (outstandingJobs_.load(
                   std::memory_order_acquire) !=
               0)
        {
            if (TryExecuteOne(
                    kExternalThread))
            {
                continue;
            }

            std::unique_lock lock(
                wakeMutex_);

            wakeCondition_.wait_for(
                lock,
                std::chrono::
                    milliseconds(1),
                [this]
                {
                    return
                        outstandingJobs_.
                                load(
                                    std::memory_order_acquire) ==
                            0 ||
                        stopping_.
                            load(
                                std::memory_order_acquire);
                });
        }
    }

    [[nodiscard]] u32
    WorkerCount() const noexcept
    {
        return static_cast<u32>(
            workers_.size());
    }

private:
    struct JobItem
    {
        Job function;
        std::shared_ptr<
            detail::JobGroupState>
            group;

        JobPriority priority{
            JobPriority::Normal};
    };

    struct Worker
    {
        std::mutex mutex;

        std::array<
            std::deque<JobItem>,
            3>
            queues;

        std::thread thread;
    };

    static constexpr u32
        kExternalThread =
            std::numeric_limits<u32>::
                max();

    void Enqueue(JobItem item)
    {
        const u64 ticket =
            nextWorker_.
                fetch_add(
                    1,
                    std::memory_order_relaxed);

        const u32 workerIndex =
            static_cast<u32>(
                ticket %
                static_cast<u64>(
                    workers_.size()));

        {
            Worker& worker =
                *workers_[workerIndex];

            std::scoped_lock lock(
                worker.mutex);

            worker.queues[
                PriorityIndex(
                    item.priority)].
                push_back(
                    std::move(item));

            outstandingJobs_.
                fetch_add(
                    1,
                    std::memory_order_release);
        }

        wakeCondition_.notify_one();
    }

    bool TryTakeOwn(
        const u32 workerIndex,
        JobItem& item)
    {
        if (workerIndex >=
            workers_.size())
        {
            return false;
        }

        Worker& worker =
            *workers_[workerIndex];

        std::scoped_lock lock(
            worker.mutex);

        for (const JobPriority priority :
             kPriorityOrder)
        {
            auto& queue =
                worker.queues[
                    PriorityIndex(
                        priority)];

            if (queue.empty())
            {
                continue;
            }

            item =
                std::move(
                    queue.back());

            queue.pop_back();
            return true;
        }

        return false;
    }

    bool TrySteal(
        const u32 victimIndex,
        JobItem& item)
    {
        Worker& worker =
            *workers_[victimIndex];

        std::scoped_lock lock(
            worker.mutex);

        for (const JobPriority priority :
             kPriorityOrder)
        {
            auto& queue =
                worker.queues[
                    PriorityIndex(
                        priority)];

            if (queue.empty())
            {
                continue;
            }

            item =
                std::move(
                    queue.front());

            queue.pop_front();
            return true;
        }

        return false;
    }

    bool TryExecuteOne(
        const u32 workerIndex)
    {
        JobItem item{};

        if (workerIndex !=
                kExternalThread &&
            TryTakeOwn(
                workerIndex,
                item))
        {
            Execute(
                std::move(item));

            return true;
        }

        for (u32 victim = 0;
             victim <
                static_cast<u32>(
                    workers_.size());
             ++victim)
        {
            if (victim ==
                workerIndex)
            {
                continue;
            }

            if (TrySteal(
                    victim,
                    item))
            {
                Execute(
                    std::move(item));

                return true;
            }
        }

        return false;
    }

    void Execute(
        JobItem item)
    {
        try
        {
            item.function();
        }
        catch (...)
        {
            if (item.group)
            {
                std::scoped_lock lock(
                    item.group->mutex);

                if (!item.group->
                        firstException)
                {
                    item.group->
                        firstException =
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
                item.group->remaining.
                    fetch_sub(
                        1,
                        std::memory_order_acq_rel);

            if (previous == 1)
            {
                item.group->
                    condition.
                    notify_all();
            }
        }

        const u64 previousOutstanding =
            outstandingJobs_.
                fetch_sub(
                    1,
                    std::memory_order_acq_rel);

        if (previousOutstanding == 1)
        {
            wakeCondition_.
                notify_all();
        }
    }

    void WorkerLoop(
        const u32 workerIndex)
    {
        while (true)
        {
            if (TryExecuteOne(
                    workerIndex))
            {
                continue;
            }

            if (stopping_.load(
                    std::memory_order_acquire) &&
                outstandingJobs_.load(
                    std::memory_order_acquire) ==
                    0)
            {
                return;
            }

            std::unique_lock lock(
                wakeMutex_);

            wakeCondition_.wait_for(
                lock,
                std::chrono::
                    milliseconds(1));
        }
    }

    std::vector<
        std::unique_ptr<Worker>>
        workers_;

    std::atomic<u64>
        outstandingJobs_{0};

    std::atomic<u64>
        nextWorker_{0};

    std::atomic<bool>
        stopping_{false};

    std::mutex wakeMutex_;
    std::condition_variable
        wakeCondition_;
};

JobSystem::JobSystem(
    const u32 workerCount)
    : impl_(
        std::make_unique<Impl>(
            workerCount))
{
}

JobSystem::~JobSystem() = default;

void JobSystem::Submit(
    Job job)
{
    impl_->Submit(
        JobPriority::Normal,
        std::move(job));
}

void JobSystem::Submit(
    const JobPriority priority,
    Job job)
{
    impl_->Submit(
        priority,
        std::move(job));
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

void JobSystem::Wait(
    JobGroup& group)
{
    impl_->Wait(
        group.state_);
}

void JobSystem::WaitIdle()
{
    impl_->WaitIdle();
}

u32 JobSystem::WorkerCount()
    const noexcept
{
    return impl_->WorkerCount();
}
} // namespace orbit::jobs
