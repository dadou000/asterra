#include <orbit/jobs/JobSystem.hpp>

#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>

int main()
{
    orbit::jobs::JobSystem jobs(4);

    if (jobs.WorkerCount() != 4)
    {
        std::cerr << "Unexpected Orbit worker count.\n";
        return 1;
    }

    std::atomic<orbit::u32> completed{0};
    orbit::jobs::JobGroup bulkGroup;

    constexpr orbit::u32 jobCount = 10'000;

    for (orbit::u32 index = 0; index < jobCount; ++index)
    {
        jobs.Submit(
            bulkGroup,
            [&completed]
            {
                completed.fetch_add(1, std::memory_order_relaxed);
            });
    }

    jobs.Wait(bulkGroup);

    if (completed.load(std::memory_order_relaxed) != jobCount ||
        !bulkGroup.IsComplete())
    {
        std::cerr << "Orbit bulk jobs did not complete correctly.\n";
        return 1;
    }

    std::atomic<orbit::u32> nestedCompleted{0};
    orbit::jobs::JobGroup nestedGroup;

    jobs.Submit(
        nestedGroup,
        [&jobs, &nestedGroup, &nestedCompleted]
        {
            constexpr orbit::u32 nestedCount = 512;

            for (orbit::u32 index = 0; index < nestedCount; ++index)
            {
                jobs.Submit(
                    nestedGroup,
                    [&nestedCompleted]
                    {
                        nestedCompleted.fetch_add(
                            1,
                            std::memory_order_relaxed);
                    });
            }
        });

    jobs.Wait(nestedGroup);

    if (nestedCompleted.load(std::memory_order_relaxed) != 512)
    {
        std::cerr << "Orbit nested jobs did not complete correctly.\n";
        return 1;
    }

    orbit::jobs::JobGroup exceptionGroup;

    jobs.Submit(
        exceptionGroup,
        []
        {
            throw std::runtime_error("expected test exception");
        });

    bool exceptionObserved = false;

    try
    {
        jobs.Wait(exceptionGroup);
    }
    catch (const std::runtime_error&)
    {
        exceptionObserved = true;
    }

    if (!exceptionObserved)
    {
        std::cerr << "Orbit job exceptions were not propagated.\n";
        return 1;
    }

    std::atomic<orbit::u32> fireAndForget{0};

    for (orbit::u32 index = 0; index < 1'000; ++index)
    {
        jobs.Submit(
            [&fireAndForget]
            {
                fireAndForget.fetch_add(
                    1,
                    std::memory_order_relaxed);
            });
    }

    jobs.WaitIdle();

    if (fireAndForget.load(std::memory_order_relaxed) != 1'000)
    {
        std::cerr << "Orbit WaitIdle returned too early.\n";
        return 1;
    }

    orbit::jobs::JobSystem priorityJobs(1);

    std::atomic<bool> blockerStarted{false};
    std::atomic<bool> releaseBlocker{false};

    priorityJobs.Submit(
        orbit::jobs::JobPriority::Normal,
        [&blockerStarted, &releaseBlocker]
        {
            blockerStarted.store(
                true,
                std::memory_order_release);

            while (!releaseBlocker.load(
                std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
        });

    while (!blockerStarted.load(
        std::memory_order_acquire))
    {
        std::this_thread::yield();
    }

    orbit::jobs::JobGroup priorityGroup;

    std::atomic<orbit::u32>
        executionOrder{0};

    std::atomic<orbit::u32>
        highOrder{99};

    std::atomic<orbit::u32>
        lowOrder{99};

    priorityJobs.Submit(
        priorityGroup,
        orbit::jobs::JobPriority::Low,
        [&executionOrder, &lowOrder]
        {
            lowOrder.store(
                executionOrder.fetch_add(
                    1,
                    std::memory_order_relaxed),
                std::memory_order_relaxed);
        });

    priorityJobs.Submit(
        priorityGroup,
        orbit::jobs::JobPriority::High,
        [&executionOrder, &highOrder]
        {
            highOrder.store(
                executionOrder.fetch_add(
                    1,
                    std::memory_order_relaxed),
                std::memory_order_relaxed);
        });

    releaseBlocker.store(
        true,
        std::memory_order_release);

    priorityJobs.Wait(priorityGroup);
    priorityJobs.WaitIdle();

    if (highOrder.load(
            std::memory_order_relaxed) != 0 ||
        lowOrder.load(
            std::memory_order_relaxed) != 1)
    {
        std::cerr
            << "Orbit job priorities were not respected.\n";
        return 1;
    }

    return 0;
}
