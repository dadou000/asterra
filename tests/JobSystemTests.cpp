#include <orbit/jobs/JobSystem.hpp>

#include <atomic>
#include <iostream>
#include <stdexcept>

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

    return 0;
}
